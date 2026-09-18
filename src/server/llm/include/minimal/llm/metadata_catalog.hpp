#pragma once

// Metadata catalogs (docs/EXTENSIBILITY.md §2.13): enrichment defaults for
// models a config ALREADY addresses. A second registry beside
// ModelCatalogRegistry -- not a third capability on it -- because that
// registry's kind slots are the provider wire shapes' (an entry's `backend`
// selects both how to chat and how to list), one catalog per kind,
// first-wins: an enrichment source that serves several backends at once
// (models.dev knows openai- and anthropic-shaped models alike) cannot claim
// one without displacing the listing catalog.
//
// The contract is a LOOKUP, not a List, for the same structural reason: an
// enrichment source answers for a model id that is already addressable --
// declared statically or listed by the backend's own catalog -- and core
// never asks it for an id that is not. A metadata answer can therefore only
// FILL FIELDS of an existing model (context window, output cap, and, through
// the AdvertisedEfforts ladder, variant siblings), never add one. That
// invariant is what makes auto-use safe: with metadata defaults on by
// default, a catalog that invents ids still cannot widen the addressable set.
//
// Unlike ModelCatalog::List (pure, no mutable state), a MetadataCatalog is
// EXPECTED to hold a cache: the point of the seam is a large third-party
// dataset read once and shared. Implementations own their concurrency
// discipline; Find() results are shared across threads like every other
// registry object, so Lookup must be safe for concurrent calls.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minimal/llm/model_catalog.hpp"  // DiscoveredModel

namespace minimal::llm {

// How strictly Lookup matches `model_id` against the catalog's ids.
enum class MetadataMatch {
    // Exact id equality. The safe default: a wrong answer here is invisible
    // (wrong window, wrong cap), so matching stays conservative unless the
    // operator asks otherwise.
    Full,
    // Either id contains the other as a substring -- catches dated ids
    // (`claude-sonnet-4-5-20250929` in the catalog vs `claude-sonnet-4-5`
    // configured) and vendor prefixes. Ambiguous (several candidates) yields
    // no match and a warning: enrichment guesses nothing.
    Substring,
};

struct MetadataQuery {
    // The <model> half being enriched -- already addressable, never invented.
    std::string model_id;
    // The entry's connection base_url, for catalogs that organize by endpoint
    // (models.dev's per-provider tables). Empty when the caller has none.
    std::string base_url;
    // The providers.json entry's `backend` kind. Lets one catalog serve
    // several backends and lets the caller stay ignorant of catalog-specific
    // selection rules.
    std::string backend;
    // The entry's own name (providers.json key) -- diagnostic context for
    // warnings, never a matching key.
    std::string provider_name;
    // The entry's `metadata_provider` pin, verbatim: the caller does not know
    // what a catalog's tables are called, so this is passed through untouched
    // and the catalog validates its own spelling. Empty when unpinned.
    std::string metadata_provider;
};

class MetadataCatalog {
public:
    virtual ~MetadataCatalog() = default;

    // The fields this catalog knows for `query`'s model, or nullopt.
    //
    // Only `id`, `context_window_tokens`, `max_tokens` and `metadata` of the
    // returned DiscoveredModel are consumed (id should echo the query);
    // core uses this struct so the enrichment call sites share the
    // MergeDiscoveredIntoStatic/EnrichFromDiscovery shapes exactly.
    //
    // nullopt means "no answer" -- NOT an error. Throws only on a
    // catalog-internal failure it wants reported once (implementations
    // should prefer degrading to nullopt; the caller negative-caches
    // enrichment misses implicitly by asking again on the next resolve).
    virtual std::optional<DiscoveredModel> Lookup(const MetadataQuery& query,
                                                  MetadataMatch match) const = 0;
};

// Kind-string -> catalog. Mirrors ModelCatalogRegistry exactly -- no closed
// enum, first registration for a kind wins, Register returns false on a
// collision so the loader can name both plugins -- with one addition:
// RegisterFallback, for catalogs willing to answer for ANY kind (modelsdev
// registers under the known kinds AND falls back for wire-shape plugins that
// load after it). Fallbacks are consulted in registration order after the
// kind's own slot.
class MetadataCatalogRegistry {
public:
    bool Register(std::string kind, std::shared_ptr<MetadataCatalog> catalog);

    // Appends to the fallback list. A null catalog or a repeat registration
    // of the same instance is refused (false): one fallback per plugin is the
    // shape, and double-firing one catalog would double-log every miss.
    bool RegisterFallback(std::shared_ptr<MetadataCatalog> catalog);

    // --- Attribution, for collision reporting (same seam as
    // ModelCatalogRegistry's; not thread-safe, registration is
    // single-threaded) -----------------------------------------------
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Collision {
        std::string kind;
        std::string first_plugin;
        std::string second_plugin;
    };
    const std::vector<Collision>& collisions() const { return collisions_; }

    // The kind slot, else nullptr. Does NOT consult fallbacks -- callers that
    // want the fallback chain use FindForQuery.
    MetadataCatalog* Find(std::string_view kind) const;

    // The Lookup target for a query: the kind's own catalog, else the
    // fallback chain. Multiple fallbacks answering in turn would make
    // enrichment order-dependent, so only the FIRST registered fallback that
    // answers wins -- the same first-wins rule the kind slots apply.
    MetadataCatalog* FindForQuery(const MetadataQuery& query) const;

    // The fallback chain, for diagnostics (the sanity check names what would
    // answer for a kind with no own slot).
    const std::vector<std::shared_ptr<MetadataCatalog>>& fallbacks() const {
        return fallbacks_;
    }

    bool Has(std::string_view kind) const;

    // Every kind with an own slot (sorted, like kinds()) -- NOT the fallback
    // coverage, which is "everything else" by definition.
    std::vector<std::string> kinds() const;

private:
    std::map<std::string, std::shared_ptr<MetadataCatalog>, std::less<>> catalogs_;
    std::map<std::string, std::string, std::less<>> registrant_;  // kind -> plugin that holds it
    std::vector<std::shared_ptr<MetadataCatalog>> fallbacks_;
    std::string attribution_;
    std::vector<Collision> collisions_;
};

}  // namespace minimal::llm
