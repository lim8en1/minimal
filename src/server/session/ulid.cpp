#include "minimal/session/ulid.hpp"

#include <chrono>
#include <cstdint>
#include <random>

namespace minimal::session {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32

std::uint64_t NowMillis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

std::string GenerateUlid() {
    std::uint8_t buf[16];

    std::uint64_t ts = NowMillis();
    for (int i = 5; i >= 0; --i) {  // big-endian, first six bytes
        buf[i] = static_cast<std::uint8_t>(ts & 0xFF);
        ts >>= 8;
    }

    std::random_device device;
    std::mt19937_64 engine(device());
    const std::uint64_t lo = engine();
    const std::uint64_t hi = engine();
    for (int i = 0; i < 8; ++i) {
        buf[6 + i] = static_cast<std::uint8_t>(lo >> (56 - 8 * i));
    }
    buf[14] = static_cast<std::uint8_t>(hi >> 56);
    buf[15] = static_cast<std::uint8_t>(hi >> 48);

    std::string out;
    out.resize(26);
    std::size_t pos = 0;
    std::uint64_t acc = 0;
    int bits = 0;
    for (int i = 0; i < 16 && pos < 25; ++i) {
        acc = (acc << 8) | buf[i];
        bits += 8;
        while (bits >= 5 && pos < 25) {
            bits -= 5;
            out[pos++] = kAlphabet[(acc >> bits) & 0x1F];
        }
    }
    // 128 bits hold exactly 25 full base32 digits plus 3 spare bits, which at
    // loop exit sit in acc's low three positions. The final digit top-aligns
    // them (its low two positions are zero), keeping the encoding
    // total-order-preserving like the other 25 digits.
    out[25] = kAlphabet[(acc << 2) & 0x1F];
    return out;
}

}  // namespace minimal::session
