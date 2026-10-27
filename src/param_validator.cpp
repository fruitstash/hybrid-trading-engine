#include "param_validator.hpp"

#include <cmath>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "hmac.hpp"

using nlohmann::json;

namespace {

ValidationResult reject(std::string why) {
    ValidationResult r;
    r.error = std::move(why);
    return r;
}

bool has_only_keys(const json& obj, std::initializer_list<const char*> keys) {
    if (obj.size() != keys.size()) return false;
    for (const char* k : keys) {
        if (!obj.contains(k)) return false;
    }
    return true;
}

// Reads a finite, strictly positive number. Sets `error` and returns false otherwise.
bool read_positive(const json& params, const char* key, double& out, std::string& error) {
    const json& v = params[key];
    if (!v.is_number()) { error = std::string(key) + " must be a number"; return false; }
    double d = v.get<double>();
    if (!std::isfinite(d)) { error = std::string(key) + " must be finite"; return false; }
    if (d <= 0.0)          { error = std::string(key) + " must be > 0"; return false; }
    out = d;
    return true;
}

} // namespace

ParamValidator::ParamValidator(const RiskConfig& config, std::string secret)
    : config_(config), secret_(std::move(secret)) {
    if (secret_.empty()) {
        throw std::invalid_argument("ParamValidator: HMAC secret must not be empty");
    }
}

ValidationResult ParamValidator::validate(std::string_view payload, std::string_view signature_hex, double now_s) {
    // 1. Authenticate before parsing: unsigned input never reaches the JSON parser.
    if (!verify_hmac_sha256_hex(secret_, payload, signature_hex)) {
        return reject("signature mismatch");
    }

    // 2. Parse without exceptions; malformed JSON is discarded, never fatal.
    json root = json::parse(payload.begin(), payload.end(), nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) return reject("malformed JSON");
    if (!root.is_object())   return reject("payload must be a JSON object");
    if (!has_only_keys(root, {"timestamp", "strategy_id", "parameters"})) {
        return reject("payload must contain exactly timestamp, strategy_id, parameters");
    }

    // 3. Envelope fields.
    const json& ts = root["timestamp"];
    if (!ts.is_number() || !std::isfinite(ts.get<double>())) return reject("timestamp must be a finite number");
    const double timestamp = ts.get<double>();

    const json& sid = root["strategy_id"];
    if (!sid.is_string() || sid.get_ref<const std::string&>().empty()) {
        return reject("strategy_id must be a non-empty string");
    }

    // 4. Freshness / replay protection.
    if (timestamp < now_s - config_.max_message_age_s) return reject("message too old");
    if (timestamp > now_s + config_.max_future_skew_s) return reject("message timestamp in the future");
    if (timestamp <= last_accepted_timestamp_)         return reject("replayed or out-of-order message");

    // 5. Parameters: shape, types, sign, then clamp into operator limits.
    const json& p = root["parameters"];
    if (!p.is_object()) return reject("parameters must be an object");
    if (!has_only_keys(p, {"max_spread_pct", "risk_level", "max_position_size",
                           "urgency_multiplier", "stop_loss_threshold_pct"})) {
        return reject("parameters must contain exactly max_spread_pct, risk_level, max_position_size, "
                      "urgency_multiplier, stop_loss_threshold_pct");
    }

    std::string error;
    double spread = 0, urgency = 0, stop_loss = 0;
    if (!read_positive(p, "max_spread_pct", spread, error))             return reject(error);
    if (!read_positive(p, "urgency_multiplier", urgency, error))        return reject(error);
    if (!read_positive(p, "stop_loss_threshold_pct", stop_loss, error)) return reject(error);

    const json& size = p["max_position_size"];
    if (!size.is_number_integer()) return reject("max_position_size must be an integer");
    if (!size.is_number_unsigned() || size.get<uint64_t>() == 0) return reject("max_position_size must be > 0");
    const uint64_t raw_size = size.get<uint64_t>();

    const json& level = p["risk_level"];
    auto risk_level = level.is_string() ? parse_risk_level(level.get_ref<const std::string&>()) : std::nullopt;
    if (!risk_level) return reject("risk_level must be LOW, MODERATE or HIGH");

    ValidationResult r;
    r.ok        = true;
    r.timestamp = timestamp;
    r.params.max_spread_pct          = config_.max_spread_pct.clamp(spread);
    r.params.urgency_multiplier      = config_.urgency_multiplier.clamp(urgency);
    r.params.stop_loss_threshold_pct = config_.stop_loss_threshold_pct.clamp(stop_loss);
    r.params.max_position_size       = raw_size > config_.max_position_size.max
                                           ? config_.max_position_size.max
                                           : config_.max_position_size.clamp(static_cast<uint32_t>(raw_size));
    r.params.risk_level              = *risk_level;
    r.clamped = r.params.max_spread_pct != spread
             || r.params.urgency_multiplier != urgency
             || r.params.stop_loss_threshold_pct != stop_loss
             || r.params.max_position_size != raw_size;

    last_accepted_timestamp_ = timestamp;
    return r;
}
