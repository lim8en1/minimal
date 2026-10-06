#include "embedding_service.hpp"

#include <algorithm>
#include <stdexcept>

#include "embedding_http.hpp"
#include "embedding_sidecar.hpp"
#include "memory_store.hpp"

#include "minimal/common/logger.hpp"
#include "policy.hpp"

namespace minimal::memory {

// ---------------------------------------------------------- CircuitBreaker

bool CircuitBreaker::Allow(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    return failures_ == 0 || now >= open_until_;
}

void CircuitBreaker::RecordSuccess() {
    std::lock_guard<std::mutex> lock(mu_);
    failures_ = 0;
    last_error_.clear();
}

void CircuitBreaker::RecordFailure(Clock::time_point now, std::string reason) {
    std::lock_guard<std::mutex> lock(mu_);
    ++failures_;
    auto backoff = base_;
    for (int i = 1; i < failures_ && backoff < max_; ++i) backoff *= 2;
    open_until_ = now + std::min(backoff, max_);
    last_error_ = std::move(reason);
}

CircuitBreaker::State CircuitBreaker::Snapshot(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    State state;
    state.consecutive_failures = failures_;
    state.open = failures_ > 0 && now < open_until_;
    if (state.open) {
        state.retry_in = std::chrono::duration_cast<std::chrono::seconds>(open_until_ - now);
    }
    state.last_error = last_error_;
    return state;
}

// ------------------------------------------------------------------- plan

EmbeddingPlan MakeEmbeddingPlan(const MemorySettings& settings,
                                const std::filesystem::path& data_dir,
                                const config::SecretResolver* secrets) {
    // "auto" walks sidecar -> http -> none. The winner is what /memory
    // reports, so the resolution is never a silent fallback.
    auto strategy = settings.strategy;
    if (strategy == StrategyKind::kAuto) {
        strategy = !settings.sidecar_model.empty() ? StrategyKind::kSidecar
                   : !settings.api_base.empty()    ? StrategyKind::kHttp
                                                   : StrategyKind::kNone;
    }
    EmbeddingPlan plan;
    if (strategy == StrategyKind::kHttp && !settings.api_base.empty()) {
        plan.strategy = "http";
        plan.signature = HttpEmbeddingFunction::SignatureFor(settings);
        // Bound here, at load: a malformed reference fails the plugin's load
        // rather than every embedding; one not resolvable yet (a secret not
        // stored, an unset env: variable) fails per request.
        static const config::SecretResolver kDefaultSecrets;
        const auto& resolver = secrets != nullptr ? *secrets : kDefaultSecrets;
        common::SecretHandle api_key =
            resolver.Bind(settings.api_key, settings.api_base + "/embeddings");
        plan.factory = [settings, api_key = std::move(api_key)] {
            return std::make_unique<HttpEmbeddingFunction>(settings, api_key, net::HttpClient{});
        };
    } else if (strategy == StrategyKind::kSidecar && !settings.sidecar_model.empty()) {
        plan.strategy = "sidecar";
        plan.signature = SidecarSignatureFor(settings);
        plan.factory = [settings, data_dir] {
            return MakeSidecarEmbeddingFunction(settings, data_dir);
        };
    }
    return plan;
}

// --------------------------------------------------------- EmbeddingService

EmbeddingService::EmbeddingService(MemoryStore* store, EmbeddingPlan plan, common::Logger& logger,
                                   Options options)
    : store_(store),
      plan_(std::move(plan)),
      logger_(logger),
      options_(options),
      breaker_(options.breaker_base, options.breaker_max) {}

EmbeddingService::~EmbeddingService() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    // Bounded: the worker is at most inside one backend call, which carries
    // its own timeout (http timeout_ms, sidecar request/startup timeouts).
    if (worker_.joinable()) worker_.join();
}

void EmbeddingService::Start() {
    if (!configured() || worker_.joinable()) return;
    worker_ = std::thread([this] { WorkerLoop(); });
}

std::shared_ptr<EmbeddingFunction> EmbeddingService::Backend() const {
    std::lock_guard<std::mutex> lock(backend_mu_);
    return backend_;
}

void EmbeddingService::Notify() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        notified_ = true;
    }
    cv_.notify_all();
}

