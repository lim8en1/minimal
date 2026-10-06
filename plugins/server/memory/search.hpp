#pragma once

// Hybrid retrieval: lexical (FTS5 bm25) and semantic (cosine over cached
// vectors) candidate lists fused by Reciprocal Rank Fusion. RRF needs no
// score calibration between the two -- bm25 and cosine live on unrelated
// scales -- and an entry both lists agree on rises to the top. Either list
// alone still works: no embedder, or its breaker open, is plain lexical
// search; a query in words the entry never uses is found semantically.

#include <optional>
#include <string>
#include <vector>

#include "memory_store.hpp"
#include "policy.hpp"

namespace minimal::memory {

class EmbeddingService;

struct SearchRequest {
    std::optional<std::string> query;  // nullopt/empty = browse
    MemoryStore::Filters filters;
    int limit = kDefaultSearchLimit;
    // Pre-computed query vector (the store path already embedded the text);
    // when absent and a query is given, the service is asked for one.
    std::optional<std::vector<float>> query_vector;
};

struct SearchResult {
    EntryRow row;
    double score = 0;     // relative relevance, 1.0 = best hit of this search
    std::string snippet;  // body excerpt around lexical matches, may be empty
    bool semantic = false;  // the vector list contributed
};

// `embeddings` may be null (lexical only).
std::vector<SearchResult> HybridSearch(MemoryStore& store, EmbeddingService* embeddings,
                                       const SearchRequest& request);

}  // namespace minimal::memory
