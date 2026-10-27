#include "risk_config.hpp"

#include <fstream>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    throw std::runtime_error("risk config: " + msg);
}

// Catch typos like "staleness_window" — a silently ignored key is a limit you think is set but isn't.
void reject_unknown_keys(const json& obj, const std::set<std::string>& allowed, const std::string& where) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!allowed.count(it.key())) fail("unknown key '" + where + it.key() + "'");
    }
}

double read_number(const json& obj, const char* key, double current, const std::string& where) {
    if (!obj.contains(key)) return current;
    const json& v = obj[key];
    if (!v.is_number()) fail("'" + where + key + "' must be a number");
    return v.get<double>();
}

uint32_t read_uint(const json& obj, const char* key, uint32_t current, const std::string& where) {
    if (!obj.contains(key)) return current;
    const json& v = obj[key];
    if (!v.is_number_unsigned() || v.get<uint64_t>() > UINT32_MAX) {
        fail("'" + where + key + "' must be a non-negative integer");
    }
    return v.get<uint32_t>();
}

template <typename T, typename Reader>
void read_bounds(const json& limits, const char* key, Bounds<T>& bounds, Reader read) {
    if (!limits.contains(key)) return;
    const json& b = limits[key];
    const std::string where = std::string("limits.") + key + ".";
    if (!b.is_object()) fail("'limits." + std::string(key) + "' must be an object with min/max");
    reject_unknown_keys(b, {"min", "max"}, where);
    bounds.min = read(b, "min", bounds.min, where);
    bounds.max = read(b, "max", bounds.max, where);
}

} // namespace

void validate_risk_config(const RiskConfig& c) {
    auto check_bounds = [](const auto& b, const char* name) {
        if (!(b.min <= b.max)) fail(std::string(name) + ": min must be <= max");
    };
    check_bounds(c.max_spread_pct, "max_spread_pct");
    check_bounds(c.urgency_multiplier, "urgency_multiplier");
    check_bounds(c.stop_loss_threshold_pct, "stop_loss_threshold_pct");
    check_bounds(c.max_position_size, "max_position_size");

    if (!(c.max_spread_pct.min > 0.0))          fail("max_spread_pct.min must be > 0");
    if (!(c.urgency_multiplier.min > 0.0))      fail("urgency_multiplier.min must be > 0");
    if (!(c.stop_loss_threshold_pct.min > 0.0)) fail("stop_loss_threshold_pct.min must be > 0");
    if (c.max_position_size.min == 0)           fail("max_position_size.min must be >= 1");

    auto in = [](const auto& b, auto v) { return v >= b.min && v <= b.max; };
    const RiskParams& f = c.fallback;
    if (!in(c.max_spread_pct, f.max_spread_pct))                   fail("fallback.max_spread_pct outside limits");
    if (!in(c.urgency_multiplier, f.urgency_multiplier))           fail("fallback.urgency_multiplier outside limits");
    if (!in(c.stop_loss_threshold_pct, f.stop_loss_threshold_pct)) fail("fallback.stop_loss_threshold_pct outside limits");
    if (!in(c.max_position_size, f.max_position_size))             fail("fallback.max_position_size outside limits");

    if (!(c.staleness_window_s > 0.0)) fail("staleness_window_s must be > 0");
    if (!(c.max_message_age_s > 0.0))  fail("max_message_age_s must be > 0");
    if (!(c.max_future_skew_s >= 0.0)) fail("max_future_skew_s must be >= 0");
}

RiskConfig load_risk_config(const std::string& path) {
    RiskConfig config;

    std::ifstream in(path);
    if (!in) return config; // no file: compiled-in defaults

    json root = json::parse(in, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (root.is_discarded()) fail("'" + path + "' is not valid JSON");
    if (!root.is_object())   fail("top level must be an object");

    reject_unknown_keys(root, {"fallback", "limits", "staleness_window_s", "max_message_age_s", "max_future_skew_s"}, "");

    if (root.contains("fallback")) {
        const json& f = root["fallback"];
        if (!f.is_object()) fail("'fallback' must be an object");
        reject_unknown_keys(f, {"max_spread_pct", "urgency_multiplier", "stop_loss_threshold_pct",
                                "max_position_size", "risk_level"}, "fallback.");
        RiskParams& p = config.fallback;
        p.max_spread_pct          = read_number(f, "max_spread_pct", p.max_spread_pct, "fallback.");
        p.urgency_multiplier      = read_number(f, "urgency_multiplier", p.urgency_multiplier, "fallback.");
        p.stop_loss_threshold_pct = read_number(f, "stop_loss_threshold_pct", p.stop_loss_threshold_pct, "fallback.");
        p.max_position_size       = read_uint(f, "max_position_size", p.max_position_size, "fallback.");
        if (f.contains("risk_level")) {
            auto level = f["risk_level"].is_string() ? parse_risk_level(f["risk_level"].get<std::string>()) : std::nullopt;
            if (!level) fail("'fallback.risk_level' must be LOW, MODERATE or HIGH");
            p.risk_level = *level;
        }
    }

    if (root.contains("limits")) {
        const json& l = root["limits"];
        if (!l.is_object()) fail("'limits' must be an object");
        reject_unknown_keys(l, {"max_spread_pct", "urgency_multiplier", "stop_loss_threshold_pct",
                                "max_position_size"}, "limits.");
        read_bounds(l, "max_spread_pct", config.max_spread_pct, read_number);
        read_bounds(l, "urgency_multiplier", config.urgency_multiplier, read_number);
        read_bounds(l, "stop_loss_threshold_pct", config.stop_loss_threshold_pct, read_number);
        read_bounds(l, "max_position_size", config.max_position_size, read_uint);
    }

    config.staleness_window_s = read_number(root, "staleness_window_s", config.staleness_window_s, "");
    config.max_message_age_s  = read_number(root, "max_message_age_s", config.max_message_age_s, "");
    config.max_future_skew_s  = read_number(root, "max_future_skew_s", config.max_future_skew_s, "");

    validate_risk_config(config);
    return config;
}
