#pragma once

// The core entry point into the subagent loop (ARCHITECTURE.md, "Core vs.
// plugin surface": making it a plugin would mean exposing session CREATION
// across the ABI to relocate a thin wrapper). The tool itself is deliberately
// thin: validate the model's input, hand it to an injected runner, and map the
// outcome onto a ToolResult. All of the real machinery -- session creation,
// permission inheritance, the blocking wait, the completion injection -- lives
// behind the runner, which is what keeps the agent-loop test tier able to
// script both subagent paths against fakes.

#include <functional>
#include <string>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/subagent_spawn.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::agent {

// The preset-scoped half of the spawn tool's presentation: the static base
// schema with (a) the preset's own default named in the description -- omitted
// when the preset does not allow spawning ITSELF, since enforcement would
// refuse the very default being advertised -- (b) the allowed-subagent
// listing (the registry's spawnable scan, filtered by the preset's
// allowed_subagents), and (c) a JSON-schema enum of exactly the names a spawn
// request may carry, so the model cannot hallucinate a preset name the
// enforcement below would refuse. Registered on the AgentPresetRegistry by
// main.cpp, beside the tool's own registration; runs once per resolved preset
// and the result rides AgentPreset::tool_definition_overrides.
PresetToolDefinitionBuilder MakeSpawnDefinitionBuilder();

class SpawnSubagentTool : public tools::Tool {
public:
    // The runner receives the context as well as the request: the parent is
    // whoever is executing the tool (ctx.session_id), the completion injection
    // is keyed to this very invocation (ctx.tool_use_id), and a blocking spawn
    // must observe the parent's cancellation (ctx.is_cancelled).
    using Runner = std::function<SubagentOutcome(const SubagentRequest&,
                                                 const tools::ToolContext&)>;

    explicit SpawnSubagentTool(Runner runner);

    const tools::ToolSpec& spec() const override { return spec_; }
    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override;

    // Execute-gated, so a human approves each spawn; showing the task's first
    // line is what makes that approval informed. No grants offered: a spawn is
    // a one-off act, not a repeatable binary.
    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const tools::ToolContext& ctx) const override;

private:
    Runner runner_;
    tools::ToolSpec spec_;
};

}  // namespace minimal::agent
