#include "checklist_tool.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::checklist {
namespace {

// Shared by OpSet (bulk replace) and OpUpdate (single-item edit) so the two
// enforcement points can't silently diverge.
constexpr std::size_t kMaxItemTextBytes = 4000;

tools::ToolResult Error(const std::string& message) {
    return tools::ToolResult{true, message};
}

const nlohmann::json& RequireObject(const nlohmann::json& input) {
    static const nlohmann::json empty = nlohmann::json::object();
    return input.is_object() ? input : empty;
}

// "" when `text` is an acceptable item text, else the error to return. One
// line per item: a newline would break both the model's markdown list and the
// client panel's one-row-per-item layout.
std::string TextError(const std::string& text) {
    if (text.empty()) return "checklist: item text must not be empty";
    if (text.size() > kMaxItemTextBytes) {
        return "checklist: item text is capped at " + std::to_string(kMaxItemTextBytes) + " bytes";
    }
    if (text.find_first_of("\r\n") != std::string::npos) {
        return "checklist: item text must be a single line";
    }
    return {};
}

// An optional "status" field of `object`: `status` stays nullopt when the
// field is absent; `error` is non-empty when it is present but invalid.
struct StatusField {
    std::optional<ItemStatus> status;
    std::string error;
};

StatusField ReadStatus(const nlohmann::json& object) {
    StatusField field;
    if (!object.contains("status")) return field;
    if (!object.at("status").is_string()) {
        field.error = "checklist: 'status' must be a string";
        return field;
    }
    const auto name = object.at("status").get<std::string>();
    field.status = ParseStatus(name);
    if (!field.status) {
        field.error = "checklist: unknown status \"" + name +
                      "\" (expected pending, in_progress or completed)";
    }
    return field;
}

}  // namespace

namespace {

class ChecklistTool : public tools::Tool {
public:
    ChecklistTool(std::shared_ptr<ChecklistStore> store, ChecklistHub* hub)
        : store_(std::move(store)), hub_(hub) {
        spec_.name = kPluginName;
        spec_.category = tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Maintain this session's task checklist. Track multi-step work: create the full "
            "list up front with op=set, then mark items in_progress before starting and "
            "completed as soon as each finishes. The list is visible to the user and "
            "survives context compaction.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"op",
               {{"type", "string"},
                {"enum", {"set", "update", "view"}},
                {"description", "set: replace the whole list. update: change one item's "
                                "status or text. view: read the current list."}}},
              {"items",
               {{"type", "array"},
                {"description", "op=set only. Full replacement list in execution order."},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"text",
                     {{"type", "string"}, {"description", "What has to happen (one line)"}}},
                    {"status",
                     {{"type", "string"},
                      {"enum", {"pending", "in_progress", "completed"}},
                      {"description", "Defaults to pending"}}}}},
                  {"required", nlohmann::json::array({"text"})}}}}},
              {"id",
               {{"type", "integer"},
                {"description", "op=update: the item's number as shown in the list "
                                "(1..N in list order; op=set renumbers from 1)"}}},
              {"status",
               {{"type", "string"},
                {"enum", {"pending", "in_progress", "completed"}},
                {"description", "op=update: new status"}}},
              {"text", {{"type", "string"}, {"description", "op=update: new text"}}}}},
            {"required", nlohmann::json::array({"op"})}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    // op=update is a Load -> modify -> Save over the session's one row, and a
    // round that carries set + update must apply them in the order the model
    // wrote them. Sequential dispatch gives both (tool.hpp): same-session
    // calls run one at a time in assistant-block order. Different sessions
    // write different keys, and the RPC path only reads.
    bool parallel_safe() const override { return false; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const auto& body = RequireObject(input);
        if (!body.contains("op") || !body.at("op").is_string()) {
            return Error("checklist: 'op' must be one of \"set\", \"update\", \"view\"");
        }
        const auto op = body.at("op").get<std::string>();

        if (ctx.session_id.empty()) {
            return Error("checklist: no session to store the list under");
        }

        try {
            if (op == "set") return OpSet(body, ctx);
            if (op == "update") return OpUpdate(body, ctx);
            if (op == "view") return OpView(ctx);
            return Error("checklist: unknown op \"" + op +
                         "\" (expected \"set\", \"update\" or \"view\")");
        } catch (const std::exception& error) {
            // Storage I/O and decode failures surface as error results, the
            // same way every tool failure does -- never out of execute().
            return Error(std::string("checklist: ") + error.what());
        }
    }

