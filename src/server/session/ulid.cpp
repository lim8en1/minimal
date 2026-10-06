#include "minimal/session/ulid.hpp"

#include <cstdint>
#include <random>

#include "minimal/common/clock.hpp"

// NOLINTBEGIN(readability-magic-numbers) -- the ULID spec
// (github.com/ulid/spec): a 48-bit big-endian millisecond timestamp, 80 random
// bits, 26 Crockford base32 characters of 5 bits each.

namespace minimal::session {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32

}  // namespace

std::string GenerateUlid() {
    std::uint8_t buf[16];

    auto ts = static_cast<std::uint64_t>(common::NowMillis());
    for (int i = 5; i >= 0; --i) {  // big-endian, first six bytes
        buf[i] = static_cast<std::uint8_t>(ts & 0xFF);
        ts >>= 8;
    }

    // Straight from random_device, not from an engine seeded by it. Seeding a
    // mt19937_64 with a single random_device() draw would cap the entropy of all
    // 80 random bits at the seed's 32 -- so same-millisecond collisions would be
    // governed by 2^32, putting birthday risk within reach of a few tens of
    // thousands of sessions. A collision here is a PRIMARY KEY failure thrown
    // out of CreateSession, so the cost of getting this wrong is not cosmetic.
    // thread_local: constructing a random_device opens /dev/urandom, and doing
    // that per call put a syscall pair in front of every session creation for no
    // benefit -- the object is stateless in the sense that matters here, so one
    // per thread is enough. Still random_device itself, never a seeded engine,
    // for the reason above.
    thread_local std::random_device device;
    for (int i = 0; i < 10; i += 2) {
        const std::uint32_t word = device();
        buf[6 + i] = static_cast<std::uint8_t>(word >> 8);
        buf[7 + i] = static_cast<std::uint8_t>(word);
    }

    // 26 digits x 5 bits = 130, so encoding 128 bits leaves 2 spare. The spec
    // puts them as zero padding at the FRONT (the first character therefore
    // carries only the top 3 bits of the timestamp), which is why the loop
    // starts with bits = 2 rather than appending a hand-made final digit.
    // Padding at the back would still be unique and would still sort correctly
    // -- MSB-first 5-bit grouping is order-preserving either way -- but no
    // standard ULID parser would read the result, and the type claims to be one.
    std::string out;
    out.resize(26);
    std::size_t pos = 0;
    std::uint64_t acc = 0;
    int bits = 2;
    for (const std::uint8_t byte : buf) {
        acc = (acc << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[pos++] = kAlphabet[(acc >> bits) & 0x1F];
        }
    }
    return out;
}

}  // namespace minimal::session

// NOLINTEND(readability-magic-numbers)
