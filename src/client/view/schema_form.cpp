#include "minimal/client/view/schema_form.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/view/modal_host.hpp"
#include "minimal/client/view/prompt_modal.hpp"
#include "minimal/client/view/schema_form_model.hpp"
#include "minimal/common/utf8.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client {
namespace {

using namespace ftxui;


std::string Clip(const std::string& text, std::size_t width) {
    if (text.size() <= width) return text;
    return common::TruncateCodePoints(text, width - 1) + "~";
}

class SchemaFormComponent final : public ComponentBase, public EscapeAware {
public:
    SchemaFormComponent(std::function<SchemaFormState()> source, SchemaFormCallbacks callbacks,
                        const Theme& theme)
        : source_(std::move(source)), callbacks_(std::move(callbacks)), theme_(theme) {
        InputOption option;
        option.multiline = false;
        input_ = Input(&edit_text_, option);
        Add(input_);
    }

    bool WantsEscape() const override { return editing_; }

private:
    void Sync(const SchemaFormState& state) {
        if (state.generation == built_generation_ && !rows_.empty()) return;
        built_generation_ = state.generation;
        const std::string keep = selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
                                     ? rows_[selected_].path
                                     : std::string();
        rows_ = BuildFormRows(state.describe);
        selected_ = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            if (!rows_[i].header && (selected_ < 0 || rows_[i].path == keep)) selected_ = i;
            if (!keep.empty() && rows_[i].path == keep) break;
        }
    }

    const FormRow* Selected() const {
        if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return nullptr;
        return rows_[selected_].header ? nullptr : &rows_[selected_];
    }

    Element OnRender() override {
        const SchemaFormState state = source_();
        Sync(state);

        std::size_t label_width = 0;
        for (const auto& row : rows_) {
            const std::size_t indent = row.header ? 0 : (row.path.find('.') == std::string::npos ? 0 : 2);
            label_width = std::max(label_width, std::min(row.label.size() + indent, kSchemaFormLabelWidth));
        }

        Elements lines;
        if (state.describe.is_null()) lines.push_back(text(" loading...") | Muted(theme_));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
            const auto& row = rows_[i];
            if (row.header) {
                lines.push_back(text(row.label) | bold);
                continue;
            }
            const bool nested = row.path.find('.') != std::string::npos;
            const std::string label = (nested ? "  " : "") + Clip(row.label, kSchemaFormLabelWidth);
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
                text(!row.unused.empty() ? "not used" : row.live ? "live" : "restart") |
                    Muted(theme_),
            };
            if (row.pending) cells.push_back(text("  restart pending") | color(theme_.ColorFor("attention")));
            Element line = hbox(std::move(cells));
            if (i == selected_) line = line | SelectionStyle(theme_) | focus;
            lines.push_back(std::move(line));
        }

        lines.push_back(ModalRule(theme_));
        if (const FormRow* row = Selected()) {
            std::string about = row->path + " (" + row->type;
            if (row->field.contains("default")) about += ", default " + row->field["default"].dump();
            if (row->field.contains("minimum")) about += ", min " + row->field["minimum"].dump();
            if (row->field.contains("maximum")) about += ", max " + row->field["maximum"].dump();
            if (row->field.contains("choices")) about += ", one of " + row->field["choices"].dump();
            about += ")";
            lines.push_back(text(about) | Muted(theme_));
            if (!row->unused.empty()) {
                lines.push_back(paragraph("Not used while " + row->unused + ".") |
                                color(theme_.ColorFor("attention")));
            }
            if (!row->doc.empty()) lines.push_back(paragraph(row->doc));
        }
        if (!state.status.empty()) {
            Element status = paragraph(state.status);
            lines.push_back(state.status_is_error ? status | color(theme_.ColorFor("error")) : status);
        }

        const std::string hints =
            editing_ ? "enter: save  ·  esc: cancel"
                     : "enter: edit/toggle  ·  d: default  ·  ←: back  ·  esc: close  ·  * = set";
        return ModalFrame(theme_, state.title, std::move(lines), hints);
    }

    bool OnEvent(Event event) override {
        Sync(source_());
        const FormRow* row = Selected();
        if (editing_) {
            if (event == Event::Return) {
                editing_ = false;
                if (row != nullptr && callbacks_.on_set && SubmitSaves(*row, edit_text_)) {
                    callbacks_.on_set(row->path, edit_text_);
                }
                return true;
            }
            if (event == Event::Escape) {
                editing_ = false;
                return true;
            }
            return input_->OnEvent(event);
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            selected_ = NextFieldRow(rows_, selected_, -1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            selected_ = NextFieldRow(rows_, selected_, +1);
            return true;
        }
        if (event == Event::ArrowLeft || event == Event::Backspace) {
            if (callbacks_.on_back) callbacks_.on_back();
            return true;
        }
        if (row == nullptr) return false;
        if (event == Event::Return || event == Event::Character(' ')) {
            switch (ActionFor(*row)) {
                case FormAction::kToggle:
                case FormAction::kCycle:
                    if (callbacks_.on_set) callbacks_.on_set(row->path, NextValueText(*row));
                    return true;
                case FormAction::kEdit:
                    edit_text_ = EditText(*row);
                    editing_ = true;
                    // The cursor at the end of the starting text.
                    input_->OnEvent(Event::End);
                    return true;
            }
        }
        if (event == Event::Character('d') || event == Event::Delete) {
            if (callbacks_.on_unset) callbacks_.on_unset(row->path);
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
    std::string edit_text_;
    Component input_;
};

}  // namespace

ftxui::Component MakeSchemaForm(std::function<SchemaFormState()> source,
                                SchemaFormCallbacks callbacks, const Theme& theme) {
    return Make<SchemaFormComponent>(std::move(source), std::move(callbacks), theme);
}

}  // namespace minimal::client
