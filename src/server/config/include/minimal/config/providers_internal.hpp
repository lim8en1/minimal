#pragma once

// Helpers shared by providers_config.cpp (parse) and model_catalog_service.cpp
// (runtime resolution). Not part of the public config API.

#include "minimal/config/providers_types.hpp"

namespace minimal::config {

// A copy of `static_model` with its unset window / output cap filled from the
// discovered model. User-declared fields always win.
ResolvedModel EnrichFromDiscovery(const ResolvedModel& static_model,
                                  const llm::DiscoveredModel& discovered);

// The `<base>@<variant>` sibling of `base`, parameters derived against the
// base's resolved output cap through the (optional) variant-params hook.
ResolvedModel MakeVariantResolved(const ResolvedModel& base, const ModelVariant& variant,
                                  const VariantParamsHook& variant_params_hook = {});

// The overlay ResolvedModel for one discovered model: the entry's connection
// and provider-level defaults plus the backend's id, window and output cap.
ResolvedModel BuildDiscoveredModel(const ProviderEntry& entry,
                                   const llm::DiscoveredModel& discovered);

}  // namespace minimal::config
