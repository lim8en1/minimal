#include "anthropic_provider.hpp"

#include <exception>

#include "anthropic_error_mapping.hpp"
#include "anthropic_request.hpp"
#include "anthropic_response.hpp"
#include "anthropic_stream_reassembler.hpp"
#include "minimal/llm/json_depth.hpp"
#include "minimal/net/sse_parser.hpp"
#include "minimal/common/units.hpp"

namespace minimal::provider_anthropic {

namespace {

constexpr const char* kAnthropicVersion = "2023-06-01";

[[noreturn]] void RethrowParseErrorAsLlmError(const AnthropicParseError& error) {
    throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
}

// BuildRequestBody runs OUTSIDE the transport try/catch blocks below, so an
// exception out of it used to escape the provider unclassified. The string
// escaping now replaces invalid UTF-8 rather than throwing; any residual
// serializer failure is still classified here (same shape as provider-openai).
std::string GuardedBuildRequestBody(const llm::ChatRequest& request, bool stream) {
    try {
        return BuildRequestBody(request, stream);
    } catch (const std::exception& error) {
        throw llm::LlmError(llm::LlmErrorCategory::RequestError,
                            "failed to build request body: " + std::string(error.what()));
    }
}

}  // namespace

net::HttpRequest AnthropicProvider::BuildHttpRequest(std::string body, bool streaming,
                                                     const std::string& session_id) const {
    net::HttpRequest request;
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    // Unlike provider-openai's convention, base_url here is expected WITHOUT
    // a "/v1" suffix (the canonical Anthropic base is
    // "https://api.anthropic.com") -- the full endpoint path is "/v1/messages".
    request.url = base + "/v1/messages";
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"anthropic-version", kAnthropicVersion});
    // x-api-key, not Authorization: Bearer -- Anthropic's own auth header.
    if (const auto key = llm::RequestApiKey(config_)) {
        request.headers.push_back({"x-api-key", *key});
    }
    // The session's provider-sticky-routing key, default ON whenever the
    // request carries one (agent loop, compaction summarizer included).
    if (!session_id.empty()) {
        request.headers.push_back({"x-session-id", session_id});
    }
    for (const auto& [name, value] : config_.headers) {
        request.headers.push_back({name, value});
    }
    request.body = std::move(body);
    request.connect_timeout = config_.connect_timeout;
    request.request_timeout = streaming ? std::chrono::milliseconds(0) : config_.request_timeout;
    request.idle_timeout = streaming ? config_.stream_idle_timeout : std::chrono::milliseconds(0);
    request.first_byte_timeout = streaming ? config_.first_byte_timeout : std::chrono::milliseconds(0);
    return request;
}

llm::ChatResponse AnthropicProvider::Complete(const llm::ChatRequest& request) {
    // The session's sticky-routing key, default ON whenever the request
    // carries one (agent loop, compaction summarizer included) -- see
    // SessionIdHeaderValue for the parameters contract.
    const std::string session_id = SessionIdHeaderValue(request);
    net::HttpRequest http_request = BuildHttpRequest(
        GuardedBuildRequestBody(request, /*stream=*/false), /*streaming=*/false, session_id);
    http_request.should_abort = request.should_abort;

    net::HttpResponse http_response;
    try {
        http_response = http_.Post(http_request);
    } catch (const net::HttpError& error) {
        throw ClassifyTransportFailure(error);
    }

    if (http_response.status < 200 || http_response.status >= 300) {
        throw ClassifyHttpFailure(http_response.status, http_response.body, http_response.headers);
    }

    try {
        if (!llm::JsonDepthWithinLimit(http_response.body)) {
            throw AnthropicParseError("response exceeds the JSON nesting limit");
        }
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
    const std::string session_id = SessionIdHeaderValue(request);
    net::HttpRequest http_request = BuildHttpRequest(
        GuardedBuildRequestBody(request, /*stream=*/true), /*streaming=*/true, session_id);
    http_request.should_abort = request.should_abort;

    AnthropicStreamReassembler reassembler;
    net::SseParser sse_parser;
    int response_status = 0;
    std::vector<net::HttpHeader> response_headers;
    std::string error_body;
    bool is_error_status = false;
    bool cancelled_by_sink = false;
    bool error_body_capped = false;
    std::exception_ptr pending_error;

    // Both callbacks run inside libcurl's C call stack: an exception escaping
    // either is undefined behavior. on_headers has no false-returning sink
    // contract, so it stashes the exception (cheap, allocation-free
    // assignments run first, keeping the error-status bookkeeping consistent
    // even if the header-vector copy throws) for rethrow_pending_if_any()
    // once StreamPost unwinds.
    auto on_headers = [&](int status, const std::vector<net::HttpHeader>& headers) {
        is_error_status = status < 200 || status >= 300;
        response_status = status;
        try {
            response_headers = headers;  // vector copy -- allocation can throw
        } catch (...) {
            pending_error = std::current_exception();
        }
    };

    // Same no-throw boundary: caught here, stashed, and rethrown once
    // StreamPost unwinds. Identical shape to provider-openai's on_chunk.
    auto on_chunk = [&](std::string_view chunk) -> bool {
        if (is_error_status) {
            // The append allocates too -- the barrier covers the whole
            // error-body branch, not just the parse path below.
            try {
                constexpr std::size_t kMaxErrorBody = 64 * common::kKiB;
                if (error_body.size() < kMaxErrorBody) {
                    error_body.append(chunk.substr(0, kMaxErrorBody - error_body.size()));
                    return true;
                }
                error_body_capped = true;
                return false;
            } catch (...) {
                pending_error = std::current_exception();
                return false;
            }
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
                // Keepalives and other non-data frames: a bare empty `data:`
                // line. json::parse("") throws, and the resulting ParseError
                // is NON-retryable -- a proxy that pings between chunks would
                // turn the whole turn into a hard failure. An empty payload
                // carries nothing to reassemble; skip it before parsing. (The
                // exact guard provider-openai has always had.)
                if (frame.data.find_first_not_of(" \t\r\n") == std::string::npos) {
                    continue;
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
    } catch (const net::HttpError& error) {
        rethrow_pending_if_any();
        if (error_body_capped && response_status != 0) {
            throw ClassifyHttpFailure(response_status, error_body, response_headers);
        }
        if (cancelled_by_sink || error.kind() == net::HttpErrorKind::Cancelled) {
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
