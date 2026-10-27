#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "order_book.hpp"
#include "param_store.hpp"
#include "ring_buffer.hpp"

// Consumer side of the tick pipeline: pops ticks off the SPSC ring buffer and
// feeds them to the order book as market orders. Must run on exactly one thread,
// and that thread must be the only one touching the OrderBook while it runs.
//
// If a ParamStore is supplied, every tick goes through the live risk parameters,
// in this order:
//   0. cooldown           - drop every tick for stop_loss_cooldown_s after a stop loss
//   1. stop loss          - if the open position is down more than
//                           stop_loss_threshold_pct, flatten it, start the cooldown,
//                           and drop the tick
//   2. unwind             - if |position| is over the limit (e.g. the limit was just
//                           lowered), trade up to unwind_step_shares back toward it,
//                           within price protection; then carry on with the tick
//   3. spread gate        - reject if either side is empty or
//                           (ask - bid) / mid > max_spread_pct
//   4. position limit     - reject if |net position| could exceed
//                           max_position_size * scale(risk_level), unless the
//                           tick reduces exposure
//   5. price protection   - fill no further than
//                           base_slippage_pct * urgency_multiplier past the best price
// Without a ParamStore, ticks go straight to the book unchecked.
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
        size_t  total_processed()     const { return processed_.load(std::memory_order_relaxed); }
        size_t  rejected_spread()     const { return rejected_spread_.load(std::memory_order_relaxed); }
        size_t  rejected_position()   const { return rejected_position_.load(std::memory_order_relaxed); }
        size_t  stop_loss_triggers()  const { return stop_losses_.load(std::memory_order_relaxed); }
        size_t  rejected_cooldown()   const { return rejected_cooldown_.load(std::memory_order_relaxed); }
        size_t  unwind_orders()       const { return unwinds_.load(std::memory_order_relaxed); }
        size_t  total_rejected()      const {
            return rejected_spread() + rejected_position() + stop_loss_triggers() + rejected_cooldown();
        }
        int64_t net_position()        const { return position_.load(std::memory_order_relaxed); }
        double  avg_entry_price()     const { return avg_price_.load(std::memory_order_relaxed); }

    private:
        void handle(const Tick& tick) {
            if (!risk_) {
                apply_fill(tick.is_buy, book_.match_market_order(tick.is_buy, tick.shares));
                return;
            }

            const RiskConfig& cfg = risk_->config();
            const int64_t now = ParamStore::now_ns();
            const RiskParams p = risk_->current(now).params;

            // 0. cooldown after a stop loss
            if (now < cooldown_until_ns_) {
                bump(rejected_cooldown_, size_t{1});
                return;
            }

            // 1. stop loss on the existing position, marked at the price we could exit at
            if (stop_loss_hit(p, book_.best_bid(), book_.best_ask())) {
                const int64_t pos = position_.load(std::memory_order_relaxed);
                const bool close_with_buy = pos < 0;
                apply_fill(close_with_buy, book_.match_market_order(close_with_buy, static_cast<uint32_t>(abs64(pos))));
                bump(stop_losses_, size_t{1});
                cooldown_until_ns_ = now + static_cast<int64_t>(cfg.stop_loss_cooldown_s * 1e9);
                return;
            }

            const int64_t limit = std::max<int64_t>(1, static_cast<int64_t>(
                std::floor(p.max_position_size * cfg.position_scale(p.risk_level))));
            const double band = cfg.base_slippage_pct * p.urgency_multiplier;

            // 2. unwind one step toward the limit if we're over it
            const int64_t held = position_.load(std::memory_order_relaxed);
            if (abs64(held) > limit) {
                const bool reduce_with_buy = held < 0;
                const uint32_t step = static_cast<uint32_t>(std::min<int64_t>(abs64(held) - limit, cfg.unwind_step_shares));
                const double ref = reduce_with_buy ? book_.best_ask() : book_.best_bid();
                if (ref > 0.0) {
                    const double limit_price = reduce_with_buy ? ref * (1.0 + band) : ref * (1.0 - band);
                    apply_fill(reduce_with_buy, book_.match_market_order(reduce_with_buy, step, limit_price));
                    bump(unwinds_, size_t{1});
                }
            }

            // 3. spread gate
            const double bid = book_.best_bid();
            const double ask = book_.best_ask();
            if (bid <= 0.0 || ask <= 0.0 || (ask - bid) / ((ask + bid) / 2.0) > p.max_spread_pct) {
                bump(rejected_spread_, size_t{1});
                return;
            }

            // 4. position limit, scaled by risk level
            const int64_t position  = position_.load(std::memory_order_relaxed);
            const int64_t projected = position + (tick.is_buy ? int64_t{tick.shares} : -int64_t{tick.shares});
            if (abs64(projected) > limit && abs64(projected) >= abs64(position)) {
                bump(rejected_position_, size_t{1});
                return;
            }

            // 5. price protection band
            const double limit_price = tick.is_buy ? ask * (1.0 + band) : bid * (1.0 - band);
            apply_fill(tick.is_buy, book_.match_market_order(tick.is_buy, tick.shares, limit_price));
        }

        bool stop_loss_hit(const RiskParams& p, double bid, double ask) const {
            const int64_t pos = position_.load(std::memory_order_relaxed);
            const double entry = avg_price_.load(std::memory_order_relaxed);
            if (pos == 0 || entry <= 0.0) return false;
            const double exit_price = pos > 0 ? bid : ask;
            if (exit_price <= 0.0) return false; // nothing to exit against; can't mark
            const double loss_pct = pos > 0 ? (entry - exit_price) / entry : (exit_price - entry) / entry;
            return loss_pct > p.stop_loss_threshold_pct;
        }

        // Update net position and volume-weighted entry price from a fill.
        void apply_fill(bool is_buy, const Fill& fill) {
            if (fill.shares == 0) return;
            const int64_t pos   = position_.load(std::memory_order_relaxed);
            const double  entry = avg_price_.load(std::memory_order_relaxed);
            const int64_t qty   = is_buy ? int64_t{fill.shares} : -int64_t{fill.shares};
            const int64_t next  = pos + qty;

            double next_entry;
            if (pos == 0 || (pos > 0) == (qty > 0)) {
                // opening or adding: blend entry prices
                next_entry = (abs64(pos) * entry + fill.notional) / static_cast<double>(abs64(next));
            } else if (next == 0) {
                next_entry = 0.0;                // flat
            } else if ((next > 0) == (pos > 0)) {
                next_entry = entry;              // partially reduced: entry unchanged
            } else {
                next_entry = fill.avg_price();   // flipped sides: new position entered at this fill
            }

            position_.store(next, std::memory_order_relaxed);
            avg_price_.store(next_entry, std::memory_order_relaxed);
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
        std::atomic<size_t>  rejected_spread_{0};
        std::atomic<size_t>  rejected_position_{0};
        std::atomic<size_t>  stop_losses_{0};
        std::atomic<size_t>  rejected_cooldown_{0};
        std::atomic<size_t>  unwinds_{0};
        int64_t cooldown_until_ns_ = 0;     // engine thread only
        std::atomic<int64_t> position_{0};  // net shares: buys positive, sells negative
        std::atomic<double>  avg_price_{0}; // volume-weighted entry price of the open position
};
