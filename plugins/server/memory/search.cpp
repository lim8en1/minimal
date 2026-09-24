#include "search.hpp"

#include <algorithm>
#include <unordered_map>

#include "embedding_service.hpp"

namespace minimal::memory {
namespace {

// The standard RRF constant: dampens the gap between rank 1 and rank 2 so
// one list cannot dominate on a single strong hit.
constexpr double kRrfK = 60.0;

// A vector list always returns its k nearest, however unrelated. A hit the
// lexical side did not also find must be at least this fraction of the best
// cosine to count, so a small store does not pad every answer with noise.
constexpr double kVectorRelativeFloor = 0.5;

}  // namespace

std::vector<SearchResult> HybridSearch(MemoryStore& store, EmbeddingService* embeddings,
                                       const SearchRequest& request) {
    const int limit = std::max(request.limit, 1);
    std::vector<SearchResult> results;

    if (!request.query || request.query->empty()) {
        for (auto& row : store.GetMany(store.Browse(request.filters, limit), request.filters.scope)) {
            results.push_back({std::move(row), 0.0, {}, false});
        }
        return results;
    }

    const int candidates = std::max(limit * 4, 40);
    struct Fused {
        double rrf = 0;
        std::string snippet;
        bool lexical = false;
        bool semantic = false;
    };
    std::unordered_map<std::string, Fused> fused;
    std::vector<std::string> order;  // first-seen order, for deterministic ties
    auto touch = [&](const std::string& id) -> Fused& {
        auto [it, inserted] = fused.try_emplace(id);
        if (inserted) order.push_back(id);
        return it->second;
    };

    const auto lexical = store.Lexical(*request.query, request.filters, candidates);
    for (std::size_t rank = 0; rank < lexical.size(); ++rank) {
        Fused& f = touch(lexical[rank].id);
        f.rrf += 1.0 / (kRrfK + static_cast<double>(rank) + 1.0);
        f.snippet = lexical[rank].snippet;
        f.lexical = true;
    }

    std::optional<std::vector<float>> query_vector = request.query_vector;
    if (!query_vector && embeddings != nullptr && embeddings->configured()) {
        if (auto embedded = embeddings->EmbedNow(*request.query)) {
            query_vector = std::move(embedded->values);
        }
    }
    if (query_vector && embeddings != nullptr) {
        const auto allowed = store.AllowedIds(request.filters);
        const auto nearest = store.Nearest(*query_vector, embeddings->signature(),
                                           request.filters.scope, candidates,
                                           allowed ? &*allowed : nullptr);
        const double best = nearest.empty() ? 0.0 : nearest.front().score;
        for (std::size_t rank = 0; rank < nearest.size(); ++rank) {
            const bool also_lexical = fused.count(nearest[rank].id) != 0;
            if (!also_lexical && (best <= 0 || nearest[rank].score < best * kVectorRelativeFloor)) {
                continue;
            }
            Fused& f = touch(nearest[rank].id);
            f.rrf += 1.0 / (kRrfK + static_cast<double>(rank) + 1.0);
            f.semantic = true;
        }
    }

    std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        return fused[a].rrf > fused[b].rrf;
    });
    if (order.size() > static_cast<std::size_t>(limit)) order.resize(static_cast<std::size_t>(limit));
    if (order.empty()) return results;

    const double top = fused[order.front()].rrf;
    for (auto& row : store.GetMany(order, request.filters.scope)) {
        const Fused& f = fused[row.id];
        results.push_back({std::move(row), top > 0 ? f.rrf / top : 0.0, f.snippet, f.semantic});
    }
    return results;
}

}  // namespace minimal::memory
