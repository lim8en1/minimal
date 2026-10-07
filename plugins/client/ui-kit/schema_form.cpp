#include "schema_form.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "modal_chrome.hpp"
#include "minimal/client/sdk/schema_form_model.hpp"
#include "minimal/client/sdk/policy.hpp"
#include "minimal/client/sdk/utf8.hpp"
#include "policy.hpp"

namespace minimal::client::ui_kit {
namespace {

using namespace ftxui;


// Clipped to `width` display columns (a wide glyph counts two), with an
// ellipsis when cut.
std::string Clip(const std::string& text, std::size_t width) {
    return TruncateUtf8Columns(text, width);
}

// A nested field's label sits under its group header, indented.
constexpr std::size_t kNestedIndent = 2;  // NOLINT(readability-magic-numbers): the columns of "  "
// An item's line also takes the gap and the live/restart tag's columns.
constexpr std::size_t kItemLineSlack = 12;  // NOLINT(readability-magic-numbers): "  " + "  " + "not used"

// What the keys do on the selected row -- the row's own action first.
// `in_item`: the form is one list item's, edited as a draft.
std::string HintsFor(const FormRow* row, bool can_pick, bool can_back, bool in_item) {
    std::string hints;
    if (row != nullptr) {
        switch (ActionFor(*row, can_pick)) {
            case FormAction::kToggle: hints = "enter/space: toggle"; break;
            case FormAction::kCycle: hints = "enter/space: next value"; break;
            case FormAction::kEdit: hints = "enter: edit"; break;
            case FormAction::kPick: hints = "enter: choose"; break;
            case FormAction::kOpen: hints = row->add_item ? "enter: add" : "enter: open"; break;
        }
        if (row->item >= 0) {
            hints += "  ·  d: delete  ·  ctrl+↑↓/K J: move";
        } else if (row->overridden && !row->add_item) {
            hints += "  ·  d/del: reset to default";
        }
        hints += "  ·  ";
    }
    if (in_item) return hints + "←/esc: save and back  ·  x: discard changes";
    hints += can_back ? "←/esc: back" : "esc: close";
    return hints + "  ·  * = set";
}

class SchemaFormComponent final : public ComponentBase, public EscapeAware {
public:
    SchemaFormComponent(std::function<SchemaFormState()> source, SchemaFormCallbacks callbacks,
                        const Theme& theme)
        : source_(std::move(source)), callbacks_(std::move(callbacks)), theme_(theme) {
        InputOption option;
        option.multiline = false;
        // A secret's text is typed blind: a literal key never shows.
        option.password = &secret_edit_;
        input_ = Input(&edit_text_, option);
        Add(input_);
    }

    // The editor's Escape cancels it; otherwise Escape steps back a level
    // when there is one to step back to.
    bool WantsEscape() const override {
        return editing_ || item_.has_value() || static_cast<bool>(callbacks_.on_back);
    }

private:
    void Sync(const SchemaFormState& state) {
        if (item_) {
            // The whole list went out when the item was left: its answer
            // decides -- accepted, back to the form; refused, the item stays
            // open with its draft and the plugin's reason.
            if (item_->committing && !state.busy && state.generation != item_->sent_generation) {
                item_->committing = false;
                if (!state.status_is_error) {
                    keep_item_ = item_->index >= 0 ? item_->index
                                                   : static_cast<int>(ListOf(item_->list_row).size());
                    keep_path_ = item_->list_row.path;
                    item_.reset();
                    built_generation_ = 0;
                }
            }
            if (item_) return;
        }
        if (state.generation == built_generation_ && !rows_.empty()) return;
        built_generation_ = state.generation;
        std::string keep = selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
                               ? rows_[selected_].path
                               : std::string();
        int keep_item = selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
                            ? rows_[selected_].item
                            : -1;
        if (!keep_path_.empty()) {
            keep = std::exchange(keep_path_, {});
            keep_item = std::exchange(keep_item_, -1);
        }
        rows_ = BuildFormRows(state.describe);
        selected_ = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const bool same = rows_[i].path == keep && !rows_[i].add_item &&
                              rows_[i].item == keep_item;
            if (!rows_[i].header && (selected_ < 0 || same)) selected_ = i;
            if (!keep.empty() && same) break;
        }
        // The answer to the last edit typed in the editor: a refusal reopens
        // it with what was typed; anything else closes the matter.
        if (submitted_path_.empty()) return;
        const std::string path = std::exchange(submitted_path_, {});
        if (!state.status_is_error || state.error_path != path) return;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            if (rows_[i].header || rows_[i].path != path) continue;
            selected_ = i;
            OpenEditor(rows_[i], std::exchange(submitted_text_, {}));
            return;
        }
    }

    static std::size_t Indent(const FormRow& row) {
        if (row.item >= 0 || row.add_item) return kNestedIndent;
        return !row.header && row.path.find('.') != std::string::npos ? kNestedIndent : 0;
    }

