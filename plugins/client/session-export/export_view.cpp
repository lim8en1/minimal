#include "export_view.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <system_error>
#include <utility>

#include <ftxui/component/event.hpp>

#include "minimal/common/atomic_write.hpp"
#include "transcript_fetch.hpp"

namespace minimal::session_export {

namespace {

std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The modal: rebuilt when it is reopened or the transcript it offers
// changes; the form state it edits lives in ExportForm, so a rebuild keeps
// what was typed. Render-time rebuild keeps the worker off the UI's objects
// (the mcp-view shape).
class ExportComponent final : public ftxui::ComponentBase {
public:
    ExportComponent(std::shared_ptr<ExportFeed> feed, std::shared_ptr<ExportForm> form,
                    std::shared_ptr<std::atomic<bool>> stale, const plugins::WidgetKit* widgets,
                    const client::Theme* theme, std::filesystem::path start_dir)
        : feed_(std::move(feed)),
          form_(std::move(form)),
          stale_(std::move(stale)),
          widgets_(widgets),
          theme_(theme),
          start_dir_(std::move(start_dir)) {}

    ftxui::Element OnRender() override {
        Sync();
        return body_->Render();
    }

    bool OnEvent(ftxui::Event event) override {
        Sync();
        if (event == ftxui::Event::Return) {
            Submit();
            return true;
        }
        const bool up = event == ftxui::Event::ArrowUp;
        const bool down = event == ftxui::Event::ArrowDown;
        // The start list follows the cursor: a highlighted row IS the pick,
        // with no separate space to commit it (Radiobox alone selects only
        // on space). At the list's edge the arrow moves focus instead.
        if (radio_ && radio_->Focused() && (up || down) && radio_->OnEvent(event)) {
            radio_->OnEvent(ftxui::Event::Character(" "));
            return true;
        }
        // Focus moves here, not in the container: the single-line Input
        // claims the arrows (and would type a space meant for a checkbox).
        if (up || down) return MoveFocus(up ? -1 : 1, /*wrap=*/false);
        if (event == ftxui::Event::Tab) return MoveFocus(1, /*wrap=*/true);
        if (event == ftxui::Event::TabReverse) return MoveFocus(-1, /*wrap=*/true);
        const ftxui::Component target = body_;
        return target->OnEvent(std::move(event));
    }

    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return body_; }

private:
    bool MoveFocus(int step, bool wrap) {
        const int count = static_cast<int>(focus_order_.size());
        int at = 0;
        for (int i = 0; i < count; ++i) {
            if (focus_order_[i]->Focused()) at = i;
        }
        int next = at + step;
        if (wrap) {
            next = (next + count) % count;
        } else {
            next = std::clamp(next, 0, count - 1);
        }
        focus_order_[next]->TakeFocus();
        return true;
    }

    void Sync() {
        auto transcript = feed_->transcript();
        const bool reopened = stale_->exchange(false);
        if (body_ && !reopened && transcript == built_) return;
        built_ = transcript;
        // A load landing mid-edit rebuilds the widgets: keep focus where it was.
        int focused = 0;
        for (int i = 0; !reopened && i < static_cast<int>(focus_order_.size()); ++i) {
            if (focus_order_[i]->Focused()) focused = i;
        }

        auto& form = *form_;
        form.starts = transcript ? BuildStartPoints(*transcript) : std::vector<StartPoint>{};
        form.start_labels.clear();
        for (const auto& start : form.starts) form.start_labels.push_back(start.label);
        if (form.start < 0 || form.start >= static_cast<int>(form.starts.size())) form.start = 0;

        using namespace ftxui;
        InputOption input_option;
        input_option.multiline = false;
        auto input = Input(&form.path, "file path", input_option);
        auto thinking = Checkbox("export thinking", &form.thinking);
        auto tools = Checkbox("export tool calls (and their results)", &form.tool_calls);
        Components children{input, thinking, tools};
        radio_ = nullptr;
        if (!form.start_labels.empty()) {
            RadioboxOption radio_option;
            radio_option.entries = &form.start_labels;
            radio_option.selected = &form.start;
            radio_option.focused_entry = form.start;
            radio_ = Radiobox(radio_option);
            children.push_back(radio_);
        }
        focus_order_ = children;
        auto container = Container::Vertical(std::move(children));

        auto body = Renderer(container, [this, input, thinking, tools] {
            const client::Theme& theme = *theme_;
            const Decorator muted = client::Muted(theme);
            Elements rows;
            rows.push_back(text("file") | bold);
            rows.push_back(hbox({text(" "), input->Render() | flex}));
            rows.push_back(text(" .md: markdown  ·  .txt: plain text  ·  relative to " +
                                start_dir_.string()) |
                           muted);
            rows.push_back(widgets_->ModalRule(theme));
            rows.push_back(thinking->Render());
            rows.push_back(tools->Render());
            rows.push_back(widgets_->ModalRule(theme));
            rows.push_back(text("start from") | bold);
            if (radio_) {
                rows.push_back(radio_->Render());
            } else {
                rows.push_back(text(" (waiting for the transcript)") | muted);
            }

            Elements pinned;
            if (const std::string status = feed_->status(); !status.empty()) {
                pinned.push_back(paragraph(status) |
                                 (feed_->status_is_error() ? color(theme.ColorFor("attention"))
                                                           : muted));
            }
            return widgets_->ModalFrame(
                theme, "export session", std::move(rows), std::move(pinned),
                "enter: export  ·  tab/↑↓: move  ·  space: toggle  ·  esc: close",
                client::ModalAccent::Focus);
        });

        if (body_) DetachAllChildren();
        body_ = body;
        Add(body_);
        // A fresh open starts on the path: the field most worth a look.
        focus_order_[std::min<std::size_t>(focused, focus_order_.size() - 1)]->TakeFocus();
    }

