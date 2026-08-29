#include "fs_tools.hpp"

#include <sstream>
#include <utility>

#include "file_editor.hpp"
#include "minimal/tools/atomic_write.hpp"
#include "minimal/tools/path_confinement.hpp"

namespace minimal::tools_fs {
namespace {

using tools::ConfineResult;
using tools::ToolCategory;
using tools::ToolContext;
using tools::ToolResult;
using tools::ToolSpec;

ToolResult Error(std::string message) { return ToolResult{true, std::move(message)}; }

nlohmann::json StringProperty(const char* description) {
    return nlohmann::json{{"type", "string"}, {"description", description}};
}

// Resolves the model-supplied path for OPERATION. ToolExecutor has already
// resolved it once to CLASSIFY permission -- different question, and a tool must
// not operate on a path it did not itself resolve.
ConfineResult ResolveForUse(const ToolContext& ctx, const nlohmann::json& input, bool must_exist,
                             std::string& error_out) {
    ConfineResult resolved;
    if (!input.contains("path") || !input.at("path").is_string()) {
        error_out = "missing required string field: path";
        return resolved;
    }
    resolved = tools::confine(ctx.project_root, input.at("path").get<std::string>(), must_exist);
    if (!resolved.ok) error_out = resolved.error;
    return resolved;
}

std::optional<std::string> ExtractPathField(const nlohmann::json& input) {
    if (!input.contains("path") || !input.at("path").is_string()) return std::nullopt;
    return input.at("path").get<std::string>();
}

// --- read -------------------------------------------------------------------

class ReadTool : public tools::Tool {
public:
    explicit ReadTool(FsSettings settings) : settings_(settings) {
        spec_.name = "read";
        spec_.category = ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Read a window of lines from a text file. Returns numbered lines; if the file is "
            "longer than the window, the footer states the total and the offset to continue from.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"path", StringProperty("File path, relative to the project root or absolute within it")},
              {"offset", {{"type", "integer"}, {"description", "1-based first line (default 1)"}}},
              {"limit", {{"type", "integer"}, {"description", "Maximum lines to return"}}}}},
            {"required", nlohmann::json::array({"path"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        return ExtractPathField(input);
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        std::string error;
        const auto resolved = ResolveForUse(ctx, input, /*must_exist=*/true, error);
        if (!error.empty()) return Error(error);

        const int offset = input.value("offset", 1);
        const int limit = input.value("limit", settings_.read_default_limit_lines);

        const auto window = tools::read_window(resolved.absolute_path, offset, limit,
                                                settings_.read_max_line_length);
        if (!window.ok) return Error(window.error);
        return ToolResult{false, RenderWindow(window)};
    }

private:
    ToolSpec spec_;
    FsSettings settings_;
};

// --- write ------------------------------------------------------------------

class WriteTool : public tools::Tool {
public:
    explicit WriteTool(FsSettings settings) : settings_(settings) {
        (void)settings_;
        spec_.name = "write";
        spec_.category = ToolCategory::FileWrite;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Create or overwrite a file with the given content. The parent directory must already "
            "exist.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"path", StringProperty("File path, relative to the project root or absolute within it")},
              {"content", StringProperty("Full file content")}}},
            {"required", nlohmann::json::array({"path", "content"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        return ExtractPathField(input);
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        std::string error;
        // must_exist=false: writing a new file is the common case.
        const auto resolved = ResolveForUse(ctx, input, /*must_exist=*/false, error);
        if (!error.empty()) return Error(error);

        if (!input.contains("content") || !input.at("content").is_string()) {
            return Error("missing required string field: content");
        }
        const auto content = input.at("content").get<std::string>();

        const auto outcome = tools::write_file(resolved.absolute_path, content);
        if (!outcome.ok) return Error(outcome.error);

        return ToolResult{false, (outcome.created ? "Created " : "Wrote ") +
                                      resolved.absolute_path.string() + " (" +
                                      std::to_string(outcome.bytes_written) + " bytes)"};
    }

private:
    ToolSpec spec_;
    FsSettings settings_;
};

// --- edit -------------------------------------------------------------------

class EditTool : public tools::Tool {
public:
    explicit EditTool(FsSettings settings) : settings_(settings) {
        spec_.name = "edit";
        spec_.category = ToolCategory::FileWrite;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "View or modify a file. Commands: view (show lines), create (write a new file), "
            "str_replace (replace one unique string), insert (insert text after a line).";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"command",
               {{"type", "string"},
                {"enum", nlohmann::json::array({"view", "create", "str_replace", "insert"})}}},
              {"path", StringProperty("File path, relative to the project root or absolute within it")},
              {"view_range",
               {{"type", "array"},
                {"items", {{"type", "integer"}}},
                {"description", "[start, end] 1-based line range for view"}}},
              {"file_text", StringProperty("Full content, for create")},
              {"old_str", StringProperty("Exact string to replace; must occur exactly once")},
              {"new_str", StringProperty("Replacement string")},
              {"insert_line", {{"type", "integer"}, {"description", "0 = before the first line"}}},
              {"insert_text", StringProperty("Text to insert")}}},
            {"required", nlohmann::json::array({"command", "path"})}};
    }

