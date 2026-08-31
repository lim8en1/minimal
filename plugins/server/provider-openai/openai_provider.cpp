#include "openai_provider.hpp"

#include <exception>

#include "minimal/llm/sse_parser.hpp"
#include "openai_error_mapping.hpp"
#include "openai_request.hpp"
#include "openai_response.hpp"
#include "openai_stream_reassembler.hpp"

namespace minimal::provider_openai {

namespace {

[[noreturn]] void RethrowParseErrorAsLlmError(const OpenAiParseError& error) {
    throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
}

}  // namespace

llm::HttpRequest OpenAiProvider::BuildHttpRequest(std::string body, bool streaming) const {
    llm::HttpRequest request;
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    request.url = base + "/chat/completions";
    request.headers.push_back({"Content-Type", "application/json"});
    if (!config_.api_key.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + config_.api_key});
    }
    request.body = std::move(body);
    request.connect_timeout = config_.connect_timeout;
    // Never cap total duration of a stream; the idle timeout covers stalls.
    request.request_timeout = streaming ? std::chrono::milliseconds(0) : config_.request_timeout;
    request.idle_timeout = streaming ? config_.stream_idle_timeout : std::chrono::milliseconds(0);
    return request;
}

llm::ChatResponse OpenAiProvider::Complete(const llm::ChatRequest& request) {
    llm::ChatRequest normalized = request;
    normalized.stream = false;
    llm::HttpRequest http_request = BuildHttpRequest(BuildRequestBody(normalized), /*streaming=*/false);

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
    } catch (const OpenAiParseError& error) {
        RethrowParseErrorAsLlmError(error);
    } catch (const nlohmann::json::exception& error) {
        throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
    }
    __builtin_unreachable();  // both catch branches above always throw
}

llm::ChatResponse OpenAiProvider::StreamComplete(const llm::ChatRequest& request, const llm::StreamEventSink& sink) {
    llm::ChatRequest normalized = request;
    normalized.stream = true;
    llm::HttpRequest http_request = BuildHttpRequest(BuildRequestBody(normalized), /*streaming=*/true);

    OpenAiStreamReassembler reassembler;
    llm::SseParser sse_parser;
    int response_status = 0;
    std::vector<llm::HttpHeader> response_headers;
    std::string error_body;
    bool is_error_status = false;
    bool cancelled_by_sink = false;
    std::exception_ptr pending_error;

    auto on_headers = [&](int status, const std::vector<llm::HttpHeader>& headers) {
        response_status = status;
        response_headers = headers;
        is_error_status = status < 200 || status >= 300;
    };

    // Exceptions must never escape into libcurl's C call stack (undefined
    // behavior) -- caught here, stashed, and rethrown once StreamPost unwinds.
    auto on_chunk = [&](std::string_view chunk) -> bool {
        if (is_error_status) {
            error_body.append(chunk);
            return true;
        }
        try {
            for (const auto& frame : sse_parser.Feed(chunk)) {
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
        } catch (const OpenAiParseError& error) {
            RethrowParseErrorAsLlmError(error);
        }
    };

    try {
        http_.StreamPost(http_request, on_headers, on_chunk);
    } catch (const llm::HttpError& error) {
        rethrow_pending_if_any();
        if (cancelled_by_sink || error.kind() == llm::HttpErrorKind::Cancelled) {
            try {
                llm::ChatResponse response = reassembler.Finalize();
                response.stop_reason = llm::StopReason::Cancelled;
                return response;
            } catch (const OpenAiParseError& parse_error) {
                RethrowParseErrorAsLlmError(parse_error);
            }
        }
        throw ClassifyTransportFailure(error);
    }

    rethrow_pending_if_any();

    if (is_error_status) {
        throw ClassifyHttpFailure(response_status, error_body, response_headers);
    }

    try {
        return reassembler.Finalize();
    } catch (const OpenAiParseError& error) {
        RethrowParseErrorAsLlmError(error);
    }
    __builtin_unreachable();  // RethrowParseErrorAsLlmError always throws
}

}  // namespace minimal::provider_openai
