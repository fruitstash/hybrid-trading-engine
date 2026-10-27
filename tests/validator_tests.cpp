#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "hmac.hpp"
#include "param_validator.hpp"
#include "risk_config.hpp"

using nlohmann::json;

static int passed = 0, failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) { std::cout << "[PASS] " << name << "\n"; ++passed; }
    else           { std::cout << "[FAIL] " << name << "\n"; ++failed; }
}

template <typename Fn>
bool throws(Fn fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}

static const std::string SECRET = "test-secret";
static constexpr double NOW = 1'800'000'000.0;

json good_payload(double timestamp = NOW) {
    return {
        {"timestamp", timestamp},
        {"strategy_id", "VOLATILITY_ADAPTIVE_V1"},
        {"parameters", {
            {"max_spread_pct", 0.05},
            {"risk_level", "MODERATE"},
            {"max_position_size", 500},
            {"urgency_multiplier", 1.25},
            {"stop_loss_threshold_pct", 0.015},
        }},
    };
}

// Sign and validate a raw string payload.
ValidationResult run_raw(ParamValidator& v, const std::string& payload, double now = NOW) {
    return v.validate(payload, hmac_sha256_hex(SECRET, payload), now);
}

ValidationResult run(ParamValidator& v, const json& payload, double now = NOW) {
    return run_raw(v, payload.dump(), now);
}

// Fresh validator per case so replay state doesn't leak between checks.
ValidationResult run_fresh(const json& payload, double now = NOW) {
    RiskConfig config;
    ParamValidator v(config, SECRET);
    return run(v, payload, now);
}

bool rejected_with(const ValidationResult& r, const std::string& fragment) {
    return !r.ok && r.error.find(fragment) != std::string::npos;
}

std::string write_temp(const std::string& name, const std::string& contents) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << contents;
    return path.string();
}

