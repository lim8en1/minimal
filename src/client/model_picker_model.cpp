#include "minimal/client/model_picker_model.hpp"

#include <algorithm>

#include "minimal/client/utf8.hpp"
#include "minimal/common/model_ref.hpp"

namespace minimal::client {

std::vector<ModelPickerRow> BuildModelPickerRows(
    const std::vector<v1::ModelSummary>& models, const std::string& filter) {
    const std::string lowered_filter = ToLower(filter);

    // Sorted by ref, so the grouping below is correct for ANY input order --
    // not just the server's ascending delivery. (Ref sort groups by provider
    // first, so one stable pass is all the 2-tier layout needs.)
    std::vector<const v1::ModelSummary*> ordered;
    ordered.reserve(models.size());
    for (const auto& model : models) ordered.push_back(&model);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const v1::ModelSummary* a, const v1::ModelSummary* b) {
                         return a->ref() < b->ref();
                     });

    std::vector<ModelPickerRow> rows;
    rows.reserve(ordered.size());  // undercounts by header-row count, still avoids most reallocation
    std::string current_provider;
    bool header_emitted = false;

    for (const auto* summary : ordered) {
        const auto ref = minimal::common::SplitModelRef(summary->ref());
        if (!ref.has_value()) continue;   // unlistable; the server guarantees this shape anyway
        const std::string provider = ref->provider;

        if (provider != current_provider) {
            current_provider = provider;
            header_emitted = false;
        }

        // Match against the full ref, not the bare model id: the filter must
        // reach the provider half too ("local" should surface local/*).
        if (!lowered_filter.empty() &&
            ToLower(summary->ref()).find(lowered_filter) == std::string::npos) {
            continue;
        }

        if (!header_emitted) {
            rows.push_back(ModelPickerRow{provider, "", /*header=*/true});
            header_emitted = true;
        }
        rows.push_back(ModelPickerRow{ref->model, summary->ref(), /*header=*/false});
    }
    return rows;
}

}  // namespace minimal::client
