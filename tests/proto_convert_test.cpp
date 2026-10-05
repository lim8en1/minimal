#include <catch2/catch_test_macros.hpp>

#include "minimal/rpc/proto_convert.hpp"
#include "proto_inverse.hpp"

using namespace minimal;
using namespace minimal::rpc;

namespace {

// Round-trips a block and returns it, so each case reads as one assertion about
// what survived rather than four lines of plumbing.
llm::ContentBlock RoundTrip(const llm::ContentBlock& block) {
    auto result = FromProto(ToProto(block));
    REQUIRE(result.has_value());
    return *result;
}

nlohmann::json RoundTrip(const nlohmann::json& value) {
    return StructToJson(JsonToStruct(value));
}

}  // namespace

// --- JSON <-> Struct ---------------------------------------------------------

TEST_CASE("json round-trips through Struct", "[proto_convert]") {
    SECTION("nested objects and arrays") {
        const nlohmann::json input = {
            {"name", "read"},
            {"nested", {{"depth", 2}, {"flag", true}}},
            {"list", {1, "two", false, nullptr}},
        };
        CHECK(RoundTrip(input) == input);
    }

    SECTION("empty object") {
        CHECK(RoundTrip(nlohmann::json::object()) == nlohmann::json::object());
    }

    SECTION("null, bool, and string scalars survive as themselves") {
        const nlohmann::json input = {{"n", nullptr}, {"b", false}, {"s", "hello"}};
        const nlohmann::json out = RoundTrip(input);
        CHECK(out["n"].is_null());
        CHECK(out["b"].is_boolean());
        CHECK(out["s"] == "hello");
    }

    SECTION("non-ASCII strings are not mangled") {
        const nlohmann::json input = {{"s", "héllo — 世界 🌍"}};
        CHECK(RoundTrip(input) == input);
    }

    SECTION("a deliberate float keeps its fractional part") {
        const nlohmann::json input = {{"temperature", 0.75}};
        CHECK(RoundTrip(input) == input);
        CHECK(RoundTrip(input)["temperature"].is_number_float());
    }
}

TEST_CASE("integers survive Struct's double-only number model", "[proto_convert]") {
    // Struct has no integer type, so this is the documented narrowing: an integer
    // goes out as a double and must come back an integer, or every tool input
    // logged or re-rendered client-side turns `offset: 3` into `offset: 3.0`.
    const nlohmann::json input = {{"offset", 3}, {"limit", 2000}, {"negative", -17}};
    const nlohmann::json out = RoundTrip(input);
    CHECK(out == input);
    CHECK(out["offset"].is_number_integer());
    CHECK(out["limit"].is_number_integer());
    CHECK(out["negative"].is_number_integer());
}

TEST_CASE("a non-object json cannot be a Struct and yields an empty one", "[proto_convert]") {
    CHECK(JsonToStruct(nlohmann::json::array({1, 2})).fields_size() == 0);
    CHECK(JsonToStruct(nlohmann::json(42)).fields_size() == 0);
}

// --- Content blocks ----------------------------------------------------------