int main() {
    // ---------- HMAC-SHA256 (RFC 4231 test vectors) ----------
    {
        check("hmac: RFC 4231 case 2",
              hmac_sha256_hex("Jefe", "what do ya want for nothing?") ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
        const std::string key(20, '\x0b');
        check("hmac: RFC 4231 case 1",
              hmac_sha256_hex(key, "Hi There") ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

        const std::string sig = hmac_sha256_hex(SECRET, "payload");
        check("hmac verify: correct signature", verify_hmac_sha256_hex(SECRET, "payload", sig));
        std::string upper = sig;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        check("hmac verify: uppercase hex accepted", verify_hmac_sha256_hex(SECRET, "payload", upper));
        check("hmac verify: wrong key fails", !verify_hmac_sha256_hex("other", "payload", sig));
        check("hmac verify: tampered data fails", !verify_hmac_sha256_hex(SECRET, "payload!", sig));
        check("hmac verify: truncated signature fails", !verify_hmac_sha256_hex(SECRET, "payload", sig.substr(0, 63)));
        check("hmac verify: non-hex fails", !verify_hmac_sha256_hex(SECRET, "payload", std::string(64, 'z')));
        check("hmac verify: empty fails", !verify_hmac_sha256_hex(SECRET, "payload", ""));
    }

    // ---------- accept path ----------
    {
        auto r = run_fresh(good_payload());
        check("valid message accepted", r.ok);
        check("valid: values carried through unchanged",
              r.params.max_position_size == 500 && r.params.max_spread_pct == 0.05 &&
              r.params.urgency_multiplier == 1.25 && r.params.stop_loss_threshold_pct == 0.015 &&
              r.params.risk_level == RiskLevel::MODERATE && !r.clamped);
        check("valid: timestamp reported", r.timestamp == NOW);
    }

    // ---------- signature ----------
    {
        RiskConfig config;
        ParamValidator v(config, SECRET);
        const std::string payload = good_payload().dump();
        check("signature: wrong secret rejected",
              rejected_with(v.validate(payload, hmac_sha256_hex("wrong", payload), NOW), "signature"));
        std::string tampered = payload;
        tampered.replace(tampered.find("500"), 3, "999");
        check("signature: tampered payload rejected",
              rejected_with(v.validate(tampered, hmac_sha256_hex(SECRET, payload), NOW), "signature"));
        check("signature: missing signature rejected", rejected_with(v.validate(payload, "", NOW), "signature"));
        check("signature: checked before parsing (garbage + bad sig)",
              rejected_with(v.validate("{{{not json", "00", NOW), "signature"));
        check("empty secret throws at construction", throws([&] { ParamValidator bad(config, ""); }));
    }

    // ---------- malformed / wrong shape ----------
    {
        RiskConfig config;
        ParamValidator v(config, SECRET);
        check("malformed JSON rejected (no crash)", rejected_with(run_raw(v, "{\"timestamp\": 17765"), "malformed"));
        check("empty payload rejected", rejected_with(run_raw(v, ""), "malformed"));
        check("non-object payload rejected", rejected_with(run_raw(v, "[1,2,3]"), "object"));
    }
    {
        json p = good_payload(); p.erase("strategy_id");
        check("missing envelope field rejected", rejected_with(run_fresh(p), "exactly"));
        p = good_payload(); p["extra"] = 1;
        check("extra envelope field rejected", rejected_with(run_fresh(p), "exactly"));
        p = good_payload(); p["parameters"].erase("stop_loss_threshold_pct");
        check("missing parameter rejected", rejected_with(run_fresh(p), "exactly"));
        p = good_payload(); p["parameters"]["leverage"] = 10;
        check("unknown parameter rejected", rejected_with(run_fresh(p), "exactly"));
        p = good_payload(); p["parameters"] = "none";
        check("parameters not an object rejected", rejected_with(run_fresh(p), "object"));
        p = good_payload(); p["strategy_id"] = "";
        check("empty strategy_id rejected", rejected_with(run_fresh(p), "strategy_id"));
        p = good_payload(); p["timestamp"] = "now";
        check("string timestamp rejected", rejected_with(run_fresh(p), "timestamp"));
    }

    // ---------- types and sign ----------
    {
        json p = good_payload(); p["parameters"]["max_position_size"] = "500";
        check("size as string rejected", rejected_with(run_fresh(p), "integer"));
        p = good_payload(); p["parameters"]["max_position_size"] = 500.5;
        check("fractional size rejected", rejected_with(run_fresh(p), "integer"));
        p = good_payload(); p["parameters"]["max_position_size"] = -500;
        check("negative size rejected", rejected_with(run_fresh(p), "> 0"));
        p = good_payload(); p["parameters"]["max_position_size"] = 0;
        check("zero size rejected", rejected_with(run_fresh(p), "> 0"));
        p = good_payload(); p["parameters"]["max_spread_pct"] = 0.0;
        check("zero spread rejected", rejected_with(run_fresh(p), "max_spread_pct"));
        p = good_payload(); p["parameters"]["urgency_multiplier"] = -1.0;
        check("negative urgency rejected", rejected_with(run_fresh(p), "urgency_multiplier"));
        p = good_payload(); p["parameters"]["stop_loss_threshold_pct"] = true;
        check("boolean stop loss rejected", rejected_with(run_fresh(p), "stop_loss_threshold_pct"));
        p = good_payload(); p["parameters"]["risk_level"] = "YOLO";
        check("unknown risk level rejected", rejected_with(run_fresh(p), "risk_level"));
        p = good_payload(); p["parameters"]["risk_level"] = 2;
        check("numeric risk level rejected", rejected_with(run_fresh(p), "risk_level"));
        RiskConfig config;
        ParamValidator v(config, SECRET);
        check("huge exponent rejected (not finite)",
              !run_raw(v, R"({"timestamp":1800000000,"strategy_id":"S","parameters":{"max_spread_pct":1e400,)"
                          R"("risk_level":"LOW","max_position_size":5,"urgency_multiplier":1,"stop_loss_threshold_pct":0.01}})").ok);
    }

    // ---------- clamping into operator limits ----------
    {
        json p = good_payload(); p["parameters"]["urgency_multiplier"] = 50.0;
        auto r = run_fresh(p);
        check("urgency above max clamped", r.ok && r.params.urgency_multiplier == 2.0 && r.clamped);
        p = good_payload(); p["parameters"]["urgency_multiplier"] = 0.01;
        r = run_fresh(p);
        check("urgency below min clamped", r.ok && r.params.urgency_multiplier == 0.5 && r.clamped);
        p = good_payload(); p["parameters"]["max_spread_pct"] = 0.9;
        r = run_fresh(p);
        check("spread above max clamped", r.ok && r.params.max_spread_pct == 0.10);
        p = good_payload(); p["parameters"]["max_spread_pct"] = 0.00001;
        r = run_fresh(p);
        check("spread below min clamped", r.ok && r.params.max_spread_pct == 0.0005);
        p = good_payload(); p["parameters"]["stop_loss_threshold_pct"] = 0.5;
        r = run_fresh(p);
        check("stop loss above max clamped", r.ok && r.params.stop_loss_threshold_pct == 0.10);
        p = good_payload(); p["parameters"]["max_position_size"] = 1'000'000;
        r = run_fresh(p);
        check("size above cap clamped", r.ok && r.params.max_position_size == 1000 && r.clamped);
        p = good_payload(); p["parameters"]["max_position_size"] = 10'000'000'000ull;
        r = run_fresh(p);
        check("size beyond uint32 clamped (no overflow)", r.ok && r.params.max_position_size == 1000);
    }

    // ---------- freshness and replay ----------
    {
        check("too old rejected", rejected_with(run_fresh(good_payload(NOW - 10)), "too old"));
        check("within age accepted", run_fresh(good_payload(NOW - 4)).ok);
        check("future rejected", rejected_with(run_fresh(good_payload(NOW + 5)), "future"));
        check("small clock skew accepted", run_fresh(good_payload(NOW + 1)).ok);

        RiskConfig config;
        ParamValidator v(config, SECRET);
        check("replay: first copy accepted", run(v, good_payload(NOW)).ok);
        check("replay: identical message rejected", rejected_with(run(v, good_payload(NOW)), "replayed"));
        check("replay: older message rejected", rejected_with(run(v, good_payload(NOW - 1)), "replayed"));
        check("replay: newer message accepted", run(v, good_payload(NOW + 0.5)).ok);

        ParamValidator v2(config, SECRET);
        json bad = good_payload(NOW); bad["parameters"]["max_position_size"] = -1;
        run(v2, bad);
        check("rejected message doesn't advance replay window", run(v2, good_payload(NOW)).ok);
    }

    // ---------- operator config ----------
    {
        RiskConfig defaults;
        check("config: compiled-in defaults are consistent", !throws([&] { validate_risk_config(defaults); }));

        RiskConfig missing = load_risk_config("definitely/not/here.json");
        check("config: missing file -> defaults",
              missing.fallback.max_position_size == 100 && missing.staleness_window_s == 30.0);

        RiskConfig repo = load_risk_config(std::string(HTE_SOURCE_DIR) + "/config/risk_limits.json");
        check("config: repo's risk_limits.json loads",
              repo.max_position_size.max == 1000 && repo.fallback.risk_level == RiskLevel::LOW);

        RiskConfig partial = load_risk_config(write_temp("hte_partial.json", R"({
            // comments are allowed
            "staleness_window_s": 10,
            "limits": { "max_position_size": { "max": 250 } },
            "fallback": { "max_position_size": 50, "risk_level": "MODERATE" }
        })"));
        check("config: overrides applied",
              partial.staleness_window_s == 10.0 && partial.max_position_size.max == 250 &&
              partial.fallback.max_position_size == 50 && partial.fallback.risk_level == RiskLevel::MODERATE);
        check("config: untouched keys keep defaults",
              partial.max_position_size.min == 1 && partial.urgency_multiplier.max == 2.0);

        RiskConfig tight = partial;
        ParamValidator v(tight, SECRET);
        json p = good_payload(); // asks for 500
        check("config: custom cap used by validator", run(v, p).params.max_position_size == 250);

        check("config: unknown key throws",
              throws([] { load_risk_config(write_temp("hte_typo.json", R"({"staleness_window": 10})")); }));
        check("config: inverted bounds throw",
              throws([] { load_risk_config(write_temp("hte_inv.json", R"({"limits":{"urgency_multiplier":{"min":3,"max":1}}})")); }));
        check("config: fallback outside limits throws",
              throws([] { load_risk_config(write_temp("hte_out.json", R"({"fallback":{"max_position_size":5000}})")); }));
        check("config: wrong type throws",
              throws([] { load_risk_config(write_temp("hte_type.json", R"({"max_message_age_s":"5"})")); }));
        check("config: malformed JSON throws",
              throws([] { load_risk_config(write_temp("hte_bad.json", "{ nope")); }));
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
