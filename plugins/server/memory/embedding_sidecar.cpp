#include "embedding_sidecar.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/sidecar/process.hpp"
#include "minimal/common/units.hpp"

namespace minimal::memory {
namespace {

// The embedded driver, launched via `python -I -c <this> <model> <cache_dir>`
// -- no temp files, the same convention tools-python's driver_source.hpp
// uses for its interpreter. `-I` (isolated mode) ignores the environment's
// PYTHONPATH/site dirs; fastembed still resolves through whichever
// interpreter's OWN site-packages this was launched with (pointing
// sidecar_python_path at one with fastembed installed is the operator's
// setup step, same shape as pointing the http backend at a running server).
//
// PROTOCOL, one JSON object per line each direction:
//   startup:            {"ready": true, "dim": N} | {"ready": false, "error": "..."}
//   parent -> child:    {"id": N, "texts": ["...", ...]}
//   child -> parent:    {"id": N, "ok": true, "embeddings": [[...], ...]}
//                     | {"id": N, "ok": false, "error": "..."}
//
// fd 0 is the protocol input (the launch spec dup2's the real stdin pipe
// there); fd 3 is the protocol output, dup'd to a private fd and the
// original closed immediately, same as the python interpreter's driver --
// nothing else in the process can inject a line on it. fd 1/2 (the child's
// REAL stdout+stderr -- fastembed's first-run model download prints a tqdm
// progress bar there) go to a plain diagnostics pipe the parent drains but
// never parses; undrained, that pipe filling would block the child forever.
constexpr std::string_view kSidecarDriverSource = R"PY(import sys, os, json

def _main():
    proto_in = os.fdopen(os.dup(0), "r", encoding="utf-8", errors="replace")
    proto_out = os.fdopen(os.dup(3), "w", encoding="utf-8")
    os.close(3)

    def emit(obj):
        proto_out.write(json.dumps(obj))
        proto_out.write("\n")
        proto_out.flush()

    try:
        from fastembed import TextEmbedding
    except Exception as e:
        emit({"ready": False, "error": "fastembed import failed: %s" % e})
        sys.exit(1)

    try:
        model = TextEmbedding(model_name=sys.argv[1], cache_dir=sys.argv[2])
        dim = len(next(model.embed(["warmup"])))
    except Exception as e:
        emit({"ready": False, "error": str(e)})
        sys.exit(1)

    emit({"ready": True, "dim": dim})

    while True:
        line = proto_in.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            emit({"ok": False, "error": "bad request: %s" % e})
            continue
        try:
            vectors = [v.tolist() for v in model.embed(req["texts"])]
            emit({"id": req.get("id"), "ok": True, "embeddings": vectors})
        except Exception as e:
            emit({"id": req.get("id"), "ok": False, "error": str(e)})

_main()
)PY";

constexpr int kProtoOutFd = 3;
constexpr std::size_t kMaxDiagBytes = 16 * common::kKiB;
// A generously large single protocol line: a big reindex batch's embeddings
// serialized as JSON. Above this is corruption, not a legitimate response.
constexpr std::size_t kMaxLineBytes = 256 * common::kMiB;

class SidecarEmbeddingFunction final : public EmbeddingFunction {
public:
    SidecarEmbeddingFunction(std::string python_path, std::string model,
                             std::filesystem::path cache_dir, int startup_timeout_ms,
                             int request_timeout_ms)
        : python_path_(std::move(python_path)),
          model_(std::move(model)),
          cache_dir_(std::move(cache_dir)),
          startup_timeout_ms_(startup_timeout_ms),
          request_timeout_ms_(request_timeout_ms) {
        std::lock_guard<std::mutex> lock(mu_);
        Spawn();  // throws on failure; MakeSidecarEmbeddingFunction propagates it
    }

    ~SidecarEmbeddingFunction() override {
        std::lock_guard<std::mutex> lock(mu_);
        Shutdown();
    }

