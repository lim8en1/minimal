#include "openai_provider.hpp"

#include <exception>

#include "minimal/llm/json_depth.hpp"
#include "minimal/net/sse_parser.hpp"
#include "openai_error_mapping.hpp"
#include "openai_request.hpp"
#include "openai_response.hpp"
#include "openai_stream_reassembler.hpp"

namespace minimal::provider_openai {

namespace {

[[noreturn]] void RethrowParseErrorAsLlmError(const OpenAiParseError& error) {
    throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
}

// BuildRequestBody runs OUTSIDE the transport try/catch blocks below, so an
// exception out of it used to escape the provider unclassified (and, on the
// agent loop, brick the session: every subsequent turn -- and the compaction
// call -- threw again). The string escaping now replaces invalid UTF-8 rather
// than throwing; any residual serializer failure is still classified here.
std::string GuardedBuildRequestBody(const llm::ChatRequest& request, bool stream,
                                    const std::string& max_tokens_field) {
    try {
        return BuildRequestBody(request, stream, max_tokens_field);
    } catch (const std::exception& error) {
        throw llm::LlmError(llm::LlmErrorCategory::RequestError,
                            "failed to build request body: " + std::string(error.what()));
    }
}

}  // namespace

net::HttpRequest OpenAiProvider::BuildHttpRequest(std::string body, bool streaming) const {
    net::HttpRequest request;
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    request.url = base + "/chat/completions";
    request.headers.push_back({"Content-Type", "application/json"});
    if (const auto key = llm::RequestApiKey(config_)) {
        request.headers.push_back({"Authorization", "Bearer " + *key});
    }
    for (const auto& [name, value] : config_.headers) {
        request.headers.push_back({name, value});
    }
    request.body = std::move(body);
    request.connect_timeout = config_.connect_timeout;
    // Never cap total duration of a stream; the idle timeout covers stalls.
    request.request_timeout = streaming ? std::chrono::milliseconds(0) : config_.request_timeout;
    request.idle_timeout = streaming ? config_.stream_idle_timeout : std::chrono::milliseconds(0);
    request.first_byte_timeout = streaming ? config_.first_byte_timeout : std::chrono::milliseconds(0);
    return request;
}

llm::ChatResponse OpenAiProvider::Complete(const llm::ChatRequest& request) {
    // No deep copy: `stream` rides BuildRequestBody as a parameter. The copy
    // this replaces duplicated the tools vector (every schema DOM), the
    // parameters object and the system string per call -- and, worse, per
    // RETRY ATTEMPT, since the loop calls this method once per attempt.
    net::HttpRequest http_request =
        BuildHttpRequest(GuardedBuildRequestBody(request, /*stream=*/false, config_.max_tokens_field), /*streaming=*/false);
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
            throw OpenAiParseError("response exceeds the JSON nesting limit");
        }
        return ParseNonStreamingResponse(nlohmann::json::parse(http_response.body));
    } catch (const OpenAiParseError& error) {
        RethrowParseErrorAsLlmError(error);
    } catch (const nlohmann::json::exception& error) {
        throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
    }
    __builtin_unreachable();  // both catch branches above always throw
}

