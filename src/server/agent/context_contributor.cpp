#include "minimal/agent/context_contributor.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "minimal/common/utf8.hpp"

namespace minimal::agent {

ContextContributorRegistry::ContextContributorRegistry(common::Logger& logger)
    : logger_(logger) {}

void ContextContributorRegistry::SetAttribution(std::string plugin_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_ = std::move(plugin_name);
}

void ContextContributorRegistry::ClearAttribution() {
    std::lock_guard<std::mutex> lock(mutex_);
    attribution_.clear();
}

void ContextContributorRegistry::Register(std::shared_ptr<ContextContributor> contributor) {
    if (contributor == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({attribution_.empty() ? "core" : attribution_, std::move(contributor)});
}

void ContextContributorRegistry::RemoveAfter(std::size_t existing_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (existing_count < entries_.size()) entries_.resize(existing_count);
}

std::size_t ContextContributorRegistry::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void ContextContributorRegistry::Configure(ContextContributorSettings settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = std::move(settings);
}

std::string ContextContributorRegistry::Render(const ContextRequest& request) const {
    // Snapshot under the lock, call outside it: a contributor is plugin code
    // and may take its own locks; holding ours across it would serialize every
    // session creation behind the slowest contributor for no benefit.
    std::vector<Entry> entries;
    ContextContributorSettings settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries = entries_;
        settings = settings_;
    }

    std::string out;
    for (const auto& entry : entries) {
        if (std::find(settings.disabled.begin(), settings.disabled.end(), entry.plugin) !=
            settings.disabled.end()) {
            continue;
        }
        std::optional<ContextSection> section;
        try {
            section = entry.contributor->Contribute(request);
        } catch (const std::exception& error) {
            logger_.warn("session {}: context contributor from '{}' threw; skipped: {}",
                         request.session_id, entry.plugin, error.what());
            continue;
        } catch (...) {
            logger_.warn("session {}: context contributor from '{}' threw; skipped",
                         request.session_id, entry.plugin);
            continue;
        }
        if (!section || section->body.empty()) continue;

        std::string body = std::move(section->body);
        if (settings.max_bytes_per_section > 0 &&
            body.size() > static_cast<std::size_t>(settings.max_bytes_per_section)) {
            logger_.warn("session {}: context section '{}' from '{}' is {} bytes; cut to {}",
                         request.session_id, section->title, entry.plugin, body.size(),
                         settings.max_bytes_per_section);
            body = common::TruncateUtf8Bytes(
                       body, static_cast<std::size_t>(settings.max_bytes_per_section)) +
                   "\n[... section truncated]";
        }
        if (!out.empty()) out += "\n\n";
        if (!section->title.empty()) out += "## " + section->title + "\n";
        out += body;
    }
    return out;
}

}  // namespace minimal::agent