TEST_CASE("every ContentBlock alternative round-trips", "[proto_convert]") {
    SECTION("text") {
        const llm::ContentBlock block{llm::TextBlock{"hello"}};
        const auto out = RoundTrip(block);
        REQUIRE(std::holds_alternative<llm::TextBlock>(out));
        CHECK(std::get<llm::TextBlock>(out).text == "hello");
    }

    SECTION("thinking, with and without a signature") {
        const llm::ContentBlock with{llm::ThinkingBlock{"reasoning", "sig-abc"}};
        const auto out = RoundTrip(with);
        REQUIRE(std::holds_alternative<llm::ThinkingBlock>(out));
        CHECK(std::get<llm::ThinkingBlock>(out).text == "reasoning");
        CHECK(std::get<llm::ThinkingBlock>(out).signature == "sig-abc");

        const llm::ContentBlock without{llm::ThinkingBlock{"reasoning", std::nullopt}};
        CHECK_FALSE(std::get<llm::ThinkingBlock>(RoundTrip(without)).signature.has_value());
    }

    SECTION("thinking, redacted data round-trips") {
        const llm::ContentBlock redacted{llm::ThinkingBlock{"", std::nullopt, "encrypted-blob"}};
        const auto out = RoundTrip(redacted);
        REQUIRE(std::holds_alternative<llm::ThinkingBlock>(out));
        const auto& thinking = std::get<llm::ThinkingBlock>(out);
        CHECK_FALSE(thinking.signature.has_value());
        REQUIRE(thinking.redacted_data.has_value());
        CHECK(*thinking.redacted_data == "encrypted-blob");
    }

    SECTION("tool_use with nested-object input") {
        const nlohmann::json input = {{"path", "src/main.cpp"}, {"opts", {{"offset", 10}}}};
        const llm::ContentBlock block{llm::ToolUseBlock{"call_1", "read", input}};
        const auto out = RoundTrip(block);
        REQUIRE(std::holds_alternative<llm::ToolUseBlock>(out));
        const auto& tool_use = std::get<llm::ToolUseBlock>(out);
        CHECK(tool_use.id == "call_1");
        CHECK(tool_use.name == "read");
        CHECK(tool_use.input == input);
    }

    SECTION("tool_result carrying is_error") {
        llm::ToolResultBlock result;
        result.tool_use_id = "call_1";
        result.is_error = true;
        result.content.push_back(llm::TextBlock{"permission denied"});
        const auto out = RoundTrip(llm::ContentBlock{result});
        REQUIRE(std::holds_alternative<llm::ToolResultBlock>(out));
        const auto& converted = std::get<llm::ToolResultBlock>(out);
        CHECK(converted.tool_use_id == "call_1");
        CHECK(converted.is_error);
        REQUIRE(converted.content.size() == 1);
        CHECK(std::get<llm::TextBlock>(converted.content[0]).text == "permission denied");
        CHECK(converted.display.is_null());
    }

    SECTION("tool_result carrying a display payload") {
        llm::ToolResultBlock result;
        result.tool_use_id = "call_1";
        result.content.push_back(llm::TextBlock{"ok"});
        result.display = {{"kind", "process"}, {"command", "ls"}, {"exit_code", 0},
                          {"duration_ms", 12}};
        const auto out = RoundTrip(llm::ContentBlock{result});
        const auto& converted = std::get<llm::ToolResultBlock>(out);
        CHECK(converted.display == result.display);
        CHECK(converted.display.at("exit_code").is_number_integer());   // not a double
    }
}

TEST_CASE("a malformed display_json is dropped, the result text survives", "[proto_convert]") {
    v1::ContentBlock block;
    auto* result = block.mutable_tool_result();
    result->set_tool_use_id("call_1");
    result->add_content()->mutable_text()->set_text("kept");
    result->set_display_json("{not json");

    const auto out = FromProto(block);
    REQUIRE(out.has_value());
    const auto& converted = std::get<llm::ToolResultBlock>(*out);
    CHECK(converted.display.is_null());
    REQUIRE(converted.content.size() == 1);
    CHECK(std::get<llm::TextBlock>(converted.content[0]).text == "kept");
}

TEST_CASE("a block with an unset oneof converts to nullopt, not a default block",
          "[proto_convert]") {
    // Off the wire this is a default-constructed message. Materialising it as an
    // empty TextBlock would put a phantom message into history.
    CHECK_FALSE(FromProto(v1::ContentBlock{}).has_value());
}

