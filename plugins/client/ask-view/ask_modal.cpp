#include "ask_modal.hpp"

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>

#include <ftxui/component/event.hpp>

#include "minimal/client/view/prompt_modal.hpp"

namespace minimal::ask_view {

namespace plugins = minimal::client::plugins;
using minimal::client::Theme;

namespace {

std::string StringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (!it->is_string()) throw std::invalid_argument(std::string("ask: \"") + key + "\" not a string");
    return it->get<std::string>();
}

bool BoolField(const nlohmann::json& object, const char* key, bool fallback) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_boolean()) throw std::invalid_argument(std::string("ask: \"") + key + "\" not a boolean");
    return it->get<bool>();
}

// Ephemeral per-question UI state, owned by the component (shared_ptr,
// captured by every closure): the modal is rebuilt only when the request id
// changes, so the state must outlive the factory call that built the widgets
// pointing into it.
struct QuestionState {
    // Entry strings, "1. label" -- numbered for the digit keys. Stored here
    // (not built per frame) because ftxui's ConstStringRef/ConstStringListRef
    // borrow the storage.
    std::vector<std::string> entries;
    // multiple: one stable bool per entry (deque: the checkboxes need stable
    // bool*, which vector<bool> cannot give).
    std::deque<bool> boxes;
    // single: the radiobox's selected index; meaningful only once touched
    // (FTXUI clamps -1 to 0 at first render, with no user interaction).
    int radio = -1;
    bool radio_touched = false;
    // "Type your own answer" is the TRAILING entry of `entries`/`boxes` (or
    // `radio == options.size()`) when allow_other is set, so exclusivity with
    // the real picks is structural for a radio list and cross-cleared by
    // index for a checkbox list.
    std::string other_text;
    // Focus targets. Weak: the components' own closures hold this state, so
    // strong references here would be a cycle.
    std::weak_ptr<ftxui::ComponentBase> list;
    std::weak_ptr<ftxui::ComponentBase> input;
};

struct ModalState {
    std::vector<AskQuestion> questions;
    std::vector<QuestionState> qs;
    std::string asker;
    // Two-phase submit: false is editing (widgets live), true is the frozen
    // review list.
    bool reviewing = false;
    // A first Escape with answers entered only arms the dismissal.
    bool dismiss_armed = false;
    bool interacted = false;
    bool replied = false;
};

bool HasOther(const AskQuestion& question) { return question.allow_other; }
int OtherIndex(const AskQuestion& question) { return static_cast<int>(question.options.size()); }

bool OtherChosen(const AskQuestion& question, const QuestionState& qs) {
    if (!HasOther(question)) return false;
    if (question.multiple) return !qs.boxes.empty() && qs.boxes.back();
    return qs.radio_touched && qs.radio == OtherIndex(question);
}

bool SlotAnswered(const AskQuestion& question, const QuestionState& qs) {
    if (OtherChosen(question, qs)) return !qs.other_text.empty();
    if (question.multiple) {
        for (std::size_t j = 0; j < question.options.size(); ++j) {
            if (qs.boxes[j]) return true;
        }
        return false;
    }
    return qs.radio_touched && qs.radio >= 0 && qs.radio < OtherIndex(question);
}

bool AnyAnswered(const ModalState& state) {
    for (std::size_t i = 0; i < state.questions.size(); ++i) {
        if (SlotAnswered(state.questions[i], state.qs[i]) || !state.qs[i].other_text.empty()) {
            return true;
        }
    }
    return false;
}

