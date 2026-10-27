#pragma once

#include <string>
#include <string_view>
#include "risk_config.hpp"
#include "risk_params.hpp"

struct ValidationResult {
    bool        ok = false;
    RiskParams  params{};       // valid only when ok
    double      timestamp = 0;  // message timestamp, valid only when ok
    bool        clamped = false; // at least one value was pulled into bounds
    std::string error;          // why it was rejected, when !ok
};

// Guardrail between the untrusted AI cold path and live risk state (spec §5).
//
// Wire format is a two-frame ZMQ message:
//   frame 0: JSON payload (the IPC data contract, without a signature field)
//   frame 1: lowercase hex HMAC-SHA256 of frame 0's exact bytes
// Signing the raw bytes avoids having Python and C++ agree on a canonical JSON
// serialisation, which is fragile (float formatting, key order, whitespace).
//
// Accepting is all-or-nothing: any bad field rejects the whole message.
// Not thread-safe; owned by the single IPC listener thread.
class ParamValidator {
    public:
        // Throws std::invalid_argument on an empty secret.
        ParamValidator(const RiskConfig& config, std::string secret);

        // `now_s` is wall-clock seconds since the Unix epoch (passed in for testability).
        ValidationResult validate(std::string_view payload, std::string_view signature_hex, double now_s);

    private:
        RiskConfig config_;
        std::string secret_;
        double last_accepted_timestamp_ = 0.0;
};