    // --- An object list's item, edited as a draft --------------------------

    struct ItemEdit {
        FormRow list_row;      // the list's row: its path, schema and value
        int index = -1;        // -1: a new item, appended when saved
        nlohmann::json draft;
        nlohmann::json original;
        std::string error;     // the last edit that did not parse
        bool committing = false;
        std::uint64_t sent_generation = 0;
    };

    void OpenItem(const FormRow& row) {
        ItemEdit edit;
        edit.list_row = row;
        edit.list_row.item = -1;
        edit.list_row.add_item = false;
        edit.index = row.add_item ? -1 : row.item;
        edit.draft = ItemDraft(row);
        // A new item always saves when left, even untouched: the template
        // is something to keep, not a blank.
        edit.original = row.add_item ? nlohmann::json() : edit.draft;
        item_ = std::move(edit);
        RebuildItemRows();
        selected_ = NextFieldRow(rows_, -1, +1);
    }

    void RebuildItemRows() {
        const int was = selected_;
        rows_ = BuildFormRows(ItemDescribe(item_->list_row, item_->draft));
        selected_ = was >= 0 && was < static_cast<int>(rows_.size()) ? was : NextFieldRow(rows_, -1, +1);
    }

    // Leaves the item: unchanged, at once; changed, by sending the whole
    // list and waiting for its answer (Sync).
    void LeaveItem(const SchemaFormState& state) {
        if (item_->draft == item_->original || !callbacks_.on_set) {
            item_.reset();
            built_generation_ = 0;
            return;
        }
        const nlohmann::json list =
            ListWithItem(ListOf(item_->list_row), item_->index, item_->draft);
        item_->error.clear();
        item_->committing = true;
        item_->sent_generation = state.generation;
        callbacks_.on_set(item_->list_row.path, list.dump());
    }

    void SetInItem(const FormRow& row, const std::string& text) {
        std::string error;
        if (SetItemField(item_->draft, row, text, error)) {
            item_->error.clear();
            RebuildItemRows();
        } else {
            item_->error = std::move(error);
            OpenEditor(row, text);
        }
    }

    const FormRow* Selected() const {
        if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[selected_].header ? nullptr : &rows_[selected_];
    }

    void OpenEditor(const FormRow& row, std::string text) {
        edit_text_ = std::move(text);
        secret_edit_ = row.field.value("secret", false);
        editing_ = true;
        // The cursor at the end of the starting text.
        input_->OnEvent(Event::End);
    }

    Element OnRender() override {
        const SchemaFormState state = source_();
        Sync(state);

        // The label column fits the longest label, indent included, up to
        // the cap -- in display columns.
        std::size_t label_width = 0;
        for (const auto& row : rows_) {
            const std::size_t indent = Indent(row);
            label_width = std::max(label_width,
                                   std::min(Utf8ColumnWidth(row.label) + indent, kSchemaFormLabelWidth));
        }

        Elements lines;
        if (state.describe.is_null()) lines.push_back(text(" loading...") | Muted(theme_));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& row = rows_[i];
            if (row.header) {
                lines.push_back(text(row.label) | bold);
                continue;
            }
            const std::size_t indent = Indent(row);
            // An item's line is its summary across the label and value
            // columns: a URL clipped to the value column said nothing.
            if (row.item >= 0 || row.add_item) {
                const std::size_t width = label_width + kSchemaFormValueWidth + kItemLineSlack;
                const std::string head = std::string(indent, ' ') + row.label + "  ";
                Element line = hbox({text(row.overridden && !row.add_item ? "* " : "  "),
                                     text(Clip(head + row.shown, width))});
                if (row.add_item) line = line | Muted(theme_);
                if (i == selected_) line = line | SelectionStyle(theme_) | focus;
                lines.push_back(std::move(line));
                continue;
            }
            const std::string label =
                std::string(indent, ' ') + Clip(row.label, kSchemaFormLabelWidth - indent);
            Element value = editing_ && i == selected_
                                ? input_->Render() | size(WIDTH, EQUAL, kSchemaFormValueWidth)
                                : text(Clip(row.shown, kSchemaFormValueWidth)) |
                                      size(WIDTH, EQUAL, kSchemaFormValueWidth);
            if (!row.overridden && !(editing_ && i == selected_)) value = value | Muted(theme_);
            Element name = text(label) | size(WIDTH, EQUAL, static_cast<int>(label_width));
            // An unused field: dimmed, and its tag says why instead of how
            // it would apply.
            if (!row.unused.empty()) name = name | Muted(theme_);
            Elements cells{
                text(row.overridden ? "* " : "  "),
                std::move(name),
                text("  "),
                std::move(value),
                text("  "),
                // Restart is the usual case: only the exceptions are tagged.
                text(!row.unused.empty() ? "not used" : row.live ? "live" : "") | Muted(theme_),
            };
            if (row.pending) cells.push_back(text("  restart pending") | color(theme_.ColorFor("attention")));
            Element line = hbox(std::move(cells));
            if (i == selected_) line = line | SelectionStyle(theme_) | focus;
            lines.push_back(std::move(line));
        }

