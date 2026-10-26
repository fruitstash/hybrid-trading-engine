#pragma once

#include <atomic>
#include <cstddef>
#include "order_book.hpp"
#include "ring_buffer.hpp"

// Consumer side of the tick pipeline: pops ticks off the SPSC ring buffer and
// feeds them to the order book as market orders. Must run on exactly one thread,
// and that thread must be the only one touching the OrderBook while it runs.
template <size_t Capacity>
class MatchingEngine {
    public:
        MatchingEngine(OrderBook& book, RingBuffer<Capacity>& ticks)
            : book_(book), ticks_(ticks) {}

        // Drain everything currently queued. Returns number of ticks processed.
        size_t process_pending() {
            Tick tick;
            size_t processed = 0;
            while (ticks_.pop(tick)) {
                book_.match_market_order(tick.is_buy, tick.shares);
                ++processed;
            }
            processed_ += processed;
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

        size_t total_processed() const { return processed_; }

    private:
        OrderBook& book_;
        RingBuffer<Capacity>& ticks_;
        size_t processed_ = 0;
};
