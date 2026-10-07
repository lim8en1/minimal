#pragma once

// Pure helpers for sizing what goes into and comes out of a model round: the
// tool-result and display ceilings, the output-cap clamp, and the compaction
// last-resort that elides one oversized result. No state, no loop dependency --
// split out of agent_loop.cpp so each can be read (and tested) on its own.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <optional>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/tools/tool_output_spill.hpp"

namespace minimal::agent {

// The tool_use blocks of an assistant message, in block order (pointers into
// `message`).
std::vector<const llm::ToolUseBlock*> ToolUsesOf(const llm::Message& message);

// THE global tool-RESULT ceiling (settings.json tools.max_result_bytes), the
// output-side mirror of the input ceiling checked before Prepare. Applied once,
// after Execute() returns, so it covers every tool uniformly -- a core tool's
// own cap (bash.max_output_bytes) is the plugin's choice, not a harness
// guarantee, and an MCP tool or third-party plugin tool has no cap at all short
// of this one. Truncation, not an error: a large result is not a refusal the
// model needs to adapt its call to, just content it should know was cut.
// `max_bytes <= 0` means no ceiling. The start and the end are kept; with a
// `spill` the whole text is saved there first and the note names the file.
void CapToolResult(llm::ToolResultBlock& block, std::int64_t max_bytes,
                   tools::ToolOutputSpill* spill = nullptr);

// Shrinks a tool round's results so that, together, they add at most
// `room_bytes` of text (RequestBuilder::ToolRoundRoomBytes). The room is
// shared fairly: results under the fair share keep everything, the larger
// ones are cut to it (never below kMinFittedResultBytes) with CapToolResult's
// shape, the whole text spilled through `spill_for(tool_use_id)`. Returns how
// many results were cut.
std::size_t FitRoundResults(
    std::vector<llm::ContentBlock>& results, std::int64_t room_bytes,
    const std::function<tools::ToolOutputSpill(const std::string& tool_use_id)>& spill_for);

// The display payload's ceiling (settings.json tools.max_display_bytes).
// DROPPED whole over the cap, never truncated: a cut diff or file excerpt
// misrepresents what happened, while the text fallback stays truthful. Returns
// the serialized size when it dropped the payload, 0 otherwise.
std::size_t CapToolDisplay(llm::ToolResultBlock& block, std::int64_t max_bytes);

// The output cap a request can actually carry. Backends validate
// input + max_tokens <= context window, and a catalog-reported cap
// (OpenRouter's top_provider.max_completion_tokens) can be 90% of the window --
// sent verbatim, every turn past ~10% occupancy was a hard 400 that no retry or
// compaction could clear. Clamped to the room left after the input estimate and
// a margin for the bytes/4 roughness; floored so a near-full context still
// sends a usable request rather than a non-positive cap. No window or no cap:
// nothing to clamp against, the cap passes through.
std::optional<int> FitOutputCap(std::optional<int> cap, std::optional<std::int64_t> window,
                                std::int64_t input_tokens);

// S4's no-boundary fallback: when snap_compaction_boundary finds no safe cut
// anywhere (every candidate lands inside an open tool-call cycle), context would
// otherwise grow forever -- but the harm that trips the trigger is usually SIZE,
// not message count, and a single oversized tool result is the common cause.
// Eliding one IN PLACE, reusing CapToolResult's own truncation shape, shrinks
// the context without removing any message, so it can never disturb
// tool_use/tool_result pairing. Returns false when nothing large enough to be
// worth eliding remains, which is the caller's signal to fall back to the
// genuine last resort.
//
// DIVERGENCE NOTE: unlike the structural path, elision mutates only the working
// history -- the persisted transcript keeps the full bytes (there is no
// message-rewrite API, and no summary to anchor a checkpoint to). A resume
// therefore regrows the elided content; that self-heals (the resume re-seeds
// occupancy, the trigger re-fires, this path runs again) at the cost of one
// oversized request between resume and the next compaction cycle.
bool ElideLargestToolResult(std::vector<llm::Message>& history);

}  // namespace minimal::agent