        // Pinned below the list: what the selected field is, and the last
        // outcome -- however far down the list the selection is.
        Elements pinned;
        if (const FormRow* row = Selected(); row != nullptr && (row->item >= 0 || row->add_item)) {
            pinned.push_back(text(row->add_item ? "a new entry in " + row->path
                                                : row->path + " #" + std::to_string(row->item + 1)) |
                             Muted(theme_));
            if (!row->add_item) pinned.push_back(paragraph(row->shown));
        } else if (row != nullptr) {
            std::string about = row->path + " (" + row->type;
            // A list's default is a JSON blob: its entries are on screen.
            if (row->field.contains("default") && row->type != "object_list") {
                about += ", default " + row->field["default"].dump();
            }
            if (row->field.contains("minimum")) about += ", min " + row->field["minimum"].dump();
            if (row->field.contains("maximum")) about += ", max " + row->field["maximum"].dump();
            if (row->field.contains("choices")) about += ", one of " + row->field["choices"].dump();
            about += ")";
            pinned.push_back(text(about) | Muted(theme_));
            // The whole value, when the column had to cut it.
            if (Utf8ColumnWidth(row->shown) > kSchemaFormValueWidth) {
                pinned.push_back(paragraph("= " + row->shown) | Muted(theme_));
            }
            if (!row->unused.empty()) {
                pinned.push_back(paragraph("Not used while " + row->unused + ".") |
                                 color(theme_.ColorFor("attention")));
            }
            if (!row->doc.empty()) pinned.push_back(paragraph(row->doc));
        }
        if (item_ && !item_->error.empty()) {
            pinned.push_back(paragraph(item_->error) | color(theme_.ColorFor("error")));
        } else if (!unset_armed_.empty()) {
            pinned.push_back(paragraph(armed_message_) | color(theme_.ColorFor("attention")));
        } else if (state.busy) {
            pinned.push_back(text("saving...") | Muted(theme_));
        } else if (!state.status.empty()) {
            Element status = paragraph(state.status);
            pinned.push_back(state.status_is_error ? status | color(theme_.ColorFor("error")) : status);
        }