private:
    // A stored row that fails to decode names the way out: op=set never
    // reads the old row, so it replaces a corrupt one.
    std::vector<Item> Current(const tools::ToolContext& ctx) const {
        try {
            return store_->Load(ctx.session_id);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + " (op=set replaces the list)");
        }
    }

    tools::ToolResult Save(const tools::ToolContext& ctx, const std::vector<Item>& items) {
        store_->Save(ctx.session_id, items);
        if (hub_ != nullptr) hub_->MarkDirty(ctx.session_id);
        return Reply(items);
    }

    tools::ToolResult Reply(const std::vector<Item>& items) const {
        return tools::ToolResult{false, RenderMarkdown(items), nlohmann::json{{"kind", "markdown"}}};
    }

    tools::ToolResult OpSet(const nlohmann::json& body, const tools::ToolContext& ctx) {
        if (!body.contains("items")) {
            return Error("checklist: op=set requires an 'items' array (empty clears the list)");
        }
        const auto& entries = body.at("items");
        if (!entries.is_array()) {
            return Error("checklist: 'items' must be an array");
        }
        // The list is one KV row: without a cap, a runaway model could store
        // an unbounded blob under its own session key.
        constexpr std::size_t kMaxItems = 200;
        if (entries.size() > kMaxItems) {
            return Error("checklist: 'items' is capped at " + std::to_string(kMaxItems) +
                         " entries");
        }

        std::vector<Item> items;
        items.reserve(entries.size());
        std::int64_t next_id = 1;
        for (const auto& entry : entries) {
            if (!entry.is_object() || !entry.contains("text") || !entry.at("text").is_string()) {
                return Error("checklist: every item needs a string 'text'");
            }
            Item item;
            item.id = next_id++;
            item.text = entry.at("text").get<std::string>();
            if (auto error = TextError(item.text); !error.empty()) return Error(error);
            auto status = ReadStatus(entry);
            if (!status.error.empty()) return Error(status.error);
            item.status = status.status.value_or(ItemStatus::Pending);
            items.push_back(std::move(item));
        }

        return Save(ctx, items);
    }

    tools::ToolResult OpUpdate(const nlohmann::json& body, const tools::ToolContext& ctx) {
        if (!body.contains("id") || !body.at("id").is_number_integer()) {
            return Error("checklist: op=update requires an integer item 'id'");
        }
        const auto id = body.at("id").get<std::int64_t>();

        const auto status = ReadStatus(body);
        if (!status.error.empty()) return Error(status.error);
        std::optional<std::string> text;
        if (body.contains("text")) {
            if (!body.at("text").is_string()) {
                return Error("checklist: 'text' must be a string");
            }
            text = body.at("text").get<std::string>();
            if (auto error = TextError(*text); !error.empty()) return Error(error);
        }
        if (!status.status && !text) {
            return Error("checklist: op=update needs a new 'status' or a new 'text'");
        }

        auto items = Current(ctx);
        auto item = std::find_if(items.begin(), items.end(),
                                 [id](const Item& candidate) { return candidate.id == id; });
        if (item == items.end()) {
            return Error("checklist: no item with id " + std::to_string(id) +
                         " -- use op=view to see the current ids");
        }
        if (status.status) item->status = *status.status;
        if (text) item->text = std::move(*text);

        return Save(ctx, items);
    }

    tools::ToolResult OpView(const tools::ToolContext& ctx) {
        if (!store_->available()) {
            return Error("checklist: plugin storage is not available in this host");
        }
        return Reply(Current(ctx));
    }

    tools::ToolSpec spec_;
    std::shared_ptr<ChecklistStore> store_;
    ChecklistHub* hub_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeChecklistTool(std::shared_ptr<ChecklistStore> store,
                                               ChecklistHub* hub) {
    return std::make_unique<ChecklistTool>(std::move(store), hub);
}

}  // namespace minimal::checklist