    void Submit() {
        auto& form = *form_;
        const auto transcript = feed_->transcript();
        if (!transcript) {
            feed_->SetStatus("the transcript has not loaded yet", true);
            return;
        }
        const std::filesystem::path path = ResolvePath(form.path, start_dir_);
        if (path.empty()) {
            feed_->SetStatus("type a file path", true);
            return;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            feed_->SetStatus(path.string() + " is a directory", true);
            return;
        }
        if (std::filesystem::exists(path, ec) && form.armed_overwrite != path.string()) {
            form.armed_overwrite = path.string();
            feed_->SetStatus(path.string() + " exists -- enter again to overwrite it", true);
            return;
        }
        form.armed_overwrite.clear();

        ExportOptions options;
        options.thinking = form.thinking;
        options.tool_calls = form.tool_calls;
        options.compaction = form.starts.empty() ? std::nullopt : form.starts[form.start].compaction;
        options.plain_text = IsPlainText(path);
        options.session_id = form.session_id;
        feed_->SetStatus("exporting...", false);
        feed_->Export(path, std::move(options));
    }

    std::shared_ptr<ExportFeed> feed_;
    std::shared_ptr<ExportForm> form_;
    std::shared_ptr<std::atomic<bool>> stale_;
    const plugins::WidgetKit* widgets_;
    const client::Theme* theme_;
    std::filesystem::path start_dir_;
    std::shared_ptr<const Transcript> built_;
    ftxui::Component body_;
    ftxui::Component radio_;
    ftxui::Components focus_order_;  // path, checkboxes, start list
};

}  // namespace

std::filesystem::path ResolvePath(const std::string& typed, const std::filesystem::path& start_dir) {
    const auto first = typed.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    std::string text = typed.substr(first, typed.find_last_not_of(" \t") - first + 1);
    if (text == "~" || text.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            text = std::string(home) + text.substr(1);
        }
    }
    std::filesystem::path path(text);
    if (path.is_relative()) path = start_dir / path;
    return path.lexically_normal();
}

ExportFeed::ExportFeed(plugins::ClientApi* api, plugins::HostUi* ui)
    : api_(api), worker_("session-export", [ui] {
          if (ui != nullptr) ui->RequestRedraw();
      }) {}

void ExportFeed::Load(const std::string& session_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        transcript_.reset();
        status_ = "reading the transcript...";
        status_error_ = false;
        id = ++load_id_;
        if (session_id.empty()) {
            status_ = "this tab has no session to export yet";
            status_error_ = true;
            return;
        }
    }
    worker_.Post([this, session_id, id] {
        auto transcript = FetchTranscript(*api_, session_id);
        std::lock_guard<std::mutex> lock(mu_);
        if (id != load_id_) return;  // reopened meanwhile: that load owns the view
        if (transcript) {
            transcript_ = std::make_shared<const Transcript>(std::move(*transcript));
            status_.clear();
        } else {
            status_ = "could not read the transcript (the server did not answer) -- "
                      "reopen /export to retry";
            status_error_ = true;
        }
    });
}

void ExportFeed::Export(std::filesystem::path path, ExportOptions options) {
    auto transcript = this->transcript();
    if (!transcript) return;
    worker_.Post([this, path = std::move(path), options = std::move(options),
                  transcript = std::move(transcript)] {
        const std::string document = Render(*transcript, options);
        const auto outcome = common::write_file(path, document);
        std::string status;
        if (outcome.ok) {
            status = "exported " + std::to_string(CountExported(*transcript, options)) +
                     " messages to " + path.string();
        } else {
            status = "export failed: " + outcome.error;
        }
        SetStatus(std::move(status), !outcome.ok);
    });
}

void ExportFeed::SetStatus(std::string status, bool error) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = std::move(status);
    status_error_ = error;
}

std::shared_ptr<const Transcript> ExportFeed::transcript() const {
    std::lock_guard<std::mutex> lock(mu_);
    return transcript_;
}

std::string ExportFeed::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

bool ExportFeed::status_is_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_error_;
}

ExportView::ExportView(plugins::ClientApi* api, plugins::FocusProvider* focus, plugins::HostUi* ui,
                       const plugins::WidgetKit* widgets, std::filesystem::path start_dir)
    : feed_(std::make_shared<ExportFeed>(api, ui)),
      focus_(focus),
      widgets_(widgets),
      start_dir_(std::move(start_dir)) {}

const plugins::ViewSpec& ExportView::spec() const {
    static const plugins::ViewSpec kSpec{kExportViewId, "export session",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component ExportView::component() {
    return ftxui::Make<ExportComponent>(feed_, form_, stale_, widgets_, &theme_, start_dir_);
}

void ExportView::OnShow() const {
    auto& form = *form_;
    form = ExportForm{};
    form.session_id = focus_->FocusedSessionId();
    stale_->store(true);
    if (!form.session_id.empty()) {
        form.path = DefaultPath(start_dir_, form.session_id, NowUnixMs()).string();
    }
    feed_->Load(form.session_id);
}

bool ExportCommand::Run(const std::vector<std::string>&, const std::string&) {
    ui_->ShowView(kExportViewId);
    return true;
}

}  // namespace minimal::session_export
