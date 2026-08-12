#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace zb {

// Frame magic bytes 'Z','B' (0x5A 0x42) transmitted in network byte order
inline constexpr uint8_t kMagicByte0 = 0x5A;
inline constexpr uint8_t kMagicByte1 = 0x42;

inline constexpr uint8_t kProtocolVersion = 1;

inline constexpr uint16_t kDefaultUdpPort = 24800;
inline constexpr uint16_t kDefaultControlPort = 24801;
inline constexpr uint16_t kDefaultDataPort = 24802;

inline constexpr uint32_t kChunkSize = 64 * 1024;
inline constexpr uint32_t kMaxControlPayload = 64 * 1024;   // 64 KB
inline constexpr uint32_t kMaxDataPayload = 16 * 1024 * 1024; // 16 MB safety cap

using TokenHash = std::array<uint8_t, 32>;

enum class Channel : uint8_t {
    Control,
    Data,
};

// Capability bits exchanged in Hello/Welcome
enum CapabilityFlags : uint32_t {
    CAP_CLIPBOARD_TEXT  = 1u << 0,
    CAP_CLIPBOARD_IMAGE = 1u << 1,
    CAP_FILE_TRANSFER   = 1u << 2,
    CAP_CLIPBOARD_HTML  = 1u << 3,
};

// Features actually implemented in this build; updated as phases land.
inline constexpr uint32_t kCurrentCapabilities = 0;

inline constexpr size_t kTokenSize = 32;

} // namespace zb
