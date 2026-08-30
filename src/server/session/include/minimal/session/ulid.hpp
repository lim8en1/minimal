#pragma once

#include <string>

namespace minimal::session {

// ULID: 48-bit millisecond timestamp + 80 bits of randomness, encoded as 26
// Crockford base32 characters. Time-sortable, so ids order by creation in
// logs; the alphabet's character order matches its value order, so plain
// string comparison compares correctly.
std::string GenerateUlid();

}  // namespace minimal::session
