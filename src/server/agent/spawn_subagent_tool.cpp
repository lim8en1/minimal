#include "minimal/agent/spawn_subagent_tool.hpp"

#include <algorithm>
#include <utility>

#include "minimal/agent/agent_preset.hpp"

namespace minimal::agent {
namespace {

tools::ToolSpec MakeSpec() {
    tools::ToolSpec spec;
    spec.name = kSpawnSubagentToolName;
    // Execute: spawning another agent that will itself run tools is an act the
    // operator approves, exactly like running a command.
    spec.category = tools::ToolCategory::Execute;
    spec.definition.name = kSpawnSubagentToolName;
    spec.definition.description =
        "Run a task in a child agent session with its own context; only its final "
        "message comes back to you. Use blocking when you need the result to "
        "continue, non-blocking to keep working while it runs (you are told when it "
        "finishes).";
    spec.definition.input_schema = nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"task",
           {{"type", "string"},
            {"description",
             "The complete, self-contained task for the child. It sees nothing "
             "else you have, so include every relevant path, name, and "
             "constraint."}}},
          {"agent_preset",
           {{"type", "string"},
            {"description", "Agent preset for the child. Defaults to your own."}}},
          {"blocking",
           {{"type", "boolean"},
            {"description",
             "true (default): wait for the child and return its final message as "
             "this tool's result. false: return immediately; the result arrives "
             "later."}}},
          {"max_turns",
            {{"type", "integer"},
             {"description",
              "Upper bound on the child's tool rounds. 0 (default) means no "
              "bound."}}},
           {"model",
            {{"type", "string"},
             {"description",
              "Model for the child as \"<provider>/<model>\". Defaults to the "
              "parent's own model (unless the child's preset declares one). A "
              "child starts with a fresh context, so this switches models "
              "without re-reading your whole history."}}}}},
         {"required", nlohmann::json::array({"task"})},
    };
    return spec;
}

}  // namespace

PresetToolDefinitionBuilder MakeSpawnDefinitionBuilder() {
    // The enum and the listing are driven by ONE filter -- the preset's
    // resolved allowed_subagents over the registry's spawnable scan -- so the
    // description can never advertise a name the input schema rejects, nor
    // the schema accept a name enforcement refuses. nullopt (field absent or
    // "*") filters nothing.
    return [](const PresetToolContext& ctx) {
        llm::ToolDefinition def = MakeSpec().definition;

        std::vector<const AgentPresetSummary*> allowed;
        for (const auto& summary : ctx.env.available_agents) {
            if (!ctx.preset.allowed_subagents.has_value() ||
                std::find(ctx.preset.allowed_subagents->begin(),
                          ctx.preset.allowed_subagents->end(),
                          summary.name) != ctx.preset.allowed_subagents->end()) {
                allowed.push_back(&summary);
            }
        }

        // The default line names the preset KEY (preset_key), not the
        // frontmatter display name -- spawn requests address presets by key,
        // and a display name that differs would send the model calling with
        // an enum value that does not exist.
        const bool self_spawnable =
            !ctx.preset.allowed_subagents.has_value() ||
            std::find(ctx.preset.allowed_subagents->begin(),
                      ctx.preset.allowed_subagents->end(),
                      ctx.preset_key) != ctx.preset.allowed_subagents->end();
        std::string description = def.description;
        if (self_spawnable) {
            description += "\n\nDefault subagent preset: `" + ctx.preset_key + "`" +
                           (ctx.preset.description.empty()
                                ? ""
                                : " (" + ctx.preset.description + ")");
        }
        if (!allowed.empty()) {
            description += "\n\nAvailable presets:";
            for (const AgentPresetSummary* summary : allowed) {
                description += "\n- `" + summary->name + "`" +
                               (summary->description.empty()
                                    ? ""
                                    : " (" + summary->description + ")");
            }
            nlohmann::json names = nlohmann::json::array();
            for (const AgentPresetSummary* summary : allowed) {
                names.push_back(summary->name);
            }
            def.input_schema["properties"]["agent_preset"]["enum"] = std::move(names);
        }
        def.description = std::move(description);
        return def;
    };
}