    const ToolSpec& spec() const override { return spec_; }
    bool path_confinement_supported() const override { return true; }
    std::optional<std::string> ExtractPath(const nlohmann::json& input) const override {
        return ExtractPathField(input);
    }

    // `view` reads; the rest mutate. Reporting the narrower category means
    // viewing a file never raises a write prompt, which is the same trick that
    // keeps Read cheap.
    std::optional<ToolCategory> category_for(const nlohmann::json& input) const override {
        if (input.value("command", std::string{}) == "view") return ToolCategory::ReadOnly;
        return std::nullopt;
    }

    ToolResult execute(const nlohmann::json& input, ToolContext& ctx) override {
        const auto command = input.value("command", std::string{});
        if (command.empty()) return Error("missing required string field: command");

        const bool must_exist = (command != "create");
        std::string error;
        const auto resolved = ResolveForUse(ctx, input, must_exist, error);
        if (!error.empty()) return Error(error);

        if (command == "view") return View(input, resolved.absolute_path);
        if (command == "create") return Create(input, resolved.absolute_path);
        if (command == "str_replace") return StrReplace(input, resolved.absolute_path);
        if (command == "insert") return Insert(input, resolved.absolute_path);
        return Error("unknown command: " + command);
    }

private:
    ToolResult View(const nlohmann::json& input, const std::filesystem::path& path) {
        int offset = 1;
        int limit = settings_.read_default_limit_lines;
        if (auto it = input.find("view_range");
            it != input.end() && it->is_array() && it->size() == 2) {
            offset = (*it)[0].get<int>();
            const int end = (*it)[1].get<int>();
            limit = (end < offset) ? 0 : end - offset + 1;
        }
        const auto window = tools::read_window(path, offset, limit, settings_.read_max_line_length);
        if (!window.ok) return Error(window.error);
        return ToolResult{false, RenderWindow(window)};
    }

    ToolResult Create(const nlohmann::json& input, const std::filesystem::path& path) {
        if (!input.contains("file_text") || !input.at("file_text").is_string()) {
            return Error("missing required string field: file_text");
        }
        // Overwrites, matching text_editor_20250728's `create`. Diverging would
        // defeat the point of adopting that vocabulary, and it is FileWrite-gated
        // either way.
        const auto outcome = tools::write_file(path, input.at("file_text").get<std::string>());
        if (!outcome.ok) return Error(outcome.error);
        return ToolResult{false, (outcome.created ? "Created " : "Overwrote ") + path.string()};
    }

    ToolResult StrReplace(const nlohmann::json& input, const std::filesystem::path& path) {
        if (!input.contains("old_str") || !input.at("old_str").is_string()) {
            return Error("missing required string field: old_str");
        }
        const auto old_str = input.at("old_str").get<std::string>();
        const auto new_str = input.value("new_str", std::string{});
        const auto outcome =
            FileEditor::StrReplace(path, old_str, new_str, settings_.edit_max_editable_file_bytes);
        if (!outcome.ok) return Error(outcome.error);
        return ToolResult{false, outcome.summary};
    }

    ToolResult Insert(const nlohmann::json& input, const std::filesystem::path& path) {
        if (!input.contains("insert_line") || !input.at("insert_line").is_number_integer()) {
            return Error("missing required integer field: insert_line");
        }
        if (!input.contains("insert_text") || !input.at("insert_text").is_string()) {
            return Error("missing required string field: insert_text");
        }
        const auto outcome =
            FileEditor::Insert(path, input.at("insert_line").get<int>(),
                                input.at("insert_text").get<std::string>(),
                                settings_.edit_max_editable_file_bytes);
        if (!outcome.ok) return Error(outcome.error);
        return ToolResult{false, outcome.summary};
    }

    ToolSpec spec_;
    FsSettings settings_;
};

}  // namespace

std::string RenderWindow(const tools::FileWindow& window) {
    if (window.lines.empty()) {
        return "[File has " + std::to_string(window.total_lines) +
                " line(s); the requested range is past the end.]";
    }

    std::ostringstream out;
    int line_number = window.start_line;
    for (const auto& line : window.lines) {
        out << line_number++ << '\t' << line << '\n';
    }

    // The pagination affordance has to be textual: the model sees only text, so
    // there is no structured channel to signal "there is more" through.
    if (window.truncated) {
        out << "\n[Showing lines " << window.start_line << '-' << window.end_line << " of "
            << window.total_lines << ". Pass offset=" << (window.end_line + 1) << " to continue.]";
    }
    return out.str();
}

std::unique_ptr<tools::Tool> MakeReadTool(FsSettings settings) {
    return std::make_unique<ReadTool>(settings);
}
std::unique_ptr<tools::Tool> MakeWriteTool(FsSettings settings) {
    return std::make_unique<WriteTool>(settings);
}
std::unique_ptr<tools::Tool> MakeEditTool(FsSettings settings) {
    return std::make_unique<EditTool>(settings);
}

}  // namespace minimal::tools_fs
