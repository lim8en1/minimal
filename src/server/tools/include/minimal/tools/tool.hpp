#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
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
    // The logger is a reference, so it must come from somewhere at construction;
    // the rest is filled in by whoever builds the context.
    explicit ToolContext(common::Logger& logger) : logger(logger) {}

    std::string session_id;
    std::filesystem::path project_root;

    // Checked before each tool runs and polled by long-running tools (bash).
    std::function<bool()> is_cancelled;

    // Incremental output, surfaced as ToolOutputDeltaEvent. Deliberately lossy
    // downstream: the server coalesces and sends only the tail that fits the
    // client's ring buffer, while the authoritative record stays the capped
    // ToolResultBlock. Two artifacts, two jobs.
    std::function<void(std::string_view)> emit_output;

    // Per-invocation audit logging (bash logs command, resolved argv or
    // rejection reason, exit code, duration). One process-wide instance for now.
    common::Logger& logger;
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

    // What an `Ask` prompt should show, and which session grants it may offer.
    // Takes the context because the answer is session-dependent: bash resolves
    // argv[0] against the session's PATH, so `git` may be /usr/bin/git in one
    // session and /tmp/git in another -- and showing the resolved path is the
    // whole point (see GrantOffer).
    //
    // The default -- name only, no grants -- reproduces today's behaviour: every
    // Ask prompts, nothing is ever remembered. A tool opts in by overriding.
    virtual PermissionDisclosure Disclose(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
        (void)input;
        (void)ctx;
        return PermissionDisclosure{spec().name, {}, true};
    }
};

}  // namespace minimal::tools