    std::vector<Embedding> Embed(const std::vector<std::string>& inputs) override {
        if (inputs.empty()) return {};
        std::lock_guard<std::mutex> lock(mu_);
        // Embed() is documented callable concurrently (the reindex drain and
        // a search can race, since MemoryRuntime is one shared instance
        // across every session); a single subprocess with one pipe pair can
        // only safely carry one in-flight request, so this mutex IS the
        // serialization -- the python_interpreter's "one read loop, strict
        // FIFO" contract, simplified to one lock since nothing here needs
        // cancellation or concurrent dispatch.
        if (!alive_) Spawn();  // one respawn attempt; throws through on failure

        nlohmann::json request;
        request["id"] = ++next_id_;
        request["texts"] = inputs;
        // replace: `inputs` is ingested text, not guaranteed valid UTF-8 --
        // see embedding_http.cpp's identical fix.
        if (!WriteAll(in_fd_.get(),
                     request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
                         "\n")) {
            const std::string reason = death_reason_.empty() ? "write failed" : death_reason_;
            alive_ = false;
            throw std::runtime_error("memory: sidecar write failed (" + reason + ")");
        }

        const auto response_line = ReadProtocolLine(request_timeout_ms_);
        if (!response_line) {
            const std::string reason = death_reason_.empty()
                                           ? "no response within " +
                                                 std::to_string(request_timeout_ms_) + "ms"
                                           : death_reason_;
            alive_ = false;
            throw std::runtime_error("memory: sidecar embed failed: " + reason);
        }
        return ParseEmbedResponse(*response_line, inputs.size());
    }

    std::string Describe() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return "fastembed (" + model_ + ", " + std::to_string(dim_hint_) + " dims)";
    }

