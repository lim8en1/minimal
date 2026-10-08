#pragma once

// session-export's host half: the "session-export" modal and the `/export`
// command that opens it.
//
// The modal reads the focused session whole on open (on the feed's worker:
// the UI thread never blocks on the server), then offers a file path --
// defaulting to a session-id-based name in the client's start directory --
// two filters (thinking, tool calls), and where to start: the first message
// or any compaction. Enter writes the file, also on the worker; the outcome
// stays in the modal's pinned line, since a plugin has no seam for a tab
// notice. Escape is the host's: it closes every plugin modal.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "export_render.hpp"
#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/task_worker.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/sdk/views.hpp"
#include "minimal/client/sdk/widgets.hpp"

namespace minimal::session_export {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

inline constexpr const char* kExportViewId = "session-export";

// The data half: the transcript the modal offers, and the outcome line.
// Load/Export queue on the worker; everything else is a locked read.
class ExportFeed {
public:
    // `ui` (optional): repainted after every task, so "loading" and the
    // export outcome show without a keystroke.
    ExportFeed(plugins::ClientApi* api, plugins::HostUi* ui);

    // Reads `session_id` afresh; a load still running for an earlier open is
    // discarded when it lands. An empty id (a tab with no session yet)
    // clears the transcript and says so.
    void Load(const std::string& session_id);
    // Renders `options` over the loaded transcript and writes `path`. The
    // outcome line is dropped if a Load ran meanwhile (the modal reopened);
    // the file is written regardless.
    void Export(std::filesystem::path path, ExportOptions options);
    // An outcome decided on the UI thread (a bad path, an armed overwrite).
    void SetStatus(std::string status, bool error);

    // Null until the latest Load lands (or when it failed).
    std::shared_ptr<const Transcript> transcript() const;
    std::string status() const;
    bool status_is_error() const;

private:
    plugins::ClientApi* api_;
    mutable std::mutex mu_;
    std::shared_ptr<const Transcript> transcript_;
    std::uint64_t load_id_ = 0;
    std::string status_;
    bool status_error_ = false;
    plugins::TaskWorker worker_;  // last: see TaskWorker's LIFETIME note
};

// What the modal's widgets point into. UI thread only; reset on every open.
struct ExportForm {
    std::string session_id;
    std::string path;
    int path_cursor = 0;  // outlives the Input, which a transcript landing rebuilds
    bool thinking = false;
    bool tool_calls = true;
    int start = 0;
    std::vector<StartPoint> starts;
    std::vector<std::string> start_labels;  // Radiobox borrows these
    std::string armed_overwrite;            // the existing path a second Enter replaces
};

class ExportView final : public plugins::View {
public:
    // `start_dir`: where a relative path (and the default name) lands.
    ExportView(plugins::ClientApi* api, plugins::FocusProvider* focus, plugins::HostUi* ui,
               const plugins::WidgetKit* widgets, std::filesystem::path start_dir);

    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // Fresh form for the focused session, and a fresh read of it.
    void OnShow() const override;

private:
    std::shared_ptr<ExportFeed> feed_;
    std::shared_ptr<ExportForm> form_ = std::make_shared<ExportForm>();
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
    plugins::FocusProvider* focus_;
    const plugins::WidgetKit* widgets_;
    std::filesystem::path start_dir_;
    client::Theme theme_;
};

// `/export`: opens the modal. Reading a transcript changes nothing the
// session holds, so it is offered on a subagent tab too.
class ExportCommand final : public plugins::ClientCommand {
public:
    explicit ExportCommand(plugins::HostUi* ui) : ui_(ui) {}
    std::string_view name() const override { return "export"; }
    std::string_view summary() const override {
        return "save this session's transcript to a .md or .txt file";
    }
    bool requires_session() const override { return true; }
    bool read_only() const override { return true; }
    bool Run(const std::vector<std::string>&, const std::string&) override;

private:
    plugins::HostUi* ui_;
};

// Where `typed` points: `~/` expands to $HOME, a relative path lands in
// `start_dir`. Empty for an empty or all-blank entry.
std::filesystem::path ResolvePath(const std::string& typed, const std::filesystem::path& start_dir);

}  // namespace minimal::session_export