SpawnSubagentTool::SpawnSubagentTool(Runner runner)
    : runner_(std::move(runner)), spec_(MakeSpec()) {}

tools::ToolResult SpawnSubagentTool::execute(const nlohmann::json& input,
                                             tools::ToolContext& ctx) {
    SubagentRequest request;

    if (!input.is_object() || !input.contains("task") || !input.at("task").is_string()) {
        return {true, "spawn_subagent requires \"task\" as a string"};
    }
    request.task = input.at("task").get<std::string>();
    if (request.task.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {true, "spawn_subagent requires a non-empty \"task\""};
    }

    if (input.contains("agent_preset")) {
        if (!input.at("agent_preset").is_string()) {
            return {true, "spawn_subagent \"agent_preset\" must be a string"};
        }
        request.agent_preset = input.at("agent_preset").get<std::string>();
    }
    if (input.contains("blocking")) {
        if (!input.at("blocking").is_boolean()) {
            return {true, "spawn_subagent \"blocking\" must be a boolean"};
        }
        request.blocking = input.at("blocking").get<bool>();
    }
    if (input.contains("max_turns")) {
        const auto& value = input.at("max_turns");
        if (!value.is_number_integer() || value.get<int>() < 0) {
            return {true, "spawn_subagent \"max_turns\" must be a non-negative integer"};
        }
        request.max_turns = value.get<int>();
    }
    if (input.contains("model")) {
        if (!input.at("model").is_string()) {
            return {true, "spawn_subagent \"model\" must be a \"<provider>/<model>\" string"};
        }
        // SplitModelRef, not ParseModelRef: discovered ids can carry slashes,
        // and the tolerant split is what /model accepts too.
        auto model = common::SplitModelRef(input.at("model").get<std::string>());
        if (!model) {
            return {true, "spawn_subagent \"model\" must be \"<provider>/<model>\" (got \"" +
                              input.at("model").get<std::string>() + "\")"};
        }
        request.model = std::move(*model);
    }

    const SubagentOutcome outcome = runner_(request, ctx);
    switch (outcome.kind) {
        case SubagentOutcome::Kind::kCompleted:
            // The child's whole result is its final text. Empty is reported as
            // such rather than passed through: a silent empty block would read
            // like success.
            if (outcome.final_text.empty()) {
                return {true, "subagent finished without producing any text"};
            }
            return {false, outcome.final_text};
        case SubagentOutcome::Kind::kStarted:
            return {false, "subagent " + outcome.child_session_id +
                               " started; its result will be delivered when it "
                               "finishes"};
        case SubagentOutcome::Kind::kError:
            return {true, outcome.error};
    }
    return {true, "subagent spawn failed"};
}

tools::PermissionDisclosure SpawnSubagentTool::Disclose(const nlohmann::json& input,
                                                        const tools::ToolContext& ctx) const {
    (void)ctx;  // nothing session-dependent to disclose: a spawn names no paths
    tools::PermissionDisclosure disclosure;
    std::string task;
    if (input.is_object() && input.contains("task") && input.at("task").is_string()) {
        task = input.at("task").get<std::string>();
    }
    // One line of the task: enough to judge the spawn, and a prompt cannot
    // carry the whole thing anyway.
    const auto newline = task.find_first_of("\r\n");
    if (newline != std::string::npos) task.resize(newline);
    constexpr std::size_t kMaxTaskPreview = 100;
    if (task.size() > kMaxTaskPreview) task = task.substr(0, kMaxTaskPreview) + "...";

    disclosure.summary = std::string("spawn subagent") + (task.empty() ? "" : ": " + task);
    disclosure.complete = true;
    return disclosure;
}

}  // namespace minimal::agent
