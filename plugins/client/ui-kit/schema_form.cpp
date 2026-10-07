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
using nlohmann::json;

// Clipped to `width` display columns (a wide glyph counts two), with an
// ellipsis when cut.
std::string Clip(const std::string& text, std::size_t width) {
    return TruncateUtf8Columns(text, width);
}

// A nested field's label sits under its group header, indented.
constexpr std::size_t kNestedIndent = 2;  // NOLINT(readability-magic-numbers): the columns of "  "
// An item's line also takes the gap and the live/restart tag's columns.
constexpr std::size_t kItemLineSlack = 12;  // NOLINT(readability-magic-numbers): "  " + "  " + "not used"

bool IsEntryRow(const FormRow& row) {
    return row.item >= 0 || !row.item_key.empty() || row.add_item;
}

// What the next frame is: a list item, a map entry, or a schema-less node.
enum class FrameKind { kListItem, kMapEntry, kNode };

// One level of drilling into a structured value, edited as a draft that is
// written back into its parent when left (the field's value, for the first).
struct Frame {
    FrameKind kind = FrameKind::kNode;
    FormRow owner;           // the row it was opened from: its schema and path
    int index = -1;          // a list item's index; -1: a new one
    std::string key;         // a map entry's name ("" new) / a node's member
    json draft;
    std::string crumb;       // this level's part of the title
};

// What the editor's text will be: a field's value, or the two steps of
// adding a member to a schema-less object (its name, then its value).
enum class EditorFor { kField, kNewKey, kNewValue };

// What the keys do on the selected row -- the row's own action first.
std::string HintsFor(const FormRow* row, bool can_pick, bool can_back, const Frame* frame) {
    std::string hints;
    const bool node = frame != nullptr && frame->kind == FrameKind::kNode;
    if (row != nullptr) {
        switch (ActionFor(*row, can_pick)) {
            case FormAction::kToggle: hints = "enter/space: toggle"; break;
            case FormAction::kCycle: hints = "enter/space: next value"; break;
            case FormAction::kEdit: hints = "enter: edit"; break;
            case FormAction::kPick:
                hints = row->type == "enum" ? "enter: choose  ·  space: next value" : "enter: choose";
                break;
            case FormAction::kOpen: hints = row->add_item ? "enter: add" : "enter: open"; break;
        }
        if (row->item >= 0) {
            hints += "  ·  d: delete  ·  ctrl+↑↓/K J: move";
        } else if (!row->item_key.empty()) {
            hints += "  ·  d: delete";
        } else if (node) {
            hints += "  ·  d: delete";
            if (frame->draft.is_array()) hints += "  ·  K J: move";
        } else if (row->overridden && !row->add_item) {
            hints += "  ·  d/del: reset to default";
        }
        hints += "  ·  ";
    }
    if (node) hints += "a: add  ·  ";
    if (frame != nullptr) return hints + "←/esc: back (keeps it)  ·  x: discard";
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
        return editing_ || !frames_.empty() || static_cast<bool>(callbacks_.on_back);
    }

