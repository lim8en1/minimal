#include "minimal/hooks/hook_dispatcher.hpp"

#include <algorithm>
#include <utility>

#include "minimal/hooks/hook_shell.hpp"

namespace minimal::hooks {
namespace {

// The envelope every hook script receives, beyond whatever the emitter adds.
constexpr std::string_view kPointField = "hook_point";
constexpr std::string_view kTimestampField = "timestamp_unix_ms";

std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

HookDispatcher::HookDispatcher(common::Logger& logger)
    : logger_(logger),
      tasks_(kDefaultTaskQueueSize),
      dispatcher_thread_([this] {
          for (;;) {
              auto task = tasks_.Pop();
              if (!task) break;  // closed and drained
              try {
                  (*task)();
              } catch (const std::exception& error) {
                  logger_.warn("hook action task threw: {}", error.what());
              } catch (...) {
                  logger_.warn("hook action task threw a non-standard exception");
              }
              outstanding_.fetch_sub(1, std::memory_order_acq_rel);
          }
      }) {
    // The harness's own points. Reserved here, before any plugin loads, so a
    // plugin claiming a core.* name is an ordinary fatal collision (the
    // empty attribution marks these as built-ins, like core tools).
    RegisterPoint({kPreToolUse, true, {"tool_name", "permission_key", "category"}});
    RegisterPoint({kPostToolUse, false, {"tool_name", "permission_key", "category"}});
    RegisterPoint({kUserPromptSubmit, true, {}});
    RegisterPoint({kStop, false, {"stop_reason"}});
    RegisterPoint({kSessionCreated, false, {}});
    RegisterPoint({kSessionEnded, false, {}});
    RegisterPoint({kPreCompact, false, {"trigger"}});
    RegisterPoint({kPostCompact, false, {"trigger"}});
    RegisterPoint({kSubagentStarted, false, {}});
    RegisterPoint({kSubagentCompleted, false, {"stop_reason"}});
    RegisterPoint({kPermissionPrompted, false, {"tool_name", "permission_key", "category"}});
}

HookDispatcher::~HookDispatcher() {
    tasks_.Close();
    if (dispatcher_thread_.joinable()) dispatcher_thread_.join();
}

HookPoint HookDispatcher::RegisterPoint(HookPointSpec spec) {
    if (sealed_) {
        logger_.warn("hook point '{}' registered after hooks were configured; ignored -- "
                     "registration is a startup-only operation",
                     spec.name);
        return HookPoint{nullptr, std::move(spec)};
    }
    attributions_.push_back({attribution_, spec.name});
    // First registration stays bound: the duplicate is reported as a fatal
    // collision by the startup name check, so which one "won" must never
    // depend on this call being the only one.
    //
    // The name is COPIED before the move: argument evaluation order is
    // unspecified, and a PointEntry constructed first would leave spec.name a
    // moved-from string -- every point then keyed on the same empty name, and
    // the whole registry silently broken.
    std::string name = spec.name;
    auto [it, inserted] = points_.try_emplace(name, PointEntry{std::move(spec), {}});
    if (!inserted) {
        logger_.warn("hook point '{}' registered twice; the first registration is kept and the "
                     "collision is reported at startup",
                     it->second.spec.name);
    }
    return HookPoint{this, it->second.spec};
}

const HookPointSpec* HookDispatcher::Find(std::string_view point) const {
    const auto it = points_.find(point);
    return it == points_.end() ? nullptr : &it->second.spec;
}

std::vector<std::string> HookDispatcher::Configure(const config::HooksConfig& config) {
    std::vector<std::string> warnings;
    // Returned, not logged: the caller (main) prints them once, deduplicated
    // against the sanity check's identical findings. Logging here too made
    // every warning surface twice.
    const auto warn = [&](std::string message) {
        warnings.push_back(std::move(message));
    };

    for (const auto& action : config.hooks) {
        const auto it = points_.find(action.on);
        if (it == points_.end()) {
            warn("action on unknown hook point '" + action.on +
                 "' -- is the plugin that provides it enabled?");
            continue;
        }
        for (const auto& [field, value] : action.filter) {
            (void)value;
            const auto& filterable = it->second.spec.filterable;
            if (std::find(filterable.begin(), filterable.end(), field) == filterable.end()) {
                warn("action on '" + action.on + "' filters on field '" + field +
                     "', which the point does not declare filterable -- the action will never "
                     "match");
            }
        }
        if (action.filter.empty()) {
            warn("action on '" + action.on + "' has no filter -- it answers EVERY emission of "
                 "the point");
        }
        if (!it->second.spec.veto_capable) {
            warn("action on '" + action.on +
                 "' targets an observe-only point: an exit code of 2 has no effect there");
        }

        Action mapped;
        mapped.command = action.command;
        const int timeout_ms = action.timeout_ms.value_or(config.default_timeout_ms);
        mapped.timeout = std::chrono::milliseconds(timeout_ms);
        mapped.filter = action.filter;
        it->second.actions.push_back(std::move(mapped));
    }

    sealed_ = true;
    return warnings;
}

nlohmann::json HookDispatcher::Envelope(std::string_view point, nlohmann::json payload) {
    if (!payload.is_object()) payload = nlohmann::json::object();
    payload[kPointField] = std::string(point);
    payload[kTimestampField] = NowUnixMs();
    return payload;
}

bool HookDispatcher::FilterMatches(const Action& action, const nlohmann::json& payload) {
    // Every listed field must match. A field the payload does not carry, or
    // carries as a non-string, never matches: filters describe string payload
    // fields, and "absent" must not quietly mean "any".
    for (const auto& [field, expected] : action.filter) {
        const auto it = payload.find(field);
        if (it == payload.end() || !it->is_string()) return false;
        if (it->get_ref<const std::string&>() != expected) return false;
    }
    return true;
}

std::optional<std::string> HookDispatcher::RunActions(
    const PointEntry& entry, nlohmann::json payload,
    const std::function<bool()>& is_cancelled) const {
    for (const auto& action : entry.actions) {
        if (!FilterMatches(action, payload)) continue;
        const auto result =
            RunShellAction(action.command, payload.dump(), action.timeout, is_cancelled);
        switch (result.outcome) {
            case ShellActionOutcome::kPass:
                break;
            case ShellActionOutcome::kBlock:
                return result.block_reason;
            case ShellActionOutcome::kFailOpen:
                // THE fail-open rule, in one place. A veto-capable point whose
                // guard script is broken must never silently become a deny --
                // and must never become an allow either: it becomes nothing.
                logger_.warn("hook action on '{}' failed open ({}): {}",
                             entry.spec.name, result.fail_reason, action.command);
                break;
        }
    }
    return std::nullopt;
}

std::optional<std::string> HookDispatcher::Emit(
    std::string_view point, nlohmann::json payload,
    const std::function<bool()>& is_cancelled) const {
    const PointEntry* entry = FindEntry(point);
    if (entry == nullptr || entry->actions.empty()) return std::nullopt;
    return RunActions(*entry, Envelope(point, std::move(payload)), is_cancelled);
}

void HookDispatcher::EmitAsync(std::string_view point, nlohmann::json payload) const {
    const PointEntry* entry = FindEntry(point);
    if (entry == nullptr || entry->actions.empty()) return;

    // outstanding_ counts queued-plus-running tasks, so WaitForIdleForTesting
    // is exact: incremented before the push (a consumer may drain it before
    // TryPush returns), decremented after the task runs, un-incremented when
    // the push is refused.
    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    const PointEntry* immutable_entry = entry;
    auto task = [this, immutable_entry, payload = Envelope(point, std::move(payload))]() mutable {
        if (auto blocked = RunActions(*immutable_entry, std::move(payload), nullptr)) {
            // Observe-only points have no return path; a block verdict is
            // still interesting enough to log.
            logger_.warn("hook action on '{}' returned a block verdict, which the observe-only "
                         "point discards: {}",
                         immutable_entry->spec.name, *blocked);
        }
    };
    if (!tasks_.TryPush(std::function<void()>(std::move(task)))) {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
        dropped_.fetch_add(1, std::memory_order_acq_rel);
        logger_.warn("hook notification queue full; emission of '{}' dropped ({} total)",
                     point, dropped_.load(std::memory_order_relaxed));
    }
}

const HookDispatcher::PointEntry* HookDispatcher::FindEntry(std::string_view point) const {
    const auto it = points_.find(point);
    return it == points_.end() ? nullptr : &it->second;
}

void HookDispatcher::WaitForIdleForTesting() {
    while (tasks_.size() > 0 || outstanding_.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// --- HookPoint ---------------------------------------------------------------

std::optional<std::string> HookPoint::Emit(nlohmann::json payload,
                                           const std::function<bool()>& is_cancelled) const {
    if (dispatcher_ == nullptr) return std::nullopt;
    return dispatcher_->Emit(spec_.name, std::move(payload), is_cancelled);
}

void HookPoint::EmitAsync(nlohmann::json payload) const {
    if (dispatcher_ == nullptr) return;
    dispatcher_->EmitAsync(spec_.name, std::move(payload));
}

}  // namespace minimal::hooks
