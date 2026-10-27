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
