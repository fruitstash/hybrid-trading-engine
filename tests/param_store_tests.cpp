#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include "param_store.hpp"

static int passed = 0, failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) { std::cout << "[PASS] " << name << "\n"; ++passed; }
    else           { std::cout << "[FAIL] " << name << "\n"; ++failed; }
}

// Every field derived from one counter, so a torn read shows up as a mismatch.
RiskParams params_for(uint32_t i) {
    return {static_cast<double>(i), static_cast<double>(i), static_cast<double>(i), i,
            static_cast<RiskLevel>(i % 3)};
}

bool consistent(const RiskParams& p) {
    const uint32_t i = p.max_position_size;
    return p.max_spread_pct == i && p.urgency_multiplier == i && p.stop_loss_threshold_pct == i &&
           p.risk_level == static_cast<RiskLevel>(i % 3);
}

int main() {
    constexpr int64_t SEC = 1'000'000'000;

    // ---------- single-threaded behaviour ----------
    {
        RiskConfig config; // fallback max_position_size=100, staleness 30s
        ParamStore store(config);

        ActiveParams a = store.current(5 * SEC);
        check("before any update: fallback used", !a.from_ai && a.params.max_position_size == 100);
        check("before any update: age reported as never", a.age_s < 0);

        RiskParams ai = config.fallback;
        ai.max_position_size = 500;
        ai.risk_level = RiskLevel::HIGH;
        store.publish(ai, 10 * SEC);

        a = store.current(10 * SEC);
        check("after publish: AI params used", a.from_ai && a.params.max_position_size == 500 &&
                                               a.params.risk_level == RiskLevel::HIGH);
        a = store.current(25 * SEC);
        check("within window: still AI, age tracked", a.from_ai && a.age_s == 15.0);
        a = store.current(40 * SEC);
        check("exactly at window edge: still AI", a.from_ai);
        a = store.current(40 * SEC + 1);
        check("past window: reverts to fallback", !a.from_ai && a.params.max_position_size == 100);
        check("past window: age still reported", a.age_s > 30.0);

        ai.max_position_size = 300;
        store.publish(ai, 41 * SEC);
        a = store.current(42 * SEC);
        check("fresh update after staleness: AI again", a.from_ai && a.params.max_position_size == 300);
    }
    {
        RiskConfig config;
        config.staleness_window_s = 0.5;
        ParamStore store(config);
        store.publish(params_for(7), 0);
        check("custom staleness window respected",
              store.current(SEC / 4).from_ai && !store.current(SEC).from_ai);
    }

    // ---------- seqlock: concurrent writer / reader never tears ----------
    {
        constexpr uint32_t WRITES = 2'000'000;
        RiskConfig config;
        config.staleness_window_s = 1e9; // never stale during the test
        ParamStore store(config);
        store.publish(params_for(1), 0);

        std::atomic<bool> done{false};
        bool torn = false, went_backwards = false;
        uint64_t reads = 0;

        std::thread writer([&] {
            for (uint32_t i = 2; i <= WRITES; ++i) store.publish(params_for(i), 0);
            done.store(true, std::memory_order_release);
        });
        std::thread reader([&] {
            uint32_t last = 0;
            while (!done.load(std::memory_order_acquire)) {
                RiskParams p = store.current(0).params;
                if (!consistent(p))             torn = true;
                if (p.max_position_size < last) went_backwards = true;
                last = p.max_position_size;
                ++reads;
            }
        });
        writer.join();
        reader.join();

        check("seqlock: no torn reads under contention", !torn);
        check("seqlock: reader never sees older value after newer", !went_backwards);
        check("seqlock: final value visible", store.current(0).params.max_position_size == WRITES);
        std::cout << "Concurrent reads performed: " << reads << "\n";
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