    std::string Signature() const override { return "sidecar:" + model_; }

private:
    std::vector<Embedding> ParseEmbedResponse(const std::string& line, std::size_t expected) {
        nlohmann::json response;
        try {
            response = nlohmann::json::parse(line);
            if (!response.value("ok", false)) {
                throw std::runtime_error(response.value("error", std::string("unknown error")));
            }
            const auto embeddings = response.find("embeddings");
            if (embeddings == response.end() || !embeddings->is_array() ||
                embeddings->size() != expected) {
                throw std::runtime_error("expected embeddings[" + std::to_string(expected) + "]");
            }
            std::vector<Embedding> out;
            out.reserve(expected);
            for (const auto& vector : *embeddings) {
                Embedding embedding;
                embedding.values.reserve(vector.size());
                for (const auto& value : vector) embedding.values.push_back(value.get<float>());
                embedding.dim = static_cast<int>(embedding.values.size());
                out.push_back(std::move(embedding));
            }
            if (!out.empty() && dim_hint_ == 0) dim_hint_ = out.front().dim;
            return out;
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("memory: sidecar embed failed: ") + error.what());
        }
    }

    // Called under mu_. Kills whatever this instance currently owns (a no-op
    // the first time, no child yet), launches a fresh process, and blocks for the
    // ready/error handshake. Throws std::runtime_error on any failure.
    void Spawn() {
        Shutdown();
        death_reason_.clear();
        line_buffer_.clear();
        diag_tail_.clear();

        std::error_code ec;
        std::filesystem::create_directories(cache_dir_, ec);

        sidecar::LaunchSpec spec;
        spec.argv = {python_path_, "-I", "-c", std::string(kSidecarDriverSource), model_,
                    cache_dir_.string()};
        // The driver touches no relative paths, but Launch()'s child always
        // chdir()s to spec.cwd -- an empty default fails that unconditionally
        // (see minimal::sidecar::Launch), so this must name a directory that
        // is guaranteed to exist; cache_dir_ (just created above) is as good
        // as any.
        spec.cwd = cache_dir_;
        spec.pipes = {
            sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kRead, {{STDIN_FILENO, false}}},
            sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite,
                              {{STDOUT_FILENO, false}, {STDERR_FILENO, false}}},
            sidecar::PipePlan{sidecar::PipePlan::ChildEnd::kWrite, {{kProtoOutFd, true}}},
        };
        auto launched = sidecar::Launch(spec);
        if (!launched.ok) {
            throw std::runtime_error("memory: cannot start sidecar " + python_path_ + ": " +
                                     launched.spawn_error);
        }
        child_ = sidecar::Child(launched.pid);
        in_fd_ = std::move(launched.parent_fds[0]);
        diag_fd_ = std::move(launched.parent_fds[1]);
        out_fd_ = std::move(launched.parent_fds[2]);

        const auto ready_line = ReadProtocolLine(startup_timeout_ms_);
        if (!ready_line) {
            const std::string reason = death_reason_.empty() ? "no ready line within " +
                                                                    std::to_string(startup_timeout_ms_) +
                                                                    "ms"
                                                              : death_reason_;
            Shutdown();
            throw std::runtime_error("memory: sidecar did not become ready (" + reason + ")");
        }
        try {
            const auto ready = nlohmann::json::parse(*ready_line);
            if (!ready.value("ready", false)) {
                throw std::runtime_error(ready.value("error", std::string("unknown error")));
            }
            dim_hint_ = ready.value("dim", 0);
        } catch (const std::exception& error) {
            Shutdown();
            throw std::runtime_error(std::string("memory: sidecar failed to start: ") +
                                     error.what());
        }
        alive_ = true;
    }

    // Called under mu_. Polls out_fd_ and diag_fd_ together until a complete
    // line arrives on out_fd_, EOF/an error ends the wait (death_reason_ is
    // set), or timeout_ms elapses (death_reason_ stays empty -- a timeout is
    // not necessarily death, just slow, but the caller treats it the same:
    // one bad round trip kills this instance's belief that the process is
    // usable, and the next call respawns).
    std::optional<std::string> ReadProtocolLine(int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        bool diag_open = diag_fd_.get() >= 0;
        while (true) {
            if (const auto pos = line_buffer_.find('\n'); pos != std::string::npos) {
                std::string line = line_buffer_.substr(0, pos);
                line_buffer_.erase(0, pos + 1);
                return line;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int slice = static_cast<int>(std::min<std::int64_t>(200, remaining));

            pollfd fds[2];
            int nfds = 0;
            const int out_slot = nfds++;
            fds[out_slot] = pollfd{out_fd_.get(), POLLIN, 0};
            const int diag_slot = diag_open ? nfds++ : -1;
            if (diag_open) fds[diag_slot] = pollfd{diag_fd_.get(), POLLIN, 0};

            const int rc = poll(fds, static_cast<nfds_t>(nfds), slice);
            if (rc < 0) {
                if (errno == EINTR) continue;
                death_reason_ = "poll error";
                return std::nullopt;
            }
            char chunk[8192];
            if (fds[out_slot].revents & (POLLIN | POLLHUP | POLLERR)) {
                const ssize_t n = read(out_fd_.get(), chunk, sizeof(chunk));
                if (n > 0) {
                    line_buffer_.append(chunk, static_cast<std::size_t>(n));
                    if (line_buffer_.size() > kMaxLineBytes) {
                        death_reason_ = "sidecar produced an oversized protocol line";
                        return std::nullopt;
                    }
                } else if (n == 0 || errno != EINTR) {
                    death_reason_ = "sidecar exited" + (diag_tail_.empty() ? "" : ": " + diag_tail_);
                    return std::nullopt;
                }
            }
            if (diag_slot >= 0 && (fds[diag_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
                const ssize_t n = read(diag_fd_.get(), chunk, sizeof(chunk));
                if (n > 0) {
                    diag_tail_.append(chunk, static_cast<std::size_t>(n));
                    if (diag_tail_.size() > kMaxDiagBytes) {
                        diag_tail_.erase(0, diag_tail_.size() - kMaxDiagBytes);
                    }
                } else if (n == 0 || errno != EINTR) {
                    diag_open = false;  // diagnostics closed; the protocol side keeps this alive
                }
            }
        }
    }

    static bool WriteAll(int fd, const std::string& data) {
        std::size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = write(fd, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Called under mu_ (constructor, destructor, and Spawn()'s own reset).
    // Always a hard kill -- SIGKILL the group, bounded-reap, forget the pgid
    // -- the same shape HardKill uses elsewhere in the tree (no graceful
    // stdin-EOF path exists there either: a D-state or wedged driver must
    // not hang teardown). A no-op when nothing has been launched yet.
    void Shutdown() {
        in_fd_.reset();
        out_fd_.reset();
        diag_fd_.reset();
        child_.Terminate();
        alive_ = false;
    }

    std::string python_path_, model_;
    std::filesystem::path cache_dir_;
    int startup_timeout_ms_, request_timeout_ms_;

    mutable std::mutex mu_;
    sidecar::ScopedFd in_fd_, out_fd_, diag_fd_;
    sidecar::Child child_;  // kills and reaps the driver on destruction and on Shutdown
    bool alive_ = false;
    std::string line_buffer_, diag_tail_, death_reason_;
    int dim_hint_ = 0;
    std::uint64_t next_id_ = 0;
};

}  // namespace

std::unique_ptr<EmbeddingFunction> MakeSidecarEmbeddingFunction(
    const MemorySettings& settings, const std::filesystem::path& data_dir) {
    if (settings.sidecar_model.empty()) {
        throw std::runtime_error("memory: sidecar_model is not set");
    }
    std::string python_path = settings.sidecar_python_path;
    if (python_path.empty()) python_path = sidecar::ResolveOnPath("python3");
    if (python_path.empty()) {
        throw std::runtime_error("memory: python3 not found on PATH (set sidecar_python_path)");
    }
    return std::make_unique<SidecarEmbeddingFunction>(
        python_path, settings.sidecar_model, data_dir / "fastembed_cache",
        settings.sidecar_startup_timeout_ms, settings.sidecar_request_timeout_ms);
}

std::string SidecarSignatureFor(const MemorySettings& settings) {
    return "sidecar:" + settings.sidecar_model;
}

}  // namespace minimal::memory