private:
    void Sync(const SchemaFormState& state) {
        if (!frames_.empty()) {
            // The whole value went out when the first level was left: its
            // answer decides -- accepted, back to the form; refused, the
            // level stays open with its draft and the plugin's reason.
            if (committing_ && !state.busy && state.generation != sent_generation_) {
                committing_ = false;
                if (!state.status_is_error) {
                    CloseFrames();
                } else {
                    frame_error_.clear();   // the status line says why
                }
            }
            if (!frames_.empty()) return;
        }
        if (state.generation == built_generation_ && !rows_.empty()) return;
        built_generation_ = state.generation;
        std::string keep;
        int keep_item = -1;
        std::string keep_key;
        if (const FormRow* row = Selected()) {
            keep = row->path;
            keep_item = row->item;
            keep_key = row->item_key;
        }
        if (!keep_path_.empty()) {
            keep = std::exchange(keep_path_, {});
            keep_item = std::exchange(keep_item_, -1);
            keep_key = std::exchange(keep_key_, {});
        }
        rows_ = BuildFormRows(state.describe);
        selected_ = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const bool same = rows_[i].path == keep && !rows_[i].add_item &&
                              rows_[i].item == keep_item && rows_[i].item_key == keep_key;
            if (!rows_[i].header && (selected_ < 0 || same)) selected_ = i;
            if (!keep.empty() && same) break;
        }
        // The answer to the last edit typed in the editor: a refusal reopens
        // it with what was typed; anything else closes the matter.
        if (submitted_path_.empty()) return;
        const std::string path = std::exchange(submitted_path_, {});
        if (!state.status_is_error || state.error_path != path) return;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            if (rows_[i].header || rows_[i].path != path || IsEntryRow(rows_[i])) continue;
            selected_ = i;
            OpenEditor(rows_[i], std::exchange(submitted_text_, {}));
            return;
        }
    }

    static std::size_t Indent(const FormRow& row) {
        if (IsEntryRow(row)) return kNestedIndent;
        if (row.field.is_object() && row.field.value("member", false)) return 0;
        return !row.header && row.path.find('.') != std::string::npos ? kNestedIndent : 0;
    }

    // --- Frames: a structured value, level by level, as drafts -------------

    Frame* Top() { return frames_.empty() ? nullptr : &frames_.back(); }

    // From the plugin's form: an entry row (or its add row) or a JSON row.
    void OpenFromForm(const FormRow& row) {
        top_row_ = row;
        top_row_.item = -1;
        top_row_.add_item = false;
        top_row_.item_key.clear();
        top_ = row.value;
        Frame frame;
        if (row.type == "object_list") {
            top_ = ListOf(row);
            frame.kind = FrameKind::kListItem;
            frame.index = row.add_item ? -1 : row.item;
            frame.crumb = row.add_item ? "(new)" : "#" + std::to_string(row.item + 1);
        } else if (row.type == "object_map") {
            if (!top_.is_object()) top_ = json::object();
            frame.kind = FrameKind::kMapEntry;
            frame.key = row.item_key;
            frame.crumb = row.add_item ? "(new)" : row.item_key;
        } else {
            frame.kind = FrameKind::kNode;
        }
        frame.owner = top_row_;
        frame.draft = frame.kind == FrameKind::kNode ? row.value : ItemDraft(row);
        // A new entry is kept when left, even untouched: the template is
        // something to keep, not a blank.
        top_original_ = row.add_item ? json() : top_;
        committing_ = false;
        frame_error_.clear();
        frames_.push_back(std::move(frame));
        ++frame_serial_;
        RebuildFrameRows(/*keep_selection=*/false);
    }

    // From a level: a nested object or array, schema-less from here on.
    void OpenNode(const FormRow& row) {
        Frame frame;
        frame.kind = FrameKind::kNode;
        frame.owner = row;
        frame.key = row.path;
        frame.draft = row.value;
        frame.crumb = row.label;
        frames_.push_back(std::move(frame));
        ++frame_serial_;
        RebuildFrameRows(/*keep_selection=*/false);
    }

    void RebuildFrameRows(bool keep_selection) {
        const Frame& frame = frames_.back();
        const int was = selected_;
        rows_ = BuildFormRows(frame.kind == FrameKind::kNode
                                  ? InferredDescribe(frame.draft)
                                  : ItemDescribe(frame.owner, frame.draft));
        selected_ = keep_selection && was >= 0 && was < static_cast<int>(rows_.size())
                        ? was
                        : NextFieldRow(rows_, -1, +1);
    }

    // `frame` written back into `parent`. False, with `error`, when it
    // cannot be (a map entry without a name, or a taken one).
    static bool WriteBack(const Frame& frame, json& parent, std::string& error) {
        switch (frame.kind) {
            case FrameKind::kListItem:
                parent = ListWithItem(parent, frame.index, frame.draft);
                return true;
            case FrameKind::kMapEntry:
                return MapWithEntry(parent, frame.key, frame.draft, error);
            case FrameKind::kNode:
                if (frame.key.empty()) {   // the field's own value
                    parent = frame.draft;
                    return true;
                }
                SetNode(parent, frame.owner, frame.draft);
                return true;
        }
        return true;
    }

    // Leaves the current level, keeping its edits: into the level above, or
    // -- from the first -- by sending the field's whole value and waiting
    // for its answer (Sync). Unchanged values are not sent.
    void LeaveFrame(const SchemaFormState& state) {
        std::string error;
        if (frames_.size() > 1) {
            Frame done = std::move(frames_.back());
            frames_.pop_back();
            if (!WriteBack(done, frames_.back().draft, error)) {
                frames_.push_back(std::move(done));
                frame_error_ = std::move(error);
                return;
            }
            frame_error_.clear();
            RebuildFrameRows(/*keep_selection=*/false);
            return;
        }
        json next = top_;
        if (!WriteBack(frames_.back(), next, error)) {
            frame_error_ = std::move(error);
            return;
        }
        if (next == top_original_ || !callbacks_.on_set) {
            CloseFrames();
            return;
        }
        frame_error_.clear();
        committing_ = true;
        sent_generation_ = state.generation;
        keep_path_ = top_row_.path;
        const Frame& first = frames_.front();
        if (first.kind == FrameKind::kListItem) {
            keep_item_ = first.index >= 0 ? first.index : static_cast<int>(ListOf(top_row_).size());
        } else if (first.kind == FrameKind::kMapEntry) {
            keep_key_ = first.draft.value(std::string(kMapKeyField), std::string());
        }
        callbacks_.on_set(top_row_.path, next.dump());
    }

    void CloseFrames() {
        frames_.clear();
        committing_ = false;
        frame_error_.clear();
        ++frame_serial_;
        built_generation_ = 0;   // the form's own rows, rebuilt at the next Sync
    }

    // An edit of a row on the current level: into its draft.
    void SetInFrame(const FormRow& row, const std::string& text) {
        std::string error;
        if (SetItemField(frames_.back().draft, row, text, error)) {
            frame_error_.clear();
            RebuildFrameRows(/*keep_selection=*/true);
        } else {
            frame_error_ = std::move(error);
            OpenEditor(row, text);
        }
    }

    // A chooser's answer for a row, wherever the form is by then: the plugin's
    // form saves it, a level's draft keeps it -- if that level is still open.
    std::function<void(std::optional<std::string>)> DeliverTo(const FormRow& row) {
        if (frames_.empty()) {
            return [this, path = row.path](std::optional<std::string> text) {
                if (text) {
                    if (callbacks_.on_set) callbacks_.on_set(path, *text);
                } else if (callbacks_.on_unset) {
                    callbacks_.on_unset(path);
                }
            };
        }
        return [this, row, serial = frame_serial_, depth = frames_.size()](
                   std::optional<std::string> text) {
            if (frame_serial_ != serial || frames_.size() != depth) return;   // moved on
            if (text) {
                SetInFrame(row, *text);
            } else {
                UnsetItemField(frames_.back().draft, row);
                RebuildFrameRows(/*keep_selection=*/true);
            }
        };
    }

    // --- Rows and the editor ---------------------------------------------

    const FormRow* Selected() const {
        if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[selected_].header ? nullptr : &rows_[selected_];
    }

    void OpenEditor(const FormRow& row, std::string text, EditorFor purpose = EditorFor::kField) {
        edit_text_ = std::move(text);
        secret_edit_ = purpose == EditorFor::kField && row.field.value("secret", false);
        editor_for_ = purpose;
        editing_ = true;
        // The cursor at the end of the starting text.
        input_->OnEvent(Event::End);
    }

    // The editor for a member a schema-less level does not have yet.
    void OpenNewMemberEditor() {
        FormRow none;
        new_key_.clear();
        if (frames_.back().draft.is_array()) {
            OpenEditor(none, {}, EditorFor::kNewValue);
        } else {
            OpenEditor(none, {}, EditorFor::kNewKey);
        }
    }

    void SubmitNewMember() {
        Frame& frame = frames_.back();
        if (editor_for_ == EditorFor::kNewKey) {
            if (edit_text_.empty()) return;
            if (frame.draft.is_object() && frame.draft.contains(edit_text_)) {
                frame_error_ = "\"" + edit_text_ + "\" is already there";
                return;
            }
            new_key_ = edit_text_;
            FormRow none;
            OpenEditor(none, {}, EditorFor::kNewValue);
            return;
        }
        json value = ParseLooseValue(edit_text_);
        if (frame.draft.is_array()) {
            frame.draft.push_back(std::move(value));
        } else {
            if (!frame.draft.is_object()) frame.draft = json::object();
            frame.draft[new_key_] = std::move(value);
        }
        frame_error_.clear();
        RebuildFrameRows(/*keep_selection=*/false);
        selected_ = static_cast<int>(rows_.size()) - 1;
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
        if (!frames_.empty() && rows_.empty()) lines.push_back(text(" (empty)  ·  a: add") | Muted(theme_));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& row = rows_[i];
            if (row.header) {
                lines.push_back(text(row.label) | bold);
                continue;
            }
            const std::size_t indent = Indent(row);
            // An entry's line is its summary across the label and value
            // columns: a URL clipped to the value column said nothing.
            if (IsEntryRow(row)) {
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
            const bool editing_here = editing_ && editor_for_ == EditorFor::kField && i == selected_;
            Element value = editing_here
                                ? input_->Render() | size(WIDTH, EQUAL, kSchemaFormValueWidth)
                                : text(Clip(row.shown, kSchemaFormValueWidth)) |
                                      size(WIDTH, EQUAL, kSchemaFormValueWidth);
            if (!row.overridden && !editing_here) value = value | Muted(theme_);
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
        // A member being added: its name, then its value, typed below.
        if (editing_ && editor_for_ != EditorFor::kField) {
            const std::string what = editor_for_ == EditorFor::kNewKey ? "new key: " : new_key_.empty() ? "new item: " : new_key_ + ": ";
            lines.push_back(hbox({text("  + " + what), input_->Render() | flex}) |
                            SelectionStyle(theme_));
        }

        // Pinned below the list: what the selected field is, and the last
        // outcome -- however far down the list the selection is.
        Elements pinned;
        if (const FormRow* row = Selected(); row != nullptr && IsEntryRow(*row)) {
            pinned.push_back(text(row->add_item ? "a new entry in " + row->path
                                  : !row->item_key.empty() ? row->path + " › " + row->item_key
                                                           : row->path + " #" + std::to_string(row->item + 1)) |
                             Muted(theme_));
            if (!row->add_item) pinned.push_back(paragraph(row->shown));
        } else if (row != nullptr) {
            std::string about = row->path == kMapKeyField ? "name" : row->label;
            about += " (" + row->type;
            // A list's or map's default is a JSON blob: its entries are on screen.
            if (row->field.contains("default") && row->type != "object_list" &&
                row->type != "object_map") {
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
        if (!frame_error_.empty()) {
            pinned.push_back(paragraph(frame_error_) | color(theme_.ColorFor("error")));
        } else if (!unset_armed_.empty()) {
            pinned.push_back(paragraph(armed_message_) | color(theme_.ColorFor("attention")));
        } else if (state.busy) {
            pinned.push_back(text("saving...") | Muted(theme_));
        } else if (!state.status.empty()) {
            Element status = paragraph(state.status);
            pinned.push_back(state.status_is_error ? status | color(theme_.ColorFor("error")) : status);
        }

        std::string hints = HintsFor(Selected(), static_cast<bool>(callbacks_.on_pick),
                                     static_cast<bool>(callbacks_.on_back), Top());
        if (editing_) {
            hints = secret_edit_ ? "enter: save (empty keeps the current key)  ·  esc: cancel"
                                 : "enter: save  ·  esc: cancel";
        }
        std::string title = state.title;
        if (!frames_.empty()) {
            title += "  ›  " + top_row_.path;
            for (const auto& frame : frames_) {
                if (!frame.crumb.empty()) title += " › " + frame.crumb;
            }
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
                if (editor_for_ != EditorFor::kField) {
                    SubmitNewMember();
                } else if (row != nullptr && SubmitSaves(*row, edit_text_)) {
                    if (!frames_.empty()) {
                        SetInFrame(*row, edit_text_);
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
        // An armed `d` (a secret's reset, a deletion) waits for a second
        // press; any other key calls it off. Mouse motion does not: a
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
        if (!frames_.empty()) return OnFrameEvent(event, state, row, reset_key);
        // Not Backspace: a habitual delete, often pressed right after the
        // editor closed, used to leave the form.
        if (event == Event::ArrowLeft || (event == Event::Escape && callbacks_.on_back)) {
            if (callbacks_.on_back) callbacks_.on_back();
            return true;
        }
        if (row == nullptr) return false;
        const bool move_up = event == Event::ArrowUpCtrl || event == Event::Character('K');
        const bool move_down = event == Event::ArrowDownCtrl || event == Event::Character('J');
        const bool raw_key = event == Event::Character('e');
        const bool edit_key = event == Event::Return || event == Event::Character(' ') ||
                              reset_key || move_up || move_down || raw_key;
        // The shown values may be stale until the last edit's answer lands:
        // a toggle now would send the value the previous one already sent.
        if (edit_key && state.busy) return true;
        if ((move_up || move_down) && row->item >= 0) {
            if (callbacks_.on_set) {
                const int step = move_up ? -1 : +1;
                const json list = ListMoved(ListOf(*row), row->item, step);
                keep_path_ = row->path;
                keep_item_ = std::clamp(row->item + step, 0, static_cast<int>(list.size()) - 1);
                callbacks_.on_set(row->path, list.dump());
            }
            return true;
        }
        // The raw JSON of a structured value, one line -- the escape hatch.
        if (raw_key && !IsEntryRow(*row) &&
            (row->type == "json" || row->type == "object_list" || row->type == "object_map")) {
            OpenEditor(*row, row->value.is_null() ? std::string() : row->value.dump());
            return true;
        }
        // Space steps an enum to its next choice even where Enter chooses.
        if (event == Event::Character(' ') && row->type == "enum") {
            if (callbacks_.on_set) callbacks_.on_set(row->path, NextValueText(*row));
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
                    if (event == Event::Return) {
                        callbacks_.on_pick({row->path, row->field, row->value, DeliverTo(*row)});
                    }
                    return true;
                case FormAction::kOpen:
                    if (event == Event::Return) OpenFromForm(*row);
                    return true;
            }
        }
        if (reset_key) {
            if (row->add_item) return true;
            const std::string key = row->path + "#" + std::to_string(row->item) + "#" + row->item_key;
            const bool entry = row->item >= 0 || !row->item_key.empty();
            const bool asks = entry || (row->field.value("secret", false) && row->overridden);
            if (asks && unset_armed_ != key) {
                unset_armed_ = key;
                armed_message_ =
                    entry ? "d again to delete " + row->path + " " +
                                (row->item >= 0 ? "#" + std::to_string(row->item + 1) : row->item_key)
                          : "d again to reset " + row->path +
                                " to its default -- the configured key is erased";
                return true;
            }
            unset_armed_.clear();
            armed_message_.clear();
            if (entry) {
                if (callbacks_.on_set) {
                    keep_path_ = row->path;
                    if (row->item >= 0) {
                        keep_item_ = std::max(row->item - 1, 0);
                        callbacks_.on_set(row->path, ListWithout(ListOf(*row), row->item).dump());
                    } else {
                        callbacks_.on_set(row->path, MapWithout(row->value, row->item_key).dump());
                    }
                }
                return true;
            }
            if (callbacks_.on_unset) callbacks_.on_unset(row->path);
            return true;
        }
        return false;
    }

    // Keys while a level is open: edits change its draft only.
    bool OnFrameEvent(const Event& event, const SchemaFormState& state, const FormRow* row,
                      bool reset_key) {
        if (committing_) return true;   // the value's answer decides (Sync)
        Frame& frame = frames_.back();
        const bool node = frame.kind == FrameKind::kNode;
        if (event == Event::ArrowLeft || event == Event::Escape) {
            LeaveFrame(state);
            return true;
        }
        if (event == Event::Character('x')) {
            frames_.pop_back();
            frame_error_.clear();
            if (frames_.empty()) {
                CloseFrames();
            } else {
                ++frame_serial_;
                RebuildFrameRows(/*keep_selection=*/false);
            }
            return true;
        }
        if (node && event == Event::Character('a')) {
            OpenNewMemberEditor();
            return true;
        }
        if (row == nullptr) return false;
        if (node && frame.draft.is_array() &&
            (event == Event::ArrowUpCtrl || event == Event::Character('K') ||
             event == Event::ArrowDownCtrl || event == Event::Character('J'))) {
            const int step = event == Event::ArrowUpCtrl || event == Event::Character('K') ? -1 : +1;
            const int index = std::stoi(row->path);
            frame.draft = ListMoved(frame.draft, index, step);
            RebuildFrameRows(/*keep_selection=*/false);
            selected_ = std::clamp(index + step, 0, static_cast<int>(rows_.size()) - 1);
            return true;
        }
        if (event == Event::Character(' ') && row->type == "enum") {
            SetInFrame(*row, NextValueText(*row));
            return true;
        }
        if (event == Event::Return || event == Event::Character(' ')) {
            switch (ActionFor(*row, static_cast<bool>(callbacks_.on_pick))) {
                case FormAction::kToggle:
                case FormAction::kCycle:
                    SetInFrame(*row, NextValueText(*row));
                    return true;
                case FormAction::kEdit:
                    OpenEditor(*row, EditText(*row));
                    return true;
                case FormAction::kPick:
                    if (event == Event::Return) {
                        callbacks_.on_pick({row->path, row->field, row->value, DeliverTo(*row)});
                    }
                    return true;
                case FormAction::kOpen:
                    if (event == Event::Return) OpenNode(*row);
                    return true;
            }
        }
        if (reset_key) {
            // A schema-less member is deleted (on a second d); a described
            // field goes back to its default at once -- the draft is not
            // saved until the level is left.
            if (node) {
                const std::string key = "node#" + row->path;
                if (unset_armed_ != key) {
                    unset_armed_ = key;
                    armed_message_ = "d again to delete " + row->label;
                    return true;
                }
                unset_armed_.clear();
                armed_message_.clear();
            }
            UnsetItemField(frame.draft, *row);
            frame_error_.clear();
            RebuildFrameRows(/*keep_selection=*/true);
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
    EditorFor editor_for_ = EditorFor::kField;
    std::string new_key_;       // a member being added: its name, once typed
    std::string edit_text_;
    // The last edit sent from the editor, until its answer lands (Sync).
    std::string submitted_path_;
    std::string submitted_text_;
    // What a first `d` armed ("" = nothing), and what the second will do.
    std::string unset_armed_;
    std::string armed_message_;
    // The levels open over one field's value, and that value: as it was,
    // and as the levels change it.
    std::vector<Frame> frames_;
    FormRow top_row_;
    json top_;
    json top_original_;
    std::string frame_error_;        // why a level could not be kept
    bool committing_ = false;        // the value went out; its answer decides
    std::uint64_t sent_generation_ = 0;
    std::uint64_t frame_serial_ = 0;  // moves whenever a level opens or closes
    // The row to select after the next rebuild (a moved or saved entry).
    std::string keep_path_;
    int keep_item_ = -1;
    std::string keep_key_;
    Component input_;
};

}  // namespace

ftxui::Component MakeSchemaForm(std::function<SchemaFormState()> source,
                                SchemaFormCallbacks callbacks, const Theme& theme) {
    return Make<SchemaFormComponent>(std::move(source), std::move(callbacks), theme);
}

}  // namespace minimal::client::ui_kit
