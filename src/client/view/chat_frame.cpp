#include "minimal/client/view/chat_frame.hpp"

#include <memory>
#include <string>

#include "minimal/client/view/tab_activity.hpp"

namespace minimal::client {

namespace {

plugins::ChatLiveSegment::Kind KindOf(LiveTurnSegment::Kind kind) {
    switch (kind) {
        case LiveTurnSegment::Kind::Text: return plugins::ChatLiveSegment::Kind::Text;
        case LiveTurnSegment::Kind::Tool: return plugins::ChatLiveSegment::Kind::Tool;
        case LiveTurnSegment::Kind::Thinking: return plugins::ChatLiveSegment::Kind::Thinking;
    }
    return plugins::ChatLiveSegment::Kind::Text;
}

}  // namespace

plugins::ChatFrame ChatFrameFrom(const SessionState::Snapshot& snapshot) {
    plugins::ChatFrame frame;
    frame.session_id = snapshot.session_id;
    frame.transcript_version = snapshot.transcript_version;
    frame.rewind_epoch = snapshot.rewind_epoch;
    frame.scrollback_page = snapshot.scrollback_page;
    frame.compactions = snapshot.compactions;
    frame.has_earlier_messages = snapshot.has_earlier_messages;
    frame.scrollback_trimmed = snapshot.scrollback_trimmed;
    frame.scrollback_gap = snapshot.scrollback_gap;
    frame.model = snapshot.model;
    frame.agent_preset = snapshot.agent_preset;
    frame.desired_model = snapshot.desired_model;
    frame.desired_agent_preset = snapshot.desired_agent_preset;
    for (const auto& pending : snapshot.pending_user_messages) {
        frame.pending_user_messages.push_back({pending.turn_id, pending.text});
    }
    for (const auto& segment : snapshot.live_segments) {
        plugins::ChatLiveSegment out;
        out.kind = KindOf(segment.kind);
        out.segment_id = segment.segment_id;
        out.text_truncated = segment.text_truncated;
        if (segment.text_view) out.text = segment.text_view;
        if (segment.kind == LiveTurnSegment::Kind::Tool) {
            // The live row while it runs; once the turn swept it, the final
            // state the marker carries until the transcript catches up --
            // rendering nothing in that window made the calls vanish.
            const ToolExecutionState* tool = nullptr;
            for (const auto& candidate : snapshot.tools) {
                if (candidate.tool_use_id == segment.tool_use_id) {
                    tool = &candidate;
                    break;
                }
            }
            if (tool == nullptr) tool = segment.final_tool.get();
            if (tool != nullptr) out.tool = ToolActivityOf(*tool);
        }
        frame.live_segments.push_back(std::move(out));
    }
    frame.streaming_finalized = snapshot.is_streaming_finalized;
    frame.turn_busy = snapshot.state == SessionStateKind::Thinking ||
                      snapshot.state == SessionStateKind::Working ||
                      snapshot.state == SessionStateKind::Waiting;
    return frame;
}

}  // namespace minimal::client
