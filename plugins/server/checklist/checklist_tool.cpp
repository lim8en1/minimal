#include "checklist_tool.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::checklist {
namespace {

tools::ToolResult Error(const std::string& message) {
    return tools::ToolResult{true, message};
}

const nlohmann::json& RequireObject(const nlohmann::json& input) {
    static const nlohmann::json empty = nlohmann::json::object();
    return input.is_object() ? input : empty;
}

}  // namespace

namespace {

class ChecklistTool : public tools::Tool {
public:
    explicit ChecklistTool(std::shared_ptr<ChecklistStore> store) : store_(std::move(store)) {
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
                     {{"type", "string"}, {"description", "What has to happen"}}},
                    {"status",
                     {{"type", "string"},
                      {"enum", {"pending", "in_progress", "completed"}},
                      {"description", "Defaults to pending"}}}}},
                  {"required", nlohmann::json::array({"text"})}}}}},
              {"id",
               {{"type", "integer"},
                {"description", "op=update: the item's id (ids run 1..N in list order; "
                                "op=set renumbers from 1)"}}},
              {"status",
               {{"type", "string"},
                {"enum", {"pending", "in_progress", "completed"}},
                {"description", "op=update: new status"}}},
              {"text", {{"type", "string"}, {"description", "op=update: new text"}}}}},
            {"required", nlohmann::json::array({"op"})}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        // The tool is parallel-dispatched (no parallel_safe override), and
        // op=update is a Load -> modify -> Save sequence over shared
        // per-session state: two concurrent updates would read the same list
        // and one would overwrite the other's change. Serializing the whole
        // execute (not just the store calls, which are individually atomic)
        // is what makes the sequence atomic; the RPC path only reads.
        std::lock_guard<std::mutex> lock(mutex_);
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
    std::vector<Item> Current(const tools::ToolContext& ctx) const {
        return store_->Load(ctx.session_id);
    }

    tools::ToolResult Reply(const std::vector<Item>& items) const {
        return tools::ToolResult{false, RenderMarkdown(items)};
    }

    tools::ToolResult OpSet(const nlohmann::json& body, const tools::ToolContext& ctx) {
        if (!body.contains("items")) {
            return Error("checklist: op=set requires an 'items' array (empty clears the list)");
        }
        const auto& entries = body.at("items");
        if (!entries.is_array()) {
            return Error("checklist: 'items' must be an array");
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
            if (item.text.empty()) {
                return Error("checklist: item text must not be empty");
            }
            item.status = ItemStatus::Pending;
            if (entry.contains("status")) {
                if (!entry.at("status").is_string()) {
                    return Error("checklist: item 'status' must be a string");
                }
                const auto status = ParseStatus(entry.at("status").get<std::string>());
                if (!status) {
                    return Error("checklist: unknown item status \"" +
                                 entry.at("status").get<std::string>() +
                                 "\" (expected pending, in_progress or completed)");
                }
                item.status = *status;
            }
            items.push_back(std::move(item));
        }

        store_->Save(ctx.session_id, items);
        return Reply(items);
    }

    tools::ToolResult OpUpdate(const nlohmann::json& body, const tools::ToolContext& ctx) {
        if (!body.contains("id") || !body.at("id").is_number_integer()) {
            return Error("checklist: op=update requires an integer item 'id'");
        }
        const auto id = body.at("id").get<std::int64_t>();

        ItemStatus status = ItemStatus::Pending;
        bool has_status = false;
        if (body.contains("status")) {
            if (!body.at("status").is_string()) {
                return Error("checklist: 'status' must be a string");
            }
            const auto parsed = ParseStatus(body.at("status").get<std::string>());
            if (!parsed) {
                return Error("checklist: unknown status \"" + body.at("status").get<std::string>() +
                             "\" (expected pending, in_progress or completed)");
            }
            status = *parsed;
            has_status = true;
        }
        bool has_text = body.contains("text");
        if (has_text && !body.at("text").is_string()) {
            return Error("checklist: 'text' must be a string");
        }
        if (!has_status && !has_text) {
            return Error("checklist: op=update needs a new 'status' or a new 'text'");
        }
        if (has_text && body.at("text").get<std::string>().empty()) {
            return Error("checklist: item text must not be empty");
        }

        auto items = Current(ctx);
        auto item = std::find_if(items.begin(), items.end(),
                                 [id](const Item& candidate) { return candidate.id == id; });
        if (item == items.end()) {
            return Error("checklist: no item with id " + std::to_string(id) +
                         " -- use op=view to see the current ids");
        }
        if (has_status) item->status = status;
        if (has_text) item->text = body.at("text").get<std::string>();

        store_->Save(ctx.session_id, items);
        return Reply(items);
    }

    tools::ToolResult OpView(const tools::ToolContext& ctx) {
        if (!store_->available()) {
            return Error("checklist: plugin storage is not available in this host");
        }
        return Reply(Current(ctx));
    }

    tools::ToolSpec spec_;
    std::shared_ptr<ChecklistStore> store_;
    // Serializes execute() -- see the comment at the top of it.
    std::mutex mutex_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeChecklistTool(std::shared_ptr<ChecklistStore> store) {
    return std::make_unique<ChecklistTool>(std::move(store));
}

}  // namespace minimal::checklist
