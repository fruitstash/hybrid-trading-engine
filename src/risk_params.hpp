#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class RiskLevel : uint8_t { LOW, MODERATE, HIGH };

inline std::optional<RiskLevel> parse_risk_level(std::string_view s) {
    if (s == "LOW")      return RiskLevel::LOW;
    if (s == "MODERATE") return RiskLevel::MODERATE;
    if (s == "HIGH")     return RiskLevel::HIGH;
    return std::nullopt;
}

inline const char* to_string(RiskLevel level) {
    switch (level) {
        case RiskLevel::LOW:      return "LOW";
        case RiskLevel::MODERATE: return "MODERATE";
        case RiskLevel::HIGH:     return "HIGH";
    }
    return "UNKNOWN";
}

// The risk settings the hot path actually trades on. Trivially copyable so it
// can be published through a seqlock.
struct RiskParams {
    double    max_spread_pct;
    double    urgency_multiplier;
    double    stop_loss_threshold_pct;
    uint32_t  max_position_size;
    RiskLevel risk_level;
};
