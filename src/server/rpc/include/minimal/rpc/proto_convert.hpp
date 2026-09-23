#pragma once

// Domain <-> wire conversion. Pure functions: no threading, no gRPC, no I/O.
//
// This layer exists because the two models are deliberately NOT the same shape.
// The proto is the client-facing contract (recursive content blocks, open enums,
// Struct-typed JSON); the domain types are what the agent loop and providers are
// written against (closed variants, a flat tool-result content type, real JSON).
// Mapping them in one file means the mismatches are enumerated in one place
// instead of rediscovered at each call site.
//
// Direction matters for trust. domain -> proto is total: every domain value has
// a wire counterpart, and the switches below have no `default:` so adding an
// enumerator without a mapping is a compile error. proto -> domain is partial:
// the input came off a socket, so it validates and degrades rather than
// asserting -- an unset oneof yields nullopt, an unrepresentable block is
// dropped with a warning.

#include <optional>
#include <string>

#include <google/protobuf/struct.pb.h>
#include <nlohmann/json.hpp>

#include "minimal/agent/session_event.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/llm/stream_event.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/stored_message.hpp"
#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/v1/common.pb.h"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

// --- JSON <-> google.protobuf.Struct -----------------------------------------
//
// Struct's number model is IEEE double -- it has no integer type. So a JSON
// integer crosses as a double and comes back as an integer again (integral
// doubles are narrowed on the way back), which keeps `{"offset": 3}` printing as
// `3` rather than `3.0` in prompts and logs. The cost is the mirror case: a
// deliberate float `3.0` also returns as `3`. That trade is right for this
// payload -- tool input is overwhelmingly integers (offsets, limits, timeouts)
// and every consumer reads it through nlohmann's numeric coercion.
//
// Only objects can be a Struct. A non-object json (an array or a bare scalar)
// is not representable and yields an empty Struct; tool input is an object by
// schema, so this is a wire-validation case, not a lossy path in practice.
google::protobuf::Struct JsonToStruct(const nlohmann::json& value);
nlohmann::json StructToJson(const google::protobuf::Struct& value);

// The value-level pair, exposed because arrays and scalars nest inside a Struct
// even though they cannot be one.
google::protobuf::Value JsonToValue(const nlohmann::json& value);
nlohmann::json ValueToJson(const google::protobuf::Value& value);

// --- Enums -------------------------------------------------------------------

v1::StopReason ToProto(llm::StopReason value);
llm::StopReason FromProto(v1::StopReason value);

v1::Role ToProto(llm::Role value);
llm::Role FromProto(v1::Role value);

v1::ToolCategory ToProto(tools::ToolCategory value);
tools::ToolCategory FromProto(v1::ToolCategory value);

v1::LlmErrorCategory ToProto(llm::LlmErrorCategory value);
llm::LlmErrorCategory FromProto(v1::LlmErrorCategory value);

v1::PermissionDecision ToProto(tools::PermissionDecision value);
tools::PermissionDecision FromProto(v1::PermissionDecision value);

v1::PermissionRunMode ToProto(tools::PermissionRunMode value);
// nullopt for UNSPECIFIED, which means "use permissions.json default_run_mode".
// Returning the fallback here instead would hide the distinction from the one
// caller that has the config to resolve it.
std::optional<tools::PermissionRunMode> FromProto(v1::PermissionRunMode value);

// --- Messages and content blocks ---------------------------------------------

v1::Usage ToProto(const llm::Usage& value);
llm::Usage FromProto(const v1::Usage& value);

v1::ContentBlock ToProto(const llm::ContentBlock& block);
// nullopt when the oneof is unset -- a default-constructed block off the wire.
std::optional<llm::ContentBlock> FromProto(const v1::ContentBlock& block);

v1::Message ToProto(const llm::Message& message);
llm::Message FromProto(const v1::Message& message);

// --- Stream events -----------------------------------------------------------

v1::StreamEvent ToProto(const llm::StreamEvent& event);
// nullopt when the oneof is unset.
std::optional<llm::StreamEvent> FromProto(const v1::StreamEvent& event);

// --- Errors ------------------------------------------------------------------

v1::ErrorEvent ToProto(const llm::LlmError& error);

// --- Session events ------------------------------------------------------------
//
// The one entry point through which agent::SessionEvent ever becomes a wire
// message. `session_id` is threaded in rather than carried on the domain
// event, matching every other agent-side payload (SessionObserver's
// SessionInfo does the same) -- ServerEvent.session_id is a transport
// concern the domain type has no reason to know about.

v1::ServerEvent ToProto(const std::string& session_id, const agent::SessionEvent& event);

// --- Permission round-trip ---------------------------------------------------

// Fills tool_name/category/tool_input/summary and the disclosure's grants and
// `complete` flag. `request_id` is the caller's to set: it is generated by
// PendingRequests, which owns correlation.
v1::PermissionRequest ToProto(const tools::PermissionRequest& request);

// `granted_keys` is filtered against `offered`: a client may only confirm grants
// the server actually offered. The AUTHORITY for that rule is ToolExecutor,
// which re-filters every response it receives -- wire answers AND plugin
// advisor answers -- before remembering any key, and is the one place the rule
// is enforced for all checker kinds. The filter here is the wire's
// defense-in-depth: it drops minted keys at the boundary and logs them, so a
// hostile or buggy client is visible instead of silently tolerated.
tools::PermissionResponse FromProto(const v1::PermissionResponse& response,
                                    const tools::PermissionDisclosure& offered);

// --- Stored transcript (scrollback) ------------------------------------------
//
// One-way by design. These types come out of SQLite and go to the client; there
// is no wire path that writes them, so a FromProto would be an unused inverse
// that could drift without anything noticing.

v1::StoredMessage ToProto(const session::StoredMessage& stored);
v1::CompactionEvent ToProto(const session::CompactionRecord& record);
v1::SessionSummary ToProto(const session::SessionSummary& summary);

// CommandArgs: already structured on the wire, so this is a straight copy rather
// than a parse. Splitting a command line is the CLIENT's job -- it is the side
// that has the raw text a human typed, and the proto carries the result.
commands::CommandArgs FromProto(const v1::CommandArgs& args);

// Fills status/message/display/view_hint; `request_id` is the caller's to set
// (the stream path echoes the invocation's, the sessionless unary leaves it
// empty -- a synchronous response needs no correlation).
void FillProto(v1::CommandResult& out, const commands::CommandResult& result);

}  // namespace minimal::rpc