nlohmann::json CollectAnswer(const ModalState& state) {
    nlohmann::json answers = nlohmann::json::array();
    for (std::size_t i = 0; i < state.questions.size(); ++i) {
        const auto& question = state.questions[i];
        const auto& qs = state.qs[i];
        nlohmann::json slot = nlohmann::json::object();
        slot["selected"] = nlohmann::json::array();
        slot["other"] = "";
        if (OtherChosen(question, qs)) {
            slot["other"] = qs.other_text;
        } else if (question.multiple) {
            for (std::size_t j = 0; j < question.options.size(); ++j) {
                if (qs.boxes[j]) slot["selected"].push_back(question.options[j].label);
            }
        } else if (SlotAnswered(question, qs)) {
            slot["selected"].push_back(question.options[qs.radio].label);
        }
        answers.push_back(std::move(slot));
    }
    return nlohmann::json{{"answers", std::move(answers)}};
}

// One review-screen line for ONE question, mirroring CollectAnswer exactly --
// whatever the review shows is what submission sends.
std::string ReviewLine(const AskQuestion& question, const QuestionState& qs) {
    if (OtherChosen(question, qs)) {
        return qs.other_text.empty() ? "(not answered)" : "-> your answer: " + qs.other_text;
    }
    if (question.multiple) {
        std::string joined;
        for (std::size_t j = 0; j < question.options.size(); ++j) {
            if (!qs.boxes[j]) continue;
            if (!joined.empty()) joined += ", ";
            joined += question.options[j].label;
        }
        return joined.empty() ? "(not answered)" : "-> " + joined;
    }
    if (SlotAnswered(question, qs)) return "-> " + question.options[qs.radio].label;
    return "(not answered)";
}

void Focus(const std::weak_ptr<ftxui::ComponentBase>& target) {
    if (const auto component = target.lock()) component->TakeFocus();
}

// The digit a key event names, 1-9, or 0 for none.
int DigitOf(const ftxui::Event& event) {
    if (!event.is_character()) return 0;
    const auto& c = event.character();
    if (c.size() != 1 || c[0] < '1' || c[0] > '9') return 0;
    return c[0] - '0';
}

}  // namespace

std::vector<AskQuestion> ParsePayload(const nlohmann::json& payload) {
    if (!payload.is_object()) throw std::invalid_argument("ask: payload is not an object");
    const auto it = payload.find("questions");
    if (it == payload.end() || !it->is_array() || it->empty()) {
        throw std::invalid_argument("ask: no questions");
    }
    std::vector<AskQuestion> questions;
    for (const auto& wire : *it) {
        if (!wire.is_object()) throw std::invalid_argument("ask: question is not an object");
        AskQuestion question;
        question.question = StringField(wire, "question");
        question.header = StringField(wire, "header");
        question.multiple = BoolField(wire, "multiple", false);
        question.allow_other = BoolField(wire, "allow_other", true);
        const auto options = wire.find("options");
        if (options == wire.end() || !options->is_array() || options->empty()) {
            throw std::invalid_argument("ask: question without options");
        }
        for (const auto& option : *options) {
            if (!option.is_object()) throw std::invalid_argument("ask: option is not an object");
            question.options.push_back(
                {StringField(option, "label"), StringField(option, "description")});
        }
        // One row per digit key (1-9) at most.
        constexpr std::size_t kDigitKeys = '9' - '0';
        if (question.options.size() + (question.allow_other ? 1 : 0) > kDigitKeys) {
            throw std::invalid_argument("ask: too many options");
        }
        questions.push_back(std::move(question));
    }
    return questions;
}

bool RadioRowSelected(bool touched, bool active, bool focused) {
    return touched && (active || focused);
}

ftxui::Element WrappedOptionEntry(const ftxui::EntryState& s, const char* marker_on,
                                  const char* marker_off, const std::string& description,
                                  const ftxui::Decorator& selection,
                                  const ftxui::Decorator& muted, bool touched) {
    using namespace ftxui;
    Element marker = text(s.state ? marker_on : marker_off);
    Element label = paragraph(s.label);
    if (RadioRowSelected(touched, s.active, s.focused)) {
        label |= bold;
        label |= selection;
    }
    Elements column = {label};
    if (!description.empty()) column.push_back(paragraph(description) | muted);
    return hbox({marker, vbox(std::move(column)) | flex});
}

