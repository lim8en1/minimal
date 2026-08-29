#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "minimal/llm/chat.hpp"
#include "minimal/tools/permission.hpp"

namespace minimal::tools {

struct ToolSpec {
    std::string name;
    ToolCategory category;
    llm::ToolDefinition definition;
};

struct ToolResult {
    bool is_error = false;
    std::string content_text;
};

struct ToolContext {
    std::string session_id;
    std::filesystem::path project_root;

    // Checked before each tool runs and polled by long-running tools (bash).
    std::function<bool()> is_cancelled;

    // Incremental output, surfaced as ToolOutputDeltaEvent. Deliberately lossy
    // downstream: the server coalesces and sends only the tail that fits the
    // client's ring buffer, while the authoritative record stays the capped
    // ToolResultBlock. Two artifacts, two jobs.
    std::function<void(std::string_view)> emit_output;

    // TODO: Logger& for bash's per-invocation audit log, once that tool lands.
};

class Tool {
public:
    virtual ~Tool() = default;

    virtual const ToolSpec& spec() const = 0;
    virtual ToolResult execute(const nlohmann::json& input, ToolContext& ctx) = 0;

    // Declarative opt-in: a tool whose input carries a filesystem path sets this
    // true and implements ExtractPath(). It never calls path_confinement itself
    // for *classification* -- ToolExecutor does that centrally, so the
    // out-of-workdir rule cannot drift between tools. Bash leaves both at their
    // defaults: there is no sound way to know which paths an arbitrary shell
    // command touches without parsing shell semantics.
    virtual bool path_confinement_supported() const { return false; }
    virtual std::optional<std::string> ExtractPath(const nlohmann::json& input) const {
        (void)input;
        return std::nullopt;
    }

    // Overridden only where one tool spans categories -- e.g. Edit's `view`
    // reports ReadOnly though the tool's static category is FileWrite.
    virtual std::optional<ToolCategory> category_for(const nlohmann::json& input) const {
        (void)input;
        return std::nullopt;
    }
};

}  // namespace minimal::tools