TEST_CASE("a tool_result containing a non-text child drops it rather than throwing",
          "[proto_convert]") {
    // The wire type is recursive and the domain type deliberately is not. This
    // input is malformed rather than merely unsupported, and it arrived over a
    // socket, so it must degrade instead of aborting the session.
    v1::ContentBlock block;
    auto* result = block.mutable_tool_result();
    result->set_tool_use_id("call_1");
    result->add_content()->mutable_text()->set_text("kept");
    result->add_content()->mutable_tool_use()->set_id("nested");
    result->add_content()->mutable_text()->set_text("also kept");

    const auto out = FromProto(block);
    REQUIRE(out.has_value());
    const auto& converted = std::get<llm::ToolResultBlock>(*out);
    REQUIRE(converted.content.size() == 2);
    CHECK(std::get<llm::TextBlock>(converted.content[0]).text == "kept");
    CHECK(std::get<llm::TextBlock>(converted.content[1]).text == "also kept");
}

TEST_CASE("a message with mixed block kinds round-trips in order", "[proto_convert]") {
    llm::Message message;
    message.role = llm::Role::Assistant;
    message.content.push_back(llm::TextBlock{"let me look"});
    message.content.push_back(llm::ThinkingBlock{"planning", std::nullopt});
    message.content.push_back(llm::ToolUseBlock{"c1", "bash", {{"command", "ls"}}});

    const llm::Message out = FromProto(ToProto(message));
    CHECK(out.role == llm::Role::Assistant);
    REQUIRE(out.content.size() == 3);
    CHECK(std::holds_alternative<llm::TextBlock>(out.content[0]));
    CHECK(std::holds_alternative<llm::ThinkingBlock>(out.content[1]));
    CHECK(std::holds_alternative<llm::ToolUseBlock>(out.content[2]));
}

// --- Stream events -----------------------------------------------------------

TEST_CASE("every StreamEvent alternative round-trips with its payload", "[proto_convert]") {
    auto trip = [](const llm::StreamEvent& event) {
        auto out = FromProto(ToProto(event));
        REQUIRE(out.has_value());
        return *out;
    };

    CHECK(std::get<llm::MessageStartEvent>(trip(llm::MessageStartEvent{"id-1", "gpt"})).model ==
          "gpt");

    const auto text = std::get<llm::TextDeltaEvent>(trip(llm::TextDeltaEvent{3, "chunk"}));
    CHECK(text.block_index == 3);
    CHECK(text.text == "chunk");

    const auto tool_start =
        std::get<llm::ToolUseStartEvent>(trip(llm::ToolUseStartEvent{1, "c1", "read"}));
    CHECK(tool_start.block_index == 1);
    CHECK(tool_start.id == "c1");
    CHECK(tool_start.name == "read");

    const auto tool_delta = std::get<llm::ToolUseInputDeltaEvent>(
        trip(llm::ToolUseInputDeltaEvent{2, "{\"pa"}));
    CHECK(tool_delta.block_index == 2);
    CHECK(tool_delta.partial_json == "{\"pa");

    const auto thinking =
        std::get<llm::ThinkingDeltaEvent>(trip(llm::ThinkingDeltaEvent{4, "hmm"}));
    CHECK(thinking.block_index == 4);
    CHECK(thinking.text == "hmm");

    CHECK(std::get<llm::ContentBlockStopEvent>(trip(llm::ContentBlockStopEvent{5})).block_index ==
          5);

    CHECK(std::get<llm::MessageDeltaEvent>(trip(llm::MessageDeltaEvent{llm::StopReason::ToolUse}))
              .stop_reason == llm::StopReason::ToolUse);

    llm::Usage usage{11, 22, 33, 44};
    const auto usage_out = std::get<llm::UsageEvent>(trip(llm::UsageEvent{usage})).usage;
    CHECK(usage_out.input_tokens == 11);
    CHECK(usage_out.output_tokens == 22);
    CHECK(usage_out.cache_read_input_tokens == 33);
    CHECK(usage_out.cache_creation_input_tokens == 44);

    CHECK(std::holds_alternative<llm::MessageStopEvent>(trip(llm::MessageStopEvent{})));
}

