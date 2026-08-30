#include "minimal/agent/compaction_trigger.hpp"

namespace minimal::agent {

std::int64_t resolve_compaction_trigger(const config::CompactionTriggerSettings& trigger,
                                        std::optional<std::int64_t> context_window_tokens) {
    using Kind = config::CompactionTriggerSettings::Kind;
    switch (trigger.kind) {
        case Kind::Never:
            return 0;
        case Kind::FixedTokens:
            return trigger.tokens;
        case Kind::PercentOfWindow:
            if (!context_window_tokens) return 0;
            return *context_window_tokens * trigger.percent / 100;
        case Kind::WindowBuffer: {
            if (!context_window_tokens) return 0;
            const std::int64_t threshold = *context_window_tokens - trigger.buffer_tokens;
            return threshold > 0 ? threshold : 0;
        }
    }
    return 0;
}

}  // namespace minimal::agent