llm::ChatResponse OpenAiProvider::StreamComplete(const llm::ChatRequest& request, const llm::StreamEventSink& sink) {
    net::HttpRequest http_request =
        BuildHttpRequest(GuardedBuildRequestBody(request, /*stream=*/true, config_.max_tokens_field), /*streaming=*/true);
    http_request.should_abort = request.should_abort;

    OpenAiStreamReassembler reassembler;
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

    // Exceptions must never escape into libcurl's C call stack (undefined
    // behavior) -- caught here, stashed, and rethrown once StreamPost unwinds.
    auto on_chunk = [&](std::string_view chunk) -> bool {
        if (is_error_status) {
            // The append allocates too -- the barrier covers the whole
            // error-body branch, not just the parse path below.
            try {
                // For classification and the error message only -- never parsed as
                // a completion. The cap is a CLASSIFICATION BUDGET, not a reading
                // commitment: 64 KiB is far more than any error message needs, and
                // a drip-feeding proxy would otherwise hold the transfer open
                // forever (the body cap used to stop accumulating but keep
                // reading). Aborting lands in the catch below, which classifies by
                // the STATUS -- the one fact that matters -- with the body we
                // already hold.
                constexpr std::size_t kMaxErrorBody = 64 * 1024;
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
                // OpenAI-shaped gateways deliver mid-stream failures as an
                // `event: error` frame; the parser always produced the event
                // name and the reassembler silently ignored it -- the error
                // vanished and the turn came back empty, presented as
                // success. Classify here, where the status machinery lives;
                // the thrown LlmError rides pending_error out of libcurl's
                // stack and is rethrown unchanged below.
                if (frame.event == "error") {
                    throw ClassifyErrorEventData(frame.data);
                }
                // Keepalives and other non-data frames: `event: ping` with no
                // data, or a bare empty `data:` line. json::parse("") throws,
                // and the resulting ParseError is NON-retryable -- a proxy
                // that pings between chunks turned the whole turn into a hard
                // failure. An empty payload carries nothing to reassemble;
                // skip it before parsing.
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
        } catch (const OpenAiParseError& error) {
            RethrowParseErrorAsLlmError(error);
        } catch (const llm::LlmError&) {
            throw;  // already classified; re-wrapping would lose its category
        } catch (const nlohmann::json::exception& error) {
            // FeedFrame reaches raw nlohmann accessors in several places --
            // delta.at("content").get<std::string>(), the tool-call fields, and
            // ParseUsage -- and only json::parse was wrapped in OpenAiParseError.
            // A proxy sending `usage: null` or a null content field therefore
            // threw a type_error that this lambda caught and then DROPPED on the
            // floor, since the only handler was for OpenAiParseError: the turn
            // came back empty with no error at all. The non-streaming path has
            // caught json::exception all along; the streaming path had drifted.
            throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
        } catch (const std::exception& error) {
            // Anything else out of the sink or the reassembler. Unclassified is
            // still infinitely better than silently discarded.
            throw llm::LlmError(llm::LlmErrorCategory::ParseError, error.what());
        }
    };

    try {
        http_.StreamPost(http_request, on_headers, on_chunk);
    } catch (const net::HttpError& error) {
        rethrow_pending_if_any();
        // The reader aborted the error body at its classification cap (the
        // on_chunk error branch returned false). Classify by the STATUS we
        // already hold -- a 429 whose body we stopped reading must not become
        // a retryable Transport loss of its Retry-After, and the abort itself
        // surfaces as HttpError(Cancelled) because the sink contract has only
        // one false shape.
        if (error_body_capped && response_status != 0) {
            throw ClassifyHttpFailure(response_status, error_body, response_headers);
        }
        if (cancelled_by_sink || error.kind() == net::HttpErrorKind::Cancelled) {
            try {
                // Blocks announced but never closed: the client's view has to be
                // consistent even when the stream was cut. The sink is passed
                // because these are wire events, not response content. Tolerance
                // mode: a call whose arguments were cut mid-JSON is dropped, so
                // a user cancel surfaces as Cancelled -- the exit code derives
                // from the stop reason -- instead of a parse failure.
                reassembler.CloseOpenBlocks(sink);
                llm::ChatResponse response =
                    reassembler.Finalize(/*tolerate_malformed_tool_calls=*/true);
                response.stop_reason = llm::StopReason::Cancelled;
                return response;
            } catch (const OpenAiParseError& parse_error) {
                RethrowParseErrorAsLlmError(parse_error);
            }
        }
        // The connection dropped AFTER the message completed (finish_reason
        // processed or [DONE] arrived) but BEFORE the socket closed tidily:
        // the response is whole -- finalizing it as a Transport failure threw
        // away a complete answer the user was already billed for and re-billed
        // them on retry. Return it as a success instead, exactly as the
        // clean-close path below would have. CloseOpenBlocks is idempotent
        // when finish_reason already closed every block.
        if (reassembler.completed()) {
            try {
                reassembler.CloseOpenBlocks(sink);
                return reassembler.Finalize();
            } catch (const OpenAiParseError& parse_error) {
                RethrowParseErrorAsLlmError(parse_error);
            }
        }
        // A transport failure DURING an error response: the status is the
        // classification that matters (a 401 behind a stalling proxy must not
        // become a retryable Transport and hammer the backend with retries).
        if (is_error_status && response_status != 0) {
            throw ClassifyHttpFailure(response_status, error_body, response_headers);
        }
        throw ClassifyTransportFailure(error);
    }

    rethrow_pending_if_any();

    if (is_error_status) {
        throw ClassifyHttpFailure(response_status, error_body, response_headers);
    }

    // A stream that ended CLEANLY but without a completion -- no finish_reason,
    // no [DONE] -- used to finalize as StopReason::Unknown and be ACCEPTED as a
    // complete turn: a proxy truncating mid-generation with a tidy FIN produced
    // a partial assistant message persisted as ordinary history. That is a
    // transport failure, classified as one (retryable), the same posture as the
    // idle timeout -- the only difference is who closed the connection.
    if (!reassembler.completed()) {
        reassembler.CloseOpenBlocks(sink);
        throw llm::LlmError(llm::LlmErrorCategory::Transport,
                            "stream ended without a completion (no finish_reason, no [DONE])");
    }

    try {
        // A stream that stopped without a finish_reason still owes the client a
        // stop for every block it opened.
        reassembler.CloseOpenBlocks(sink);
        return reassembler.Finalize();
    } catch (const OpenAiParseError& error) {
        RethrowParseErrorAsLlmError(error);
    }
    __builtin_unreachable();  // RethrowParseErrorAsLlmError always throws
}

}  // namespace minimal::provider_openai