        std::string hints =
            HintsFor(Selected(), static_cast<bool>(callbacks_.on_pick) && !item_,
                     static_cast<bool>(callbacks_.on_back), item_.has_value());
        if (editing_) {
            hints = secret_edit_ ? "enter: save (empty keeps the current key)  ·  esc: cancel"
                                 : "enter: save  ·  esc: cancel";
        }
        std::string title = state.title;
        if (item_) {
            title += "  ›  " + item_->list_row.path + " " +
                     (item_->index >= 0 ? "#" + std::to_string(item_->index + 1) : std::string("(new)"));
        }
        return ModalFrame(theme_, title, std::move(lines), std::move(pinned), hints,
                          ModalAccent::Focus);
    }

    bool OnEvent(Event event) override {
        const SchemaFormState state = source_();
        Sync(state);
        const FormRow* row = Selected();
        if (editing_) {
            if (event == Event::Return) {
                editing_ = false;
                if (row != nullptr && SubmitSaves(*row, edit_text_)) {
                    if (item_) {
                        SetInItem(*row, edit_text_);
                    } else if (callbacks_.on_set) {
                        submitted_path_ = row->path;
                        submitted_text_ = edit_text_;
                        callbacks_.on_set(row->path, edit_text_);
                    }
                }
                return true;
            }
            if (event == Event::Escape) {
                editing_ = false;
                return true;
            }
            return input_->OnEvent(event);
        }
        // An armed `d` (a secret's reset, an item's delete) waits for a
        // second press; any other key calls it off. Mouse motion does not: a
        // terminal reports it constantly.
        const bool reset_key = event == Event::Character('d') || event == Event::Delete;
        if (!event.is_mouse() && !reset_key) {
            unset_armed_.clear();
            armed_message_.clear();
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            selected_ = NextFieldRow(rows_, selected_, -1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            selected_ = NextFieldRow(rows_, selected_, +1);
            return true;
        }
        if (item_) return OnItemEvent(event, state, row, reset_key);
        // Not Backspace: a habitual delete, often pressed right after the
        // editor closed, used to leave the form.
        if (event == Event::ArrowLeft || (event == Event::Escape && callbacks_.on_back)) {
            if (callbacks_.on_back) callbacks_.on_back();
            return true;
        }
        if (row == nullptr) return false;
        const bool move_up = event == Event::ArrowUpCtrl || event == Event::Character('K');
        const bool move_down = event == Event::ArrowDownCtrl || event == Event::Character('J');
        const bool edit_key = event == Event::Return || event == Event::Character(' ') ||
                              reset_key || move_up || move_down;
        // The shown values may be stale until the last edit's answer lands:
        // a toggle now would send the value the previous one already sent.
        if (edit_key && state.busy) return true;
        if ((move_up || move_down) && row->item >= 0) {
            if (callbacks_.on_set) {
                const int step = move_up ? -1 : +1;
                const nlohmann::json list = ListMoved(ListOf(*row), row->item, step);
                keep_path_ = row->path;
                keep_item_ = std::clamp(row->item + step, 0, static_cast<int>(list.size()) - 1);
                callbacks_.on_set(row->path, list.dump());
            }
            return true;
        }
        if (event == Event::Return || event == Event::Character(' ')) {
            switch (ActionFor(*row, static_cast<bool>(callbacks_.on_pick))) {
                case FormAction::kToggle:
                case FormAction::kCycle:
                    if (callbacks_.on_set) callbacks_.on_set(row->path, NextValueText(*row));
                    return true;
                case FormAction::kEdit:
                    OpenEditor(*row, EditText(*row));
                    return true;
                case FormAction::kPick:
                    callbacks_.on_pick(row->path, row->field);
                    return true;
                case FormAction::kOpen:
                    if (event == Event::Return) OpenItem(*row);
                    return true;
            }
        }
        if (reset_key) {
            if (row->add_item) return true;
            const std::string key = row->path + "#" + std::to_string(row->item);
            const bool asks = row->item >= 0 ||
                              (row->field.value("secret", false) && row->overridden);
            if (asks && unset_armed_ != key) {
                unset_armed_ = key;
                armed_message_ =
                    row->item >= 0
                        ? "d again to delete " + row->path + " #" + std::to_string(row->item + 1)
                        : "d again to reset " + row->path +
                              " to its default -- the configured key is erased";
                return true;
            }
            unset_armed_.clear();
            armed_message_.clear();
            if (row->item >= 0) {
                if (callbacks_.on_set) {
                    keep_path_ = row->path;
                    keep_item_ = std::max(row->item - 1, 0);
                    callbacks_.on_set(row->path, ListWithout(ListOf(*row), row->item).dump());
                }
                return true;
            }
            if (callbacks_.on_unset) callbacks_.on_unset(row->path);
            return true;
        }
        return false;
    }

    // Keys while an item's form is open: edits change the draft only.
    bool OnItemEvent(const Event& event, const SchemaFormState& state, const FormRow* row,
                     bool reset_key) {
        if (item_->committing) return true;   // its answer decides (Sync)
        if (event == Event::ArrowLeft || event == Event::Escape) {
            LeaveItem(state);
            return true;
        }
        if (event == Event::Character('x')) {
            item_.reset();
            built_generation_ = 0;
            return true;
        }
        if (row == nullptr) return false;
        if (event == Event::Return || event == Event::Character(' ')) {
            switch (ActionFor(*row, /*can_pick=*/false)) {
                case FormAction::kToggle:
                case FormAction::kCycle:
                    SetInItem(*row, NextValueText(*row));
                    return true;
                case FormAction::kEdit:
                    OpenEditor(*row, EditText(*row));
                    return true;
                case FormAction::kPick:
                case FormAction::kOpen:
                    return true;   // not offered inside an item
            }
        }
        if (reset_key) {
            UnsetItemField(item_->draft, *row);
            item_->error.clear();
            RebuildItemRows();
            return true;
        }
        return false;
    }

    std::function<SchemaFormState()> source_;
    SchemaFormCallbacks callbacks_;
    const Theme& theme_;
    std::vector<FormRow> rows_;
    std::uint64_t built_generation_ = 0;
    int selected_ = -1;
    bool editing_ = false;
    bool secret_edit_ = false;  // the editor is open on a secret: masked
    std::string edit_text_;
    // The last edit sent from the editor, until its answer lands (Sync).
    std::string submitted_path_;
    std::string submitted_text_;
    // The secret a first `d` asked to reset ("" = none).
    std::string unset_armed_;
    std::string armed_message_;   // what the armed `d` will do
    // The open item, while one is (its draft, and the list it belongs to).
    std::optional<ItemEdit> item_;
    // The row to select after the next rebuild (a moved or saved item).
    std::string keep_path_;
    int keep_item_ = -1;
    Component input_;
};

}  // namespace

ftxui::Component MakeSchemaForm(std::function<SchemaFormState()> source,
                                SchemaFormCallbacks callbacks, const Theme& theme) {
    return Make<SchemaFormComponent>(std::move(source), std::move(callbacks), theme);
}

}  // namespace minimal::client::ui_kit
