#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "order_book.hpp"
#include "param_store.hpp"
#include "ring_buffer.hpp"

// Consumer side of the tick pipeline: pops ticks off the SPSC ring buffer and
// feeds them to the order book as market orders. Must run on exactly one thread,
// and that thread must be the only one touching the OrderBook while it runs.
//
// If a ParamStore is supplied, each tick passes a pre-trade risk check first:
// it is rejected if it could push the net position beyond max_position_size,
// unless it reduces exposure (so a lowered limit never traps the position).
template <size_t Capacity>
class MatchingEngine {
    public:
        MatchingEngine(OrderBook& book, RingBuffer<Capacity>& ticks, const ParamStore* risk = nullptr)
            : book_(book), ticks_(ticks), risk_(risk) {}

        // Drain everything currently queued. Returns number of ticks processed.
        size_t process_pending() {
            Tick tick;
            size_t processed = 0;
            while (ticks_.pop(tick)) {
                handle(tick);
                ++processed;
            }
            bump(processed_, processed);
            return processed;
        }

        // Spin until `stop` is set, then drain whatever the producer pushed before
        // setting it (producer's pushes happen-before its release store of `stop`).
        void run(const std::atomic<bool>& stop) {
            while (!stop.load(std::memory_order_acquire)) {
                process_pending();
            }
            process_pending();
        }

        // Safe to call from any thread (e.g. a status printer).
        size_t  total_processed() const { return processed_.load(std::memory_order_relaxed); }
        size_t  total_rejected()  const { return rejected_.load(std::memory_order_relaxed); }
        int64_t net_position()    const { return position_.load(std::memory_order_relaxed); }

    private:
        void handle(const Tick& tick) {
            const int64_t position      = position_.load(std::memory_order_relaxed);
            const int64_t signed_shares = tick.is_buy ? int64_t{tick.shares} : -int64_t{tick.shares};

            if (risk_) {
                const int64_t limit     = risk_->current(ParamStore::now_ns()).params.max_position_size;
                const int64_t projected = position + signed_shares;
                const bool within_limit     = abs64(projected) <= limit;
                const bool reduces_exposure = abs64(projected) < abs64(position);
                if (!within_limit && !reduces_exposure) {
                    bump(rejected_, size_t{1});
                    return;
                }
            }

            const uint32_t filled = book_.match_market_order(tick.is_buy, tick.shares);
            position_.store(position + (tick.is_buy ? int64_t{filled} : -int64_t{filled}),
                            std::memory_order_relaxed);
        }

        static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

        // Counters have a single writer (this thread), so load+store instead of a locked RMW.
        template <typename T>
        static void bump(std::atomic<T>& counter, T by) {
            counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
        }

        OrderBook& book_;
        RingBuffer<Capacity>& ticks_;
        const ParamStore* risk_;
        std::atomic<size_t>  processed_{0};
        std::atomic<size_t>  rejected_{0};
        std::atomic<int64_t> position_{0}; // net shares: buys positive, sells negative
};
