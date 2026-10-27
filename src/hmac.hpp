#pragma once

#include <string>
#include <string_view>

// HMAC-SHA256 of `data` keyed by `key`, as 64 lowercase hex chars.
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// Constant-time check that `signature_hex` is the HMAC-SHA256 of `data`.
// Accepts upper- or lowercase hex; anything that isn't 64 hex chars fails.
bool verify_hmac_sha256_hex(std::string_view key, std::string_view data, std::string_view signature_hex);