TEST_CASE("a stream event with an unset oneof converts to nullopt", "[proto_convert]") {
    CHECK_FALSE(FromProto(v1::StreamEvent{}).has_value());
}

// --- Enums -------------------------------------------------------------------

TEST_CASE("StopReason::Cancelled has a wire counterpart", "[proto_convert]") {
    // The headless client derives its exit code from stop_reason. Without this
    // mapping a cancelled run is indistinguishable from a malformed one, which
    // is what the proto did before this milestone.
    CHECK(ToProto(llm::StopReason::Cancelled) == v1::STOP_REASON_CANCELLED);
    CHECK(FromProto(v1::STOP_REASON_CANCELLED) == llm::StopReason::Cancelled);
}

TEST_CASE("every enum mapping is exhaustive", "[proto_convert]") {
    // The invariant: no domain enumerator may fall through to the wire's
    // UNSPECIFIED sentinel, and no wire enumerator may fall through to the
    // domain's Unknown. Adding an enumerator without a mapping fails here.
    //
    // Unknown/UNSPECIFIED are excluded from each list because they ARE the
    // fallback -- they are checked separately below.

    SECTION("StopReason, domain -> wire") {
        for (const auto value : {llm::StopReason::EndTurn, llm::StopReason::ToolUse,
                                  llm::StopReason::MaxTokens, llm::StopReason::StopSequence,
                                  llm::StopReason::ContentFilter, llm::StopReason::Refusal,
                                  llm::StopReason::Cancelled}) {
            INFO("domain StopReason " << static_cast<int>(value));
            CHECK(ToProto(value) != v1::STOP_REASON_UNSPECIFIED);
        }
        CHECK(ToProto(llm::StopReason::Unknown) == v1::STOP_REASON_UNSPECIFIED);
    }

    SECTION("StopReason, wire -> domain") {
        for (int raw = v1::STOP_REASON_END_TURN; raw <= v1::StopReason_MAX; ++raw) {
            if (!v1::StopReason_IsValid(raw)) continue;
            INFO("wire StopReason " << raw);
            CHECK(FromProto(static_cast<v1::StopReason>(raw)) != llm::StopReason::Unknown);
        }
    }

    SECTION("Role") {
        for (const auto value : {llm::Role::User, llm::Role::Assistant}) {
            CHECK(ToProto(value) != v1::ROLE_UNSPECIFIED);
            CHECK(FromProto(ToProto(value)) == value);
        }
    }

    SECTION("ToolCategory") {
        for (const auto value : {tools::ToolCategory::ReadOnly, tools::ToolCategory::FileWrite,
                                  tools::ToolCategory::Execute}) {
            CHECK(ToProto(value) != v1::TOOL_CATEGORY_UNSPECIFIED);
            CHECK(FromProto(ToProto(value)) == value);
        }
    }

    SECTION("LlmErrorCategory") {
        for (const auto value :
             {llm::LlmErrorCategory::Transport, llm::LlmErrorCategory::RateLimit,
              llm::LlmErrorCategory::ServerError, llm::LlmErrorCategory::AuthError,
              llm::LlmErrorCategory::RequestError, llm::LlmErrorCategory::ParseError}) {
            CHECK(ToProto(value) != v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
            CHECK(FromProto(ToProto(value)) == value);
        }
    }

    SECTION("PermissionRunMode") {
        for (const auto value : {tools::PermissionRunMode::AllowOnly,
                                  tools::PermissionRunMode::Normal,
                                  tools::PermissionRunMode::Bypass}) {
            CHECK(ToProto(value) != v1::PERMISSION_RUN_MODE_UNSPECIFIED);
            CHECK(FromProto(ToProto(value)) == value);
        }
        // UNSPECIFIED is not a run mode, it is "the client did not ask" -- the
        // server resolves it from permissions.json, so it must stay
        // distinguishable rather than collapsing into a default.
        CHECK_FALSE(FromProto(v1::PERMISSION_RUN_MODE_UNSPECIFIED).has_value());
    }
}

TEST_CASE("an unspecified permission decision denies", "[proto_convert]") {
    // A default-constructed answer is silence, and silence is not consent.
    CHECK(FromProto(v1::PERMISSION_DECISION_UNSPECIFIED) == tools::PermissionDecision::Deny);
    CHECK(FromProto(v1::PERMISSION_DECISION_ALLOW) == tools::PermissionDecision::Allow);
    CHECK(FromProto(v1::PERMISSION_DECISION_DENY) == tools::PermissionDecision::Deny);
}

// --- Errors ------------------------------------------------------------------

TEST_CASE("LlmError carries its classification onto the wire", "[proto_convert]") {
    const llm::LlmError retryable(llm::LlmErrorCategory::RateLimit, "slow down", 429);
    const v1::ErrorEvent out = ToProto(retryable);
    CHECK(out.category() == v1::LLM_ERROR_CATEGORY_RATE_LIMIT);
    CHECK(out.message() == "slow down");
    REQUIRE(out.has_http_status());
    CHECK(out.http_status() == 429);
    CHECK(out.is_retryable());

    const llm::LlmError fatal(llm::LlmErrorCategory::AuthError, "bad key");
    const v1::ErrorEvent fatal_out = ToProto(fatal);
    CHECK_FALSE(fatal_out.has_http_status());
    CHECK_FALSE(fatal_out.is_retryable());
}

// --- Session events ------------------------------------------------------------

TEST_CASE("every SessionEvent alternative lands on its ServerEvent oneof", "[proto_convert]") {
    const std::string session_id = "sess-1";
    auto trip = [&](const agent::SessionEvent& event) { return ToProto(session_id, event); };

    CHECK(trip(llm::StreamEvent{llm::MessageStopEvent{}}).session_id() == session_id);
    CHECK(trip(llm::StreamEvent{llm::MessageStopEvent{}}).payload_case() ==
          v1::ServerEvent::kStreamEvent);

    agent::ToolLifecycleEvent lifecycle;
    lifecycle.tool_use_id = "t1";
    lifecycle.tool_name = "bash";
    lifecycle.started = false;
    lifecycle.is_error = true;
    lifecycle.started_at_unix_ms = 42;
    lifecycle.duration = std::chrono::milliseconds(850);
    const auto execution = trip(lifecycle).tool_execution();
    CHECK(execution.tool_use_id() == "t1");
    CHECK(execution.tool_name() == "bash");
    CHECK_FALSE(execution.started());
    CHECK(execution.is_error());
    CHECK(execution.started_at_unix_ms() == 42);
    CHECK(execution.duration_ms() == 850);

    // duration_ms is meaningful only on the FINISH event -- a start event
    // (input still carries the pre-call args) must not report a stale/zero
    // duration as if the call had already been timed.
    agent::ToolLifecycleEvent start;
    start.tool_use_id = "t2";
    start.tool_name = "bash";
    start.started = true;
    start.duration = std::chrono::milliseconds(999);   // never legitimately set pre-finish
    CHECK(trip(start).tool_execution().duration_ms() == 0);

    tools::BackgroundTaskEvent task;
    task.task_id = "bg1";
    task.tool_use_id = "t1";
    task.tool_name = "python_exec";
    task.command = "sleep 1";
    task.pid = 123;
    task.state = tools::BackgroundTaskEvent::State::kKilled;
    const auto background = trip(task).background_task();
    CHECK(background.tool_use_id() == "t1");
    CHECK(background.tool_name() == "python_exec");
    CHECK(background.state() == v1::BackgroundTaskEvent::KILLED);

    const llm::LlmError provider_error(llm::LlmErrorCategory::RateLimit, "slow down", 429);
    const auto provider_error_out = trip(provider_error).error();
    CHECK(provider_error_out.category() == v1::LLM_ERROR_CATEGORY_RATE_LIMIT);
    CHECK(provider_error_out.is_retryable());

    agent::SessionCommandResult command_result;
    command_result.request_id = "req-1";
    command_result.result = commands::CommandResult::Error("nope");
    command_result.result.display = commands::CommandDisplay::kView;
    command_result.result.view_hint = "diff";
    const auto command_out = trip(command_result).command_result();
    CHECK(command_out.request_id() == "req-1");
    CHECK(command_out.status() == v1::COMMAND_STATUS_ERROR);
    CHECK(command_out.message() == "nope");
    CHECK(command_out.display() == v1::COMMAND_DISPLAY_VIEW);
    CHECK(command_out.view_hint() == "diff");

    const auto delta_out = trip(agent::ToolOutputDelta{"t1", "chunk"}).tool_output_delta();
    CHECK(delta_out.tool_use_id() == "t1");
    CHECK(delta_out.chunk() == "chunk");

    const auto subagent_out =
        trip(agent::SubagentStarted{"child-1", "do the thing", true}).subagent_started();
    CHECK(subagent_out.child_session_id() == "child-1");
    CHECK(subagent_out.task() == "do the thing");
    CHECK(subagent_out.blocking());

    agent::SessionMetadataChanged partial_name;
    partial_name.name = "new name";
    partial_name.partial = true;
    const auto partial_out = trip(partial_name).session_metadata();
    CHECK(partial_out.session_id() == session_id);
    CHECK(partial_out.name() == "new name");
    CHECK(partial_out.agent_preset().empty());
    CHECK(partial_out.context_window_tokens() == 0);

    agent::SessionMetadataChanged full;
    full.name = "n";
    full.agent_preset = "default";
    full.model = "anthropic/sonnet";
    full.permission_mode = "normal";
    full.context_window_tokens = 200000;
    full.partial = false;
    const auto full_out = trip(full).session_metadata();
    CHECK(full_out.agent_preset() == "default");
    CHECK(full_out.model() == "anthropic/sonnet");
    CHECK(full_out.context_window_tokens() == 200000);

    CHECK(trip(agent::SessionEnded{}).payload_case() == v1::ServerEvent::kSessionEnded);

    const auto rewound_out = trip(agent::SessionRewound{5, 4}).session_rewound();
    CHECK(rewound_out.truncated_to_seq() == 5);
    CHECK(rewound_out.active_last_seq() == 4);

    agent::TurnCompleted turn;
    turn.stop_reason = llm::StopReason::ToolUse;
    turn.usage = llm::Usage{1, 2, 3, 4};
    turn.hit_max_turns = true;
    turn.usage_estimated = true;
    const auto turn_out = trip(turn).turn_completed();
    CHECK(turn_out.stop_reason() == v1::STOP_REASON_TOOL_USE);
    CHECK(turn_out.usage().input_tokens() == 1);
    CHECK(turn_out.hit_max_turns());
    CHECK(turn_out.usage_estimated());

    agent::TurnCompleted reported;
    reported.usage = llm::Usage{1, 1, 0, 0};
    CHECK_FALSE(trip(reported).turn_completed().usage_estimated());

    const auto error_out = trip(agent::SessionError{"backlog full"}).error();
    CHECK(error_out.category() == v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
    CHECK(error_out.message() == "backlog full");
    CHECK_FALSE(error_out.is_retryable());
}

// --- Permission round-trip ---------------------------------------------------

TEST_CASE("a permission request carries its disclosure", "[proto_convert]") {
    tools::PermissionRequest request;
    request.tool_name = "bash";
    request.category = tools::ToolCategory::Execute;
    request.tool_input = {{"command", "make 2>&1 | tail -50"}};
    request.disclosure.summary = "make 2>&1 | tail -50";
    request.disclosure.grants = {{"bash:/usr/bin/make", "/usr/bin/make *"},
                                 {"bash:/usr/bin/tail", "/usr/bin/tail *"}};
    request.disclosure.complete = true;

    const v1::PermissionRequest out = ToProto(request);
    CHECK(out.tool_name() == "bash");
    CHECK(out.category() == v1::TOOL_CATEGORY_EXECUTE);
    CHECK(out.summary() == "make 2>&1 | tail -50");
    CHECK(out.complete());
    REQUIRE(out.grants_size() == 2);
    CHECK(out.grants(0).key() == "bash:/usr/bin/make");
    CHECK(out.grants(0).label() == "/usr/bin/make *");
    CHECK(out.grants(1).key() == "bash:/usr/bin/tail");
}

TEST_CASE("an incomplete disclosure says so on the wire", "[proto_convert]") {
    // `complete = false` is what stops a command we cannot name from satisfying
    // the grant conjunction. If it did not survive serialisation, the wire path
    // would silently be more permissive than the in-process one.
    tools::PermissionRequest request;
    request.tool_name = "bash";
    request.disclosure.complete = false;
    CHECK_FALSE(ToProto(request).complete());
}

TEST_CASE("only offered grant keys are accepted back", "[proto_convert]") {
    // A grant is only meaningful as the answer to something a human was shown.
    // Accepting arbitrary keys would let a client mint permission for a binary
    // no prompt ever displayed -- exactly what disclosure exists to prevent.
    tools::PermissionDisclosure offered;
    offered.grants = {{"bash:/usr/bin/make", "/usr/bin/make *"}};

    v1::PermissionResponse response;
    response.set_decision(v1::PERMISSION_DECISION_ALLOW);
    response.add_granted_keys("bash:/usr/bin/make");
    response.add_granted_keys("bash:/bin/rm");  // never offered

    const tools::PermissionResponse out = FromProto(response, offered);
    CHECK(out.decision == tools::PermissionDecision::Allow);
    REQUIRE(out.granted_keys.size() == 1);
    CHECK(out.granted_keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("denying with grant keys attached still denies", "[proto_convert]") {
    // The decision and the grants are independent answers; a deny must not be
    // softened by whatever the client attached alongside it.
    tools::PermissionDisclosure offered;
    offered.grants = {{"bash:/usr/bin/make", "/usr/bin/make *"}};

    v1::PermissionResponse response;
    response.set_decision(v1::PERMISSION_DECISION_DENY);
    response.add_granted_keys("bash:/usr/bin/make");

    CHECK(FromProto(response, offered).decision == tools::PermissionDecision::Deny);
}

TEST_CASE("a deny's message and stop ride through the wire conversion", "[proto_convert]") {
    v1::PermissionResponse response;
    response.set_decision(v1::PERMISSION_DECISION_DENY);
    response.set_deny_message("use make -j4");
    response.set_stop_turn(true);

    const tools::PermissionResponse out = FromProto(response, tools::PermissionDisclosure{});
    CHECK(out.decision == tools::PermissionDecision::Deny);
    CHECK(out.deny_message == "use make -j4");
    CHECK(out.stop_turn);
}

TEST_CASE("free text with ill-formed UTF-8 is scrubbed at the wire boundary", "[proto_convert]") {
    const std::string bad = std::string("hook said \xC3") + "\xFF end";

    const auto error = ToProto("s1", agent::SessionEvent{agent::SessionError{"prompt rejected: " + bad}});
    CHECK(error.error().message() == "prompt rejected: hook said \xEF\xBF\xBD\xEF\xBF\xBD end");

    const auto llm_error = ToProto(llm::LlmError(llm::LlmErrorCategory::Transport, bad));
    CHECK(llm_error.message().find("\xEF\xBF\xBD") != std::string::npos);

    // The serialized message must round-trip: that is what a peer's parser demands.
    v1::ServerEvent reparsed;
    CHECK(reparsed.ParseFromString(error.SerializeAsString()));
}
