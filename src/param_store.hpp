#pragma once

#include <chrono>
#include <cstdint>
#include "risk_config.hpp"
#include "risk_params.hpp"
#include "seqlock.hpp"

// What the hot path should trade on right now.
struct ActiveParams {
    RiskParams params;
    bool       from_ai; // false -> operator fallback (no update yet, or stale)
    double     age_s;   // seconds since last accepted AI update; < 0 if none ever
};

// Hand-off point between the IPC listener thread (single writer) and the
// matching thread (reader). Reading never blocks and never allocates.
// Staleness is judged on local steady-clock arrival time, so wall-clock jumps
// can't make old parameters look fresh.
class ParamStore {
    public:
        explicit ParamStore(const RiskConfig& config)
            : fallback_(config.fallback),
              staleness_ns_(static_cast<int64_t>(config.staleness_window_s * 1e9)),
              slot_(Snapshot{config.fallback, 0, false}) {}

        static int64_t now_ns() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Listener thread only. `received_ns` is from now_ns().
        void publish(const RiskParams& params, int64_t received_ns) {
            slot_.store(Snapshot{params, received_ns, true});
        }

        ActiveParams current(int64_t now_ns) const {
            const Snapshot s = slot_.load();
            if (!s.has_value) {
                return {fallback_, false, -1.0};
            }
            const int64_t age_ns = now_ns - s.received_ns;
            const double age_s = static_cast<double>(age_ns) / 1e9;
            if (age_ns > staleness_ns_) {
                return {fallback_, false, age_s};
            }
            return {s.params, true, age_s};
        }

    private:
        struct Snapshot {
            RiskParams params;
            int64_t    received_ns;
            bool       has_value;
        };

        const RiskParams fallback_;
        const int64_t    staleness_ns_;
        SeqLock<Snapshot> slot_;
};