ftxui::Component MakeAskModal(std::vector<AskQuestion> questions, std::string asker,
                              const Theme& theme,
                              std::function<void(plugins::PromptReply)> on_reply) {
    using namespace ftxui;

    const auto state = std::make_shared<ModalState>();
    state->questions = std::move(questions);
    state->qs.resize(state->questions.size());
    state->asker = std::move(asker);
    const int count = static_cast<int>(state->questions.size());
    const Decorator selection = minimal::client::SelectionStyle(theme);
    const Decorator muted = minimal::client::Muted(theme);

    // Sends the reply once. Later calls (a stray key after the reply went
    // out, before the host tears the modal down) are ignored.
    const auto reply = [state, on_reply](plugins::PromptReply answer) {
        if (state->replied) return;
        state->replied = true;
        on_reply(std::move(answer));
    };
    const auto submit = [state, reply] {
        reply({v1::PROMPT_OUTCOME_ANSWERED, CollectAnswer(*state)});
    };
    // Past the last question: review, or -- for a lone single-pick question,
    // where the review would only echo the one row just picked -- submit.
    const auto finish = [state, submit, count] {
        if (count == 1 && !state->questions[0].multiple) {
            submit();
        } else {
            state->reviewing = true;
        }
    };
    const auto advance = [state, finish, count](int index) {
        if (index + 1 < count) {
            Focus(state->qs[index + 1].list);
        } else {
            finish();
        }
    };

    Components question_components;
    for (int i = 0; i < count; ++i) {
        const auto& question = state->questions[i];
        auto& qs = state->qs[i];
        const bool other_allowed = HasOther(question);
        const int other_index = OtherIndex(question);

        for (std::size_t j = 0; j < question.options.size(); ++j) {
            qs.entries.push_back(std::to_string(j + 1) + ". " + question.options[j].label);
        }
        if (other_allowed) {
            qs.entries.push_back(std::to_string(other_index + 1) + ". Type your own answer");
        }

        // The free-text line is a sibling of the list (neither Radiobox nor
        // Checkbox can host an inline child), attached to `other_container`
        // only while "Type your own answer" is the current pick -- an
        // invisible-but-focusable input is never left in the tree.
        auto other_container = Container::Vertical({});
        auto raw_input = Input(&qs.other_text, "your answer");
        auto input = CatchEvent(raw_input, [state, i, advance](const Event& event) {
            if (event == Event::Return) {
                // An empty free-text answer is not an answer: stay.
                if (!state->qs[i].other_text.empty()) advance(i);
                return true;
            }
            if (event == Event::Escape) {
                // Leave the text line, keep the answers.
                Focus(state->qs[i].list);
                return true;
            }
            return false;
        });
        qs.input = input;
        const auto sync_other = [state, i, other_container, input] {
            other_container->DetachAllChildren();
            if (OtherChosen(state->questions[i], state->qs[i])) other_container->Add(input);
        };

        Component list;
        if (question.multiple) {
            for (std::size_t j = 0; j < qs.entries.size(); ++j) qs.boxes.push_back(false);
            // One on_change per entry, callable from the digit keys too.
            auto toggled = std::make_shared<std::vector<std::function<void()>>>();
            Components boxes;
            for (std::size_t j = 0; j < qs.entries.size(); ++j) {
                const bool is_other = other_allowed && static_cast<int>(j) == other_index;
                toggled->push_back([state, i, j, is_other, other_allowed, sync_other] {
                    auto& q = state->qs[i];
                    if (!other_allowed) return;
                    // "Other" and real picks are mutually exclusive: the wire
                    // contract never accepts a selection alongside free text.
                    if (is_other && q.boxes.back()) {
                        for (std::size_t k = 0; k + 1 < q.boxes.size(); ++k) q.boxes[k] = false;
                    } else if (!is_other && q.boxes[j]) {
                        q.boxes.back() = false;
                    }
                    sync_other();
                });
                CheckboxOption option_state;
                option_state.label = ConstStringRef(qs.entries[j]);
                option_state.checked = Ref<bool>(&qs.boxes[j]);
                const std::string description =
                    is_other ? "" : question.options[j].description;
                option_state.transform = [description, selection, muted](const EntryState& s) {
                    // A checkbox's picked-ness lives in its own marker.
                    return WrappedOptionEntry(s, "▣ ", "☐ ", description, selection, muted,
                                              /*touched=*/true);
                };
                option_state.on_change = (*toggled)[j];
                boxes.push_back(Checkbox(std::move(option_state)));
            }
            list = CatchEvent(Container::Vertical(std::move(boxes)),
                              [state, i, toggled, advance](const Event& event) {
                auto& q = state->qs[i];
                if (const int digit = DigitOf(event);
                    digit > 0 && digit <= static_cast<int>(q.boxes.size())) {
                    q.boxes[digit - 1] = !q.boxes[digit - 1];
                    (*toggled)[digit - 1]();
                    return true;
                }
                if (event == Event::Tab) {   // skip ahead, picks as they are
                    advance(i);
                    return true;
                }
                if (event == Event::Return) {
                    if (OtherChosen(state->questions[i], q) && q.other_text.empty()) {
                        Focus(q.input);
                    } else {
                        advance(i);
                    }
                    return true;
                }
                return false;
            });
        } else {
            RadioboxOption radio_state;
            radio_state.entries = ConstStringListRef(&qs.entries);
            radio_state.selected = &qs.radio;
            radio_state.on_change = [state, i, sync_other] {
                state->qs[i].radio_touched = true;
                sync_other();
            };
            radio_state.transform = [state, i, other_index, selection,
                                     muted](const EntryState& s) {
                const std::string description =
                    s.index == other_index
                        ? ""
                        : state->questions[i].options[s.index].description;
                return WrappedOptionEntry(s, "◉ ", "○ ", description, selection, muted,
                                          state->qs[i].radio_touched);
            };
            auto radio = Radiobox(std::move(radio_state));
            // Commit the highlighted row. FTXUI fires on_change only when the
            // selection CHANGES, and its render-time clamp already made row 0
            // "selected" -- so picking row 0 would otherwise never register.
            // The touch is recorded here, whatever FTXUI reports.
            const auto commit = [state, i, radio, sync_other] {
                radio->OnEvent(Event::Character(" "));
                state->qs[i].radio_touched = true;
                sync_other();
            };
            const auto commit_and_move = [state, i, commit, advance, other_index] {
                commit();
                if (state->qs[i].radio == other_index) {
                    Focus(state->qs[i].input);
                } else {
                    advance(i);
                }
            };
            const int rows = static_cast<int>(qs.entries.size());
            list = CatchEvent(radio, [state, i, radio, rows, commit, commit_and_move,
                                      advance](const Event& event) {
                if (event == Event::Tab) {   // skip ahead without picking
                    advance(i);
                    return true;
                }
                if (event == Event::Character(" ")) {
                    commit();
                    return true;
                }
                if (event == Event::Return) {
                    commit_and_move();
                    return true;
                }
                if (const int digit = DigitOf(event); digit > 0 && digit <= rows) {
                    // Move the highlight to that row, then pick it -- through
                    // the widget's own keys, so its hover and selection agree.
                    for (int k = 0; k < rows; ++k) radio->OnEvent(Event::ArrowUp);
                    for (int k = 1; k < digit; ++k) radio->OnEvent(Event::ArrowDown);
                    commit_and_move();
                    return true;
                }
                return false;
            });
        }
        qs.list = list;

        Components block{list};
        if (other_allowed) block.push_back(other_container);
        // ONE container child per question: the renderer addresses children
        // as ChildAt(question index).
        question_components.push_back(Container::Vertical(std::move(block)));
    }
    auto all = Container::Vertical(std::move(question_components));
    // A new modal starts on its first question -- never on whatever FTXUI's
    // container last defaulted to.
    Focus(state->qs[0].list);

    const std::string title =
        state->asker.empty() ? std::string("the assistant asks") : state->asker + " asks";
    auto body = Renderer(all, [all, state, &theme, title, count] {
        Elements rows;
        rows.push_back(text(title) | bold | color(theme.ColorFor("attention")));
        const auto name_of = [count](int i, const AskQuestion& question) {
            std::string name = count > 1 ? std::to_string(i + 1) + "/" + std::to_string(count)
                                         : std::string{};
            if (!question.header.empty()) name += (name.empty() ? "" : "  ") + question.header;
            return name;
        };
        if (state->reviewing) {
            int first_line = -1;
            for (int i = 0; i < count; ++i) {
                const auto& question = state->questions[i];
                if (i > 0) rows.push_back(minimal::client::ModalRule(theme));
                std::string name = name_of(i, question);
                if (name.empty()) name = question.question;
                rows.push_back(paragraph(name) | bold);
                rows.push_back(paragraph(ReviewLine(question, state->qs[i])));
                if (first_line < 0) first_line = static_cast<int>(rows.size()) - 1;
            }
            if (!state->interacted && first_line >= 0) rows[first_line] |= focus;
            return minimal::client::ModalFrame(theme, "review answers", std::move(rows),
                                               "enter: submit  ·  esc: edit answers");
        }
        for (int i = 0; i < count; ++i) {
            const auto& question = state->questions[i];
            if (i > 0) rows.push_back(minimal::client::ModalRule(theme));
            const std::string name = name_of(i, question);
            if (!name.empty()) rows.push_back(paragraph(name) | bold);
            rows.push_back(paragraph(question.question));
            rows.push_back(all->ChildAt(i)->Render());
        }
        if (!state->interacted) rows[0] |= focus;   // the title anchors the viewport
        const std::string hints =
            state->dismiss_armed
                ? "esc again: dismiss and discard your answers  ·  any other key: keep editing"
                : "enter: pick & next  ·  1-9: pick  ·  space: pick/toggle  ·  "
                  "tab: skip  ·  esc: dismiss";
        const std::string window_title =
            count > 1 ? std::to_string(count) + " questions" : std::string("question");
        return minimal::client::ModalFrame(theme, window_title, std::move(rows), hints);
    });

    const auto dismiss = [reply] { reply({v1::PROMPT_OUTCOME_DISMISSED, nullptr}); };
    // Runs BEFORE the widgets (CatchEvent's handler sees events first), so
    // Enter/Escape are offered to the focused widget explicitly and only
    // their unclaimed remainder is handled here.
    return CatchEvent(body, [state, all, submit, finish, dismiss](const Event& event) {
        state->interacted = true;
        if (state->reviewing) {
            if (event == Event::Return) {
                submit();
                return true;
            }
            if (event == Event::Escape) {
                state->reviewing = false;
                return true;
            }
            // Frozen: swallow everything the editing widgets could mutate
            // with, but let Ctrl+C reach the host's turn-cancel.
            return event != Event::CtrlC;
        }
        if (event == Event::Escape) {
            if (all->OnEvent(event)) {   // the text line: back to its list
                state->dismiss_armed = false;
                return true;
            }
            if (AnyAnswered(*state) && !state->dismiss_armed) {
                state->dismiss_armed = true;
                return true;
            }
            dismiss();
            return true;
        }
        if (event.is_mouse()) return false;   // hover must not disarm
        state->dismiss_armed = false;
        if (event == Event::Return) {
            if (!all->OnEvent(event)) finish();
            return true;
        }
        return false;
    });
}

ftxui::Component AskPromptRenderer::Make(
    const plugins::PromptView& prompt, const Theme& theme,
    std::function<void(plugins::PromptReply)> on_reply) const {
    return MakeAskModal(ParsePayload(prompt.payload), std::string(prompt.asker), theme,
                        std::move(on_reply));
}

}  // namespace minimal::ask_view