std::optional<Embedding> EmbeddingService::EmbedNow(const std::string& text) {
    const auto backend = Backend();
    if (backend == nullptr || !breaker_.Allow(CircuitBreaker::Clock::now())) return std::nullopt;
    try {
        auto embeddings = backend->Embed({text});
        if (embeddings.size() != 1 || embeddings.front().values.empty()) {
            throw std::runtime_error("backend returned no embedding");
        }
        breaker_.RecordSuccess();
        return std::move(embeddings.front());
    } catch (const std::exception& error) {
        breaker_.RecordFailure(CircuitBreaker::Clock::now(), error.what());
        logger_.warn("memory: embedding failed, lexical-only for now: {}", error.what());
        return std::nullopt;
    }
}

bool EmbeddingService::RunOnce() {
    if (!configured()) return false;
    const auto now = CircuitBreaker::Clock::now();
    if (!breaker_.Allow(now)) return false;
    if (Backend() == nullptr) {
        try {
            auto created = plan_.factory();
            if (created == nullptr) throw std::runtime_error("backend factory returned nothing");
            std::lock_guard<std::mutex> lock(backend_mu_);
            backend_ = std::move(created);
        } catch (const std::exception& error) {
            breaker_.RecordFailure(CircuitBreaker::Clock::now(), error.what());
            logger_.warn("memory: {} embedding backend unavailable: {}", plan_.strategy, error.what());
            return false;
        }
        breaker_.RecordSuccess();
        logger_.info("memory: embedding backend ready: {}", Backend()->Describe());
    }
    return EmbedRows();
}

bool EmbeddingService::EmbedRows() {
    const auto backend = Backend();
    const auto rows = store_->PendingEmbeddings(options_.batch_size);
    if (rows.empty() || backend == nullptr) return false;
    std::vector<std::string> texts;
    texts.reserve(rows.size());
    for (const auto& row : rows) texts.push_back(row.text);
    try {
        const auto embeddings = backend->Embed(texts);
        if (embeddings.size() != rows.size()) {
            throw std::runtime_error("backend returned " + std::to_string(embeddings.size()) +
                                     " embeddings for " + std::to_string(rows.size()) + " inputs");
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            store_->StoreEmbedding(rows[i].id, embeddings[i], plan_.signature);
        }
        breaker_.RecordSuccess();
        return true;
    } catch (const std::exception&) {
        // Outage or poison row? A trivial canary tells them apart. Canary
        // fails: the backend is down -- trip the breaker and leave every row
        // pending for when it is back. Canary passes: something in THIS batch
        // is unembeddable -- retry row by row and mark only the rows that
        // fail on their own, so one bad row can neither wedge the queue nor
        // take its batch-mates down with it.
        try {
            (void)backend->Embed({"ok"});
        } catch (const std::exception& canary_error) {
            breaker_.RecordFailure(CircuitBreaker::Clock::now(), canary_error.what());
            logger_.warn("memory: embedding backend down, retrying later: {}", canary_error.what());
            return true;
        }
        for (const auto& row : rows) {
            try {
                auto one = backend->Embed({row.text});
                if (one.size() != 1) throw std::runtime_error("backend returned no embedding");
                store_->StoreEmbedding(row.id, one.front(), plan_.signature);
            } catch (const std::exception& row_error) {
                store_->MarkEmbedFailed({row.id}, row_error.what());
                logger_.warn("memory: entry {} cannot be embedded: {}", row.id, row_error.what());
            }
        }
        breaker_.RecordSuccess();
        return true;
    }
}

void EmbeddingService::WorkerLoop() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            notified_ = false;
        }
        bool did_work = false;
        try {
            did_work = RunOnce();
        } catch (const std::exception& error) {
            // A store failure (disk full, locked) must not kill the worker.
            logger_.warn("memory: embedding worker error: {}", error.what());
        }
        if (did_work) continue;

        // Idle, or the breaker is open: sleep until notified, the breaker's
        // retry time, or the idle poll -- whichever comes first.
        auto wait = options_.idle_poll;
        const auto state = breaker_.Snapshot(CircuitBreaker::Clock::now());
        if (state.open) {
            wait = std::min<std::chrono::milliseconds>(
                wait, std::chrono::duration_cast<std::chrono::milliseconds>(state.retry_in) +
                          kBreakerWakeSlack);
        }
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, wait, [this] { return stop_ || notified_; });
        if (stop_) return;
    }
}

std::string EmbeddingService::Describe() const {
    if (!configured()) return "none (lexical search only)";
    std::string out;
    if (const auto backend = Backend()) {
        out = backend->Describe();
    } else {
        out = plan_.strategy + " (not started)";
    }
    const auto state = breaker_.Snapshot(CircuitBreaker::Clock::now());
    if (state.open) {
        out += " -- unavailable, retry in " + std::to_string(state.retry_in.count()) +
               "s: " + state.last_error;
    }
    return out;
}

}  // namespace minimal::memory
