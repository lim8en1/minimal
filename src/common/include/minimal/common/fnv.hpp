#pragma once

// 64-bit FNV-1a: a fast, deterministic, non-cryptographic hash for cache keys
// and change detection. Never a security boundary.

#include <cstdint>
#include <string_view>

namespace minimal::common {

// NOLINTBEGIN(readability-magic-numbers) -- the FNV spec's 64-bit parameters.
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;
// NOLINTEND(readability-magic-numbers)

// Folds `bytes` into `hash` (start from kFnv1a64OffsetBasis).
constexpr std::uint64_t Fnv1a64(std::string_view bytes,
                                std::uint64_t hash = kFnv1a64OffsetBasis) {
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

}  // namespace minimal::common
