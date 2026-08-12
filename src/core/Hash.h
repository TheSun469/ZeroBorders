#pragma once

#include "Types.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace zb {

// SHA-256 of a string, using Windows CNG (bcrypt).
TokenHash sha256(std::string_view s);

// Hex encoding/decoding of the 32-byte hash.
std::string toHex(const uint8_t* data, size_t len);
bool fromHex(std::string_view hex, uint8_t* out, size_t outLen);

inline std::string tokenToHex(const TokenHash& t) {
    return toHex(t.data(), t.size());
}

} // namespace zb
