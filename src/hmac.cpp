#include "hmac.hpp"

#include <array>
#include <stdexcept>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace {

constexpr size_t DIGEST_LEN = 32;
using Digest = std::array<unsigned char, DIGEST_LEN>;

Digest compute(std::string_view key, std::string_view data) {
    Digest out{};
    unsigned int len = 0;
    const unsigned char* ok = HMAC(EVP_sha256(),
                                   key.data(), static_cast<int>(key.size()),
                                   reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                                   out.data(), &len);
    if (!ok || len != DIGEST_LEN) {
        throw std::runtime_error("HMAC-SHA256 computation failed");
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    static constexpr char HEX[] = "0123456789abcdef";
    Digest d = compute(key, data);
    std::string out(DIGEST_LEN * 2, '0');
    for (size_t i = 0; i < DIGEST_LEN; ++i) {
        out[2 * i]     = HEX[d[i] >> 4];
        out[2 * i + 1] = HEX[d[i] & 0x0F];
    }
    return out;
}

bool verify_hmac_sha256_hex(std::string_view key, std::string_view data, std::string_view signature_hex) {
    if (signature_hex.size() != DIGEST_LEN * 2) return false;

    Digest given{};
    for (size_t i = 0; i < DIGEST_LEN; ++i) {
        int hi = hex_value(signature_hex[2 * i]);
        int lo = hex_value(signature_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        given[i] = static_cast<unsigned char>((hi << 4) | lo);
    }

    Digest expected = compute(key, data);
    // constant-time compare so timing doesn't leak how many bytes matched
    return CRYPTO_memcmp(given.data(), expected.data(), DIGEST_LEN) == 0;
}
