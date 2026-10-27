#pragma once

#include <cstdint>
#include <string>
#include "risk_params.hpp"

template <typename T>
struct Bounds {
    T min;
    T max;
    T clamp(T v) const { return v < min ? min : (v > max ? max : v); }
};

// Operator-owned limits. These are set by a human (config file or compiled-in
// defaults) and can never be changed by the AI — they are what the AI's
// suggestions get clamped against.
struct RiskConfig {
    // Used at startup and whenever AI parameters go stale.
    RiskParams fallback{
        /*max_spread_pct=*/0.01,
        /*urgency_multiplier=*/1.0,
        /*stop_loss_threshold_pct=*/0.01,
        /*max_position_size=*/100,
        /*risk_level=*/RiskLevel::LOW,
    };

    // Accepted AI values are clamped into these ranges.
    Bounds<double>   max_spread_pct{0.0005, 0.10};
    Bounds<double>   urgency_multiplier{0.5, 2.0};
    Bounds<double>   stop_loss_threshold_pct{0.001, 0.10};
    Bounds<uint32_t> max_position_size{1, 1000};

    // Price protection for market orders: may fill at most
    // base_slippage_pct * urgency_multiplier beyond the best price.
    double base_slippage_pct = 0.002;

    // Effective position limit = max_position_size * scale for the current risk_level.
    struct { double low = 0.5, moderate = 0.75, high = 1.0; } risk_level_position_scale;

    double position_scale(RiskLevel level) const {
        switch (level) {
            case RiskLevel::LOW:      return risk_level_position_scale.low;
            case RiskLevel::MODERATE: return risk_level_position_scale.moderate;
            case RiskLevel::HIGH:     return risk_level_position_scale.high;
        }
        return risk_level_position_scale.low;
    }

    // After a stop loss flattens the position, ignore all ticks for this long. 0 disables.
    double stop_loss_cooldown_s = 60.0;

    // When the position is over the (possibly just lowered) limit, each tick sells
    // (or buys back) at most this many shares toward the limit, within price protection.
    uint32_t unwind_step_shares = 50;

    // No accepted update for this long -> revert to `fallback`.
    double staleness_window_s = 30.0;
    // Reject messages whose timestamp is older than this (replay protection)...
    double max_message_age_s = 5.0;
    // ...or further in the future than this (clock skew tolerance).
    double max_future_skew_s = 2.0;
};

// Load overrides from a JSON file on top of the compiled-in defaults. Any key
// left out keeps its default. A missing file returns the defaults unchanged.
// Throws std::runtime_error on malformed JSON, unknown keys, wrong types, or
// inconsistent limits — bad operator config should stop startup, not be guessed at.
RiskConfig load_risk_config(const std::string& path);

// Throws std::runtime_error if bounds are inverted or the fallback lies outside them.
void validate_risk_config(const RiskConfig& config);
