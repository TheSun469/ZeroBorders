#include "Hash.h"

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#else
#include <openssl/sha.h>
#endif

namespace zb {

TokenHash sha256(std::string_view s) {
    TokenHash out{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(st)) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    st = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!NT_SUCCESS(st)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }

    st = BCryptHashData(hHash,
                        reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())),
                        static_cast<ULONG>(s.size()), 0);
    if (!NT_SUCCESS(st)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptHashData failed");
    }

    st = BCryptFinishHash(hHash, out.data(), static_cast<ULONG>(out.size()), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!NT_SUCCESS(st)) {
        throw std::runtime_error("BCryptFinishHash failed");
    }
#else
    SHA256(reinterpret_cast<const unsigned char*>(s.data()),
           s.size(),
           reinterpret_cast<unsigned char*>(out.data()));
#endif
    return out;
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(hex[data[i] >> 4]);
        s.push_back(hex[data[i] & 0x0F]);
    }
    return s;
}

bool fromHex(std::string_view hex, uint8_t* out, size_t outLen) {
    if (hex.size() != outLen * 2) return false;
    auto nibble = [](char c, uint8_t& v) -> bool {
        if (c >= '0' && c <= '9') { v = static_cast<uint8_t>(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { v = static_cast<uint8_t>(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { v = static_cast<uint8_t>(c - 'A' + 10); return true; }
        return false;
    };
    for (size_t i = 0; i < outLen; ++i) {
        uint8_t hi = 0, lo = 0;
        if (!nibble(hex[i * 2], hi) || !nibble(hex[i * 2 + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace zb
