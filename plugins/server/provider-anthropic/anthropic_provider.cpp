#include "anthropic_provider.hpp"

#include <exception>

#include "anthropic_error_mapping.hpp"
#include "anthropic_request.hpp"
#include "anthropic_response.hpp"
#include "anthropic_stream_reassembler.hpp"
#include "minimal/llm/sse_parser.hpp"

namespace minimal::provider_anthropic {

namespace {

constexpr const char* kAnthropicVersion = "2023-06-01";

[[noreturn]] void RethrowParseErrorAsLlmError(const AnthropicParseError& error) {
    throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
}

}  // namespace

llm::HttpRequest AnthropicProvider::BuildHttpRequest(std::string body, bool streaming) const {
    llm::HttpRequest request;
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    // Unlike provider-openai's convention, base_url here is expected WITHOUT
    // a "/v1" suffix (the canonical Anthropic base is
    // "https://api.anthropic.com") -- the full endpoint path is "/v1/messages".
    request.url = base + "/v1/messages";
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"anthropic-version", kAnthropicVersion});
    // x-api-key, not Authorization: Bearer -- Anthropic's own auth header.
    if (!config_.api_key.empty()) {
        request.headers.push_back({"x-api-key", config_.api_key});
    }
    request.body = std::move(body);
    request.connect_timeout = config_.connect_timeout;
    request.request_timeout = streaming ? std::chrono::milliseconds(0) : config_.request_timeout;
    request.idle_timeout = streaming ? config_.stream_idle_timeout : std::chrono::milliseconds(0);
    return request;
}

llm::ChatResponse AnthropicProvider::Complete(const llm::ChatRequest& request) {
    llm::HttpRequest http_request = BuildHttpRequest(BuildRequestBody(request, /*stream=*/false),
                                                     /*streaming=*/false);

    llm::HttpResponse http_response;
    try {
        http_response = http_.Post(http_request);
    } catch (const llm::HttpError& error) {
        throw ClassifyTransportFailure(error);
    }

    if (http_response.status < 200 || http_response.status >= 300) {
        throw ClassifyHttpFailure(http_response.status, http_response.body, http_response.headers);
    }

    try {
        return ParseNonStreamingResponse(nlohmann::json::parse(http_response.body));
    } catch (const AnthropicParseError& error) {
        RethrowParseErrorAsLlmError(error);
    } catch (const nlohmann::json::exception& error) {
        throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
    }
    __builtin_unreachable();  // both catch branches above always throw
}

llm::ChatResponse AnthropicProvider::StreamComplete(const llm::ChatRequest& request,
                                                    const llm::StreamEventSink& sink) {
    llm::HttpRequest http_request = BuildHttpRequest(BuildRequestBody(request, /*stream=*/true),
                                                     /*streaming=*/true);

    AnthropicStreamReassembler reassembler;
    llm::SseParser sse_parser;
    int response_status = 0;
    std::vector<llm::HttpHeader> response_headers;
    std::string error_body;
    bool is_error_status = false;
    bool cancelled_by_sink = false;
    bool error_body_capped = false;
    std::exception_ptr pending_error;

    auto on_headers = [&](int status, const std::vector<llm::HttpHeader>& headers) {
        response_status = status;
        response_headers = headers;
        is_error_status = status < 200 || status >= 300;
    };

    // Exceptions must never escape into libcurl's C call stack (undefined
    // behavior) -- caught here, stashed, and rethrown once StreamPost unwinds.
    // Identical shape to provider-openai's on_chunk.
    auto on_chunk = [&](std::string_view chunk) -> bool {
        if (is_error_status) {
            constexpr std::size_t kMaxErrorBody = 64 * 1024;
            if (error_body.size() < kMaxErrorBody) {
                error_body.append(chunk.substr(0, kMaxErrorBody - error_body.size()));
                return true;
            }
            error_body_capped = true;
            return false;
        }
        try {
            for (const auto& frame : sse_parser.Feed(chunk)) {
                // Anthropic delivers a mid-stream failure as a NAMED
                // `event: error` frame -- checked via the frame's own event
                // field (unlike FeedFrame, which switches on the JSON
                // payload's "type" and never sees this one, since the
                // reassembler's job is the happy path).
                if (frame.event == "error") {
                    throw ClassifyErrorEventData(frame.data);
                }
                if (!reassembler.FeedFrame(frame.data, sink)) {
                    cancelled_by_sink = true;
                    return false;
                }
            }
            return true;
        } catch (...) {
            pending_error = std::current_exception();
            return false;
        }
    };

    auto rethrow_pending_if_any = [&]() {
        if (!pending_error) return;
        try {
            std::rethrow_exception(pending_error);
        } catch (const AnthropicParseError& error) {
            RethrowParseErrorAsLlmError(error);
        } catch (const llm::LlmError&) {
            throw;  // already classified; re-wrapping would lose its category
        } catch (const nlohmann::json::exception& error) {
            throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
        } catch (const std::exception& error) {
            throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
        }
    };

    try {
        http_.StreamPost(http_request, on_headers, on_chunk);
    } catch (const llm::HttpError& error) {
        rethrow_pending_if_any();
        if (error_body_capped && response_status != 0) {
            throw ClassifyHttpFailure(response_status, error_body, response_headers);
        }
        if (cancelled_by_sink || error.kind() == llm::HttpErrorKind::Cancelled) {
            try {
                // Blocks announced but never closed: a user cancel is
                // reported as Cancelled, not a parse failure -- same
                // tolerance mode as provider-openai's.
                reassembler.CloseOpenBlocks(sink);
                llm::ChatResponse response =
                    reassembler.Finalize(/*tolerate_malformed_tool_calls=*/true);
                response.stop_reason = llm::StopReason::Cancelled;
                return response;
            } catch (const AnthropicParseError& parse_error) {
                RethrowParseErrorAsLlmError(parse_error);
            }
        }
        if (is_error_status && response_status != 0) {
            throw ClassifyHttpFailure(response_status, error_body, response_headers);
        }
        throw ClassifyTransportFailure(error);
    }

    rethrow_pending_if_any();

    if (is_error_status) {
        throw ClassifyHttpFailure(response_status, error_body, response_headers);
    }

    // A stream that ended cleanly but without a stop_reason/message_stop is a
    // truncated generation, classified the same retryable way an idle
    // timeout is -- the only difference is who closed the connection. Same
    // posture as provider-openai's completed() check.
    if (!reassembler.completed()) {
        reassembler.CloseOpenBlocks(sink);
        throw llm::LlmError(llm::LlmErrorCategory::Transport,
                            "stream ended without a completion (no stop_reason, no message_stop)");
    }

    try {
        reassembler.CloseOpenBlocks(sink);
        return reassembler.Finalize();
    } catch (const AnthropicParseError& error) {
        RethrowParseErrorAsLlmError(error);
    }
    __builtin_unreachable();  // RethrowParseErrorAsLlmError always throws
}

}  // namespace minimal::provider_anthropic
