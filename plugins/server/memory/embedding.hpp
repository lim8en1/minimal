#pragma once

// The embedding seam: everything the memory plugin needs from an embedding
// backend, and nothing else. Two implementations (http, sidecar) plus a null
// one behind it -- the strategy/config plumbing stays out of the store and
// the tools, which see only this interface.
//
// THREAD-SAFETY: Embed() may be called concurrently (the re-embed worker and
// a search racing). Implementations must hold no mutable state across calls
// or guard it -- the sidecar one serializes every call through a mutex
// around its one subprocess round trip; the http one is stateless by the
// same contract net::HttpClient carries.
//
// ERRORS: throw std::runtime_error with a self-describing message. The
// callers degrade (search falls back to lexical, re-embed marks the row
// failed with the message) rather than propagate.

#include <string>
#include <vector>

namespace minimal::memory {

struct Embedding {
    std::vector<float> values;
    int dim = 0;
};

class EmbeddingFunction {
public:
    virtual ~EmbeddingFunction() = default;

    // One call per batch; implementations batch internally where the backend
    // supports it. Order of the returned embeddings matches the input order.
    virtual std::vector<Embedding> Embed(const std::vector<std::string>& inputs) = 0;

    // Short backend descriptor for /memory and the digest, e.g.
    // "http://127.0.0.1:8080/v1 (nomic-embed-text)" or
    // "fastembed (BAAI/bge-small-en-v1.5, 384 dims)".
    virtual std::string Describe() const = 0;

    // Feeds the strategy signature ("http:<api_base>:<model>",
    // "sidecar:<model>"): a change forces re-embed of affected rows.
    virtual std::string Signature() const = 0;
};

}  // namespace minimal::memory
