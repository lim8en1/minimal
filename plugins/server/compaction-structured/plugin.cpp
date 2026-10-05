// compaction-structured -- the "structured" compaction strategy: the
// summarizer's output is a record_summary tool call parsed into typed
// sections (task, done, next steps, ...), not free text.
//
//   register_compaction_strategies  "structured" (core's cut selection, this
//                                   summarizer, record_summary as its request
//                                   tool -- core keeps it on every request for
//                                   the prompt cache and answers stray calls)
//
// The instruction is <config dir>/prompts/compression-structured.md; missing,
// the strategy runs as core's free-text summarizer. Disabled, settings or a
// preset naming "structured" warns at startup and falls back to "summarize".

#include <string>

#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/frontmatter.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "structured_compaction.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("compaction-structured")

void minimal_plugin_register_compaction_strategies(
    minimal::agent::CompactionStrategyRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    const auto path = minimal::common::ConfigDir() / "prompts" / "compression-structured.md";
    std::string instruction =
        minimal::config::parse_frontmatter_markdown(minimal::common::ReadFileOrEmpty(path)).body;
    if (instruction.find_first_not_of(" \t\r\n") == std::string::npos) {
        minimal::common::DefaultLogger().warn(
            "compaction-structured: {} not found or empty -- the structured strategy runs as "
            "free text",
            path.string());
    }
    registry.Register("structured", minimal::compaction_structured::MakeStructuredCompactionStrategy(
                                        std::move(instruction)));
}

}  // extern "C"
