#pragma once

// EmbeddingService -- owns the embedding backend, its circuit breaker and the
// one background worker that embeds new and pending rows.
//
// Why a worker: v1 embedded synchronously at the top of every memory tool
// call, so a dead backend cost every call a full timeout (and a broken
// sidecar a 60 s respawn). Here tool calls never wait for row embedding; the
// only synchronous embed is a single query/new-entry vector (EmbedNow), and
// that is breaker-guarded: after one failure the backend is skipped for a
// backoff window (30 s doubling to 5 min) and search degrades to lexical.
//
// The backend is created LAZILY, on the worker: the sidecar's first start may
// download model weights, which must not block plugin registration.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "embedding.hpp"
#include "memory_settings.hpp"
#include "minimal/config/secrets.hpp"

namespace minimal::common {
class Logger;
}

namespace minimal::memory {

class MemoryStore;

class CircuitBreaker {
public:
    using Clock = std::chrono::steady_clock;

    explicit CircuitBreaker(Clock::duration base = std::chrono::seconds(30),
                            Clock::duration max = std::chrono::minutes(5))
        : base_(base), max_(max) {}

    // Closed, or open but past its cooldown (one probe allowed through).
    bool Allow(Clock::time_point now) const;
    void RecordSuccess();
    void RecordFailure(Clock::time_point now, std::string reason);

    struct State {
        bool open = false;
        int consecutive_failures = 0;
        std::chrono::seconds retry_in{0};
        std::string last_error;
    };
    State Snapshot(Clock::time_point now) const;

private:
    mutable std::mutex mu_;
    Clock::duration base_, max_;
    int failures_ = 0;
    Clock::time_point open_until_{};
    std::string last_error_;
};

// Which backend, and how to build it -- resolved from settings at
// registration WITHOUT constructing anything. The signature is known up
// front, so rows embedded under another backend can be re-queued before the
// backend exists.
struct EmbeddingPlan {
    std::string strategy = "none";  // "http" | "sidecar" | "none"
    std::string signature = "none";
    // Throws std::runtime_error with the reason on failure. Null when
    // strategy == "none".
    std::function<std::unique_ptr<EmbeddingFunction>()> factory;
};
// `secrets` binds the http backend's `api_key` reference here, at load; null =
// the default resolver (the operator's store, no plugin namespaces). A
// reference that can never resolve (an unset env: variable) throws
// std::runtime_error; a stored secret not set yet fails per request instead.
EmbeddingPlan MakeEmbeddingPlan(const MemorySettings& settings,
                                const std::filesystem::path& data_dir,
                                const config::SecretResolver* secrets = nullptr);

class EmbeddingService {
public:
    struct Options {
        int batch_size = 32;
        std::chrono::milliseconds idle_poll{60000};
        CircuitBreaker::Clock::duration breaker_base = std::chrono::seconds(30);
        CircuitBreaker::Clock::duration breaker_max = std::chrono::minutes(5);
    };

    EmbeddingService(MemoryStore* store, EmbeddingPlan plan, common::Logger& logger,
                     Options options);
    EmbeddingService(MemoryStore* store, EmbeddingPlan plan, common::Logger& logger)
        : EmbeddingService(store, std::move(plan), logger, Options{}) {}
    ~EmbeddingService();

    EmbeddingService(const EmbeddingService&) = delete;
    EmbeddingService& operator=(const EmbeddingService&) = delete;

    // Starts the worker. Production calls it once; tests drive RunOnce()
    // instead to stay deterministic.
    void Start();

    bool configured() const { return static_cast<bool>(plan_.factory); }
    const std::string& signature() const { return plan_.signature; }

    // One synchronous embed for the tool thread: a query, or a just-stored
    // entry. nullopt when unconfigured, not yet started, or the breaker is
    // open -- callers degrade to lexical. Never creates the backend.
    std::optional<Embedding> EmbedNow(const std::string& text);

    // Wakes the worker: rows were added or changed.
    void Notify();

    // One worker iteration, synchronously: create the backend if needed and
    // allowed, then embed one batch of pending rows. True when it did work.
    bool RunOnce();

    // For /memory: backend description plus breaker state.
    std::string Describe() const;

private:
    void WorkerLoop();
    std::shared_ptr<EmbeddingFunction> Backend() const;
    bool EmbedRows();  // true when there were rows to process

    MemoryStore* store_;
    EmbeddingPlan plan_;
    common::Logger& logger_;
    Options options_;
    CircuitBreaker breaker_;

    mutable std::mutex backend_mu_;
    std::shared_ptr<EmbeddingFunction> backend_;

    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool notified_ = false;
    std::thread worker_;
};

}  // namespace minimal::memory
