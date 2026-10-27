#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cmath>
#include "matching_engine.hpp"

static int passed = 0, failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) { std::cout << "[PASS] " << name << "\n"; ++passed; }
    else           { std::cout << "[FAIL] " << name << "\n"; ++failed; }
}

int main() {
    // ---------- Part 1: single-threaded drain ----------
    {
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb);

        book->add_order(1, false, 50, 10.00);
        book->add_order(2, false, 50, 10.01);
        book->add_order(3, true,  40,  9.99);

        check("process_pending on empty buffer returns 0", engine.process_pending() == 0);

        rb->push(Tick{0.0, 30, true,  1});  // buy 30  -> 10.00 left 20
        rb->push(Tick{0.0, 30, true,  2});  // buy 30  -> 10.00 empty, 10.01 left 40
        rb->push(Tick{0.0, 15, false, 3});  // sell 15 -> 9.99 left 25

        check("process_pending drains all queued ticks", engine.process_pending() == 3);
        check("ticks applied in FIFO order (asks)",
              book->debug_ask_volume(10.00) == 0 && book->debug_ask_volume(10.01) == 40);
        check("sell tick hits bid side", book->debug_bid_volume(9.99) == 25);
        check("buffer empty after drain", engine.process_pending() == 0);
        check("total_processed counts all ticks", engine.total_processed() == 3);
    }

    // ---------- Part 2: producer thread -> engine thread ----------
    {
        constexpr uint32_t N = 50'000;
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<1024>>();
        MatchingEngine<1024> engine(*book, *rb);

        // book is populated before the engine thread starts; only the engine touches it after
        for (uint32_t i = 0; i < N; ++i) {
            book->add_order(i, false, 1, 25.00 + (i % 100) * 0.01);
        }

        std::atomic<bool> stop{false};
        auto start = std::chrono::steady_clock::now();

        std::thread consumer([&] { engine.run(stop); });
        std::thread producer([&] {
            for (uint64_t i = 0; i < N; ++i) {
                Tick t{0.0, 1, true, i};
                while (!rb->push(t)) { /* full: spin */ }
            }
            stop.store(true, std::memory_order_release);
        });

        producer.join();
        consumer.join();

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();

        check("two-thread: every tick processed", engine.total_processed() == N);
        check("two-thread: all resting asks consumed", book->best_ask() == 0.0);
        std::cout << "Elapsed: " << ms << " ms\n";
    }

    // ---------- Part 3: risk parameters from ParamStore ----------
    // Book used below: 10,000 asks @ 100.05 and 10,000 bids @ 99.95 -> spread 0.1%.
    auto seed_book = [](OrderBook& book) {
        for (uint64_t id = 0; id < 100; ++id) {
            book.add_order(id,        false, 100, 100.05);
            book.add_order(1000 + id, true,  100,  99.95);
        }
    };
    // Config with risk-level scaling switched off, so limits are exactly max_position_size.
    auto unscaled = [] {
        RiskConfig c;
        c.risk_level_position_scale = {1.0, 1.0, 1.0};
        return c;
    };

    // position limit
    {
        RiskConfig config = unscaled(); // fallback max_position_size = 100
        config.staleness_window_s = 0.2;
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        seed_book(*book);

        rb->push(Tick{0.0, 60, true, 1});
        rb->push(Tick{0.0, 60, true, 2}); // 60 + 60 = 120 > 100
        engine.process_pending();
        check("limit: fill within fallback limit", engine.net_position() == 60);
        check("limit: tick breaching fallback limit rejected", engine.rejected_position() == 1);
        check("limit: rejected tick never reaches the book", book->debug_ask_volume(100.05) == 10'000 - 60);

        RiskParams ai = config.fallback;
        ai.max_position_size = 500;
        store.publish(ai, ParamStore::now_ns());
        for (uint64_t i = 0; i < 4; ++i) rb->push(Tick{0.0, 60, true, 3 + i});
        engine.process_pending();
        check("limit: raised AI limit lets buys through", engine.net_position() == 300 && engine.rejected_position() == 1);

        rb->push(Tick{0.0, 500, false, 7}); // 300 - 500 = -200, within 500
        engine.process_pending();
        check("limit: applies to short side too", engine.net_position() == -200);
        rb->push(Tick{0.0, 400, false, 8}); // -600 breaches 500
        engine.process_pending();
        check("limit: short breach rejected", engine.net_position() == -200 && engine.rejected_position() == 2);

        rb->push(Tick{0.0, 500, true, 9}); // back to +300
        engine.process_pending();

        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // let AI params go stale
        // fallback limit 100, holding 300: each tick first unwinds 50
        rb->push(Tick{0.0, 10, true, 10});  // unwind -> 250, then buy 10 grows exposure: rejected
        rb->push(Tick{0.0, 50, false, 11}); // unwind -> 200, then sell 50 reduces exposure: allowed
        engine.process_pending();
        check("limit: stale -> fallback blocks growing exposure", engine.rejected_position() == 3);
        check("unwind: one step per tick while over the lowered limit", engine.unwind_orders() == 2);
        check("unwind: reducing tick still allowed alongside unwind", engine.net_position() == 150);

        rb->push(Tick{0.0, 10, true, 12}); // unwind -> 100 (exactly at limit), buy rejected
        rb->push(Tick{0.0, 10, true, 13}); // at limit: no unwind, buy rejected
        engine.process_pending();
        check("unwind: stops once back within the limit", engine.net_position() == 100 && engine.unwind_orders() == 3);
    }
    {
        // unwind respects price protection: don't dump into a collapsing bid
        RiskConfig config = unscaled();
        config.unwind_step_shares = 100;
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);

        RiskParams ai = config.fallback;
        ai.max_position_size = 1000;
        ai.max_spread_pct = 0.10;
        store.publish(ai, ParamStore::now_ns());
        book->add_order(1, false, 300, 100.00);
        book->add_order(2, true,  300, 99.99);
        rb->push(Tick{0.0, 300, true, 1});
        engine.process_pending(); // long 300

        book->cancel_order(2);
        book->add_order(3, true, 20, 99.99);  // only 20 near the best bid
        book->add_order(4, true, 500, 95.00); // the rest is 5% lower
        book->add_order(5, false, 100, 100.00);
        ai.max_position_size = 100;           // limit drops: 200 over
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 1, true, 2});
        engine.process_pending();
        check("unwind: price protection caps how much is dumped", engine.net_position() == 280 &&
                                                                 book->debug_bid_volume(95.00) == 500);
    }

    // stop-loss cooldown
    {
        RiskConfig config = unscaled();
        config.stop_loss_cooldown_s = 0.3;
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        RiskParams ai = config.fallback;
        ai.max_spread_pct = 0.10;
        store.publish(ai, ParamStore::now_ns());

        book->add_order(1, false, 50, 100.00);
        book->add_order(2, true, 50, 99.95);
        rb->push(Tick{0.0, 50, true, 1}); // long 50 @ 100
        engine.process_pending();
        book->cancel_order(2);
        book->add_order(3, true, 500, 98.00);    // -2%
        book->add_order(4, false, 500, 98.05);
        rb->push(Tick{0.0, 10, true, 2});        // triggers stop loss
        engine.process_pending();
        check("cooldown: stop loss fired", engine.stop_loss_triggers() == 1 && engine.net_position() == 0);

        store.publish(ai, ParamStore::now_ns()); // keep AI params fresh
        rb->push(Tick{0.0, 10, true, 3});
        rb->push(Tick{0.0, 10, false, 4});
        engine.process_pending();
        check("cooldown: buys and sells ignored during cooldown",
              engine.rejected_cooldown() == 2 && engine.net_position() == 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 10, true, 5});
        engine.process_pending();
        check("cooldown: trading resumes afterwards", engine.net_position() == 10 && engine.rejected_cooldown() == 2);
    }
    {
        RiskConfig config = unscaled();
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        book->add_order(1, false, 10, 50.00);
        book->add_order(2, true, 100, 49.99);

        rb->push(Tick{0.0, 50, true, 1}); // only 10 available
        engine.process_pending();
        check("limit: position tracks actual fills, not requested size", engine.net_position() == 10);
    }

    // risk level scales the limit
    {
        RiskConfig config; // scales LOW 0.5, MODERATE 0.75, HIGH 1.0
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        seed_book(*book);

        RiskParams ai = config.fallback;
        ai.max_position_size = 400;
        auto try_buy = [&](RiskLevel level, uint32_t shares) {
            ai.risk_level = level;
            store.publish(ai, ParamStore::now_ns());
            const size_t before = engine.rejected_position();
            rb->push(Tick{0.0, shares, true, 0});
            engine.process_pending();
            return engine.rejected_position() == before;
        };
        check("risk level: LOW allows 50% of max (200)", try_buy(RiskLevel::LOW, 200));
        check("risk level: LOW blocks beyond 50%", !try_buy(RiskLevel::LOW, 1));
        check("risk level: MODERATE allows 75% (300)", try_buy(RiskLevel::MODERATE, 100));
        check("risk level: MODERATE blocks beyond 75%", !try_buy(RiskLevel::MODERATE, 1));
        check("risk level: HIGH allows 100% (400)", try_buy(RiskLevel::HIGH, 100));
        check("risk level: HIGH blocks beyond 100%", !try_buy(RiskLevel::HIGH, 1));
        check("risk level: position ends at 400", engine.net_position() == 400);
    }

    // spread gate
    {
        RiskConfig config = unscaled(); // fallback max_spread_pct = 1%
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);

        book->add_order(1, false, 100, 100.00);
        rb->push(Tick{0.0, 10, true, 1});
        engine.process_pending();
        check("spread: rejected when bid side is empty", engine.rejected_spread() == 1 && engine.net_position() == 0);

        book->add_order(2, true, 100, 97.00); // spread 3/98.5 = 3.0% > 1%
        rb->push(Tick{0.0, 10, true, 2});
        engine.process_pending();
        check("spread: rejected when wider than max_spread_pct", engine.rejected_spread() == 2);

        RiskParams ai = config.fallback;
        ai.max_spread_pct = 0.05;
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 10, true, 3});
        engine.process_pending();
        check("spread: AI widening the tolerance lets it trade", engine.net_position() == 10);

        book->add_order(3, true, 100, 99.99); // spread now 0.01%
        ai.max_spread_pct = 0.001;
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 10, true, 4});
        engine.process_pending();
        check("spread: tight market passes a strict tolerance", engine.net_position() == 20 && engine.rejected_spread() == 2);
    }

    // price protection (urgency)
    {
        RiskConfig config = unscaled(); // base slippage 0.2%
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);

        book->add_order(1, true,  100, 99.99);
        book->add_order(2, false, 10, 100.00);
        book->add_order(3, false, 10, 100.15); // 0.15% past best
        book->add_order(4, false, 10, 100.35); // 0.35% past best
        book->add_order(5, false, 10, 101.00); // 1% past best

        RiskParams ai = config.fallback;
        ai.max_position_size = 1000;
        ai.urgency_multiplier = 1.0; // band 0.2% -> up to 100.20
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 40, true, 1});
        engine.process_pending();
        check("urgency 1.0: fills only within 0.2% of best", engine.net_position() == 20);
        check("urgency 1.0: levels beyond the band untouched",
              book->debug_ask_volume(100.35) == 10 && book->debug_ask_volume(101.00) == 10);
        check("urgency 1.0: average entry price tracked", std::fabs(engine.avg_entry_price() - 100.075) < 1e-9);

        // best ask is now 100.35; urgency 2.0 -> band 0.4% -> up to ~100.75
        ai.urgency_multiplier = 2.0;
        store.publish(ai, ParamStore::now_ns());
        rb->push(Tick{0.0, 40, true, 2});
        engine.process_pending();
        check("urgency 2.0: wider band reaches further", engine.net_position() == 30 &&
                                                       book->debug_ask_volume(101.00) == 10);
    }

    // stop loss
    {
        RiskConfig config = unscaled(); // fallback stop loss 1%
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);

        RiskParams ai = config.fallback;
        ai.max_spread_pct = 0.10;
        store.publish(ai, ParamStore::now_ns());

        book->add_order(1, false, 50, 100.00);
        book->add_order(2, true,  50,  99.95);
        rb->push(Tick{0.0, 50, true, 1}); // long 50 @ 100.00
        engine.process_pending();
        check("stop loss: position opened", engine.net_position() == 50 && engine.avg_entry_price() == 100.00);

        // market drops: bids now 99.50 (-0.5%), still inside 1%
        book->cancel_order(2);
        book->add_order(3, true, 100, 99.50);
        book->add_order(4, false, 100, 99.55);
        rb->push(Tick{0.0, 1, false, 2});
        engine.process_pending();
        check("stop loss: -0.5% does not trigger", engine.stop_loss_triggers() == 0 && engine.net_position() == 49);

        // market crashes: best bid 98.50 (-1.5%) -> stop out on the next tick
        book->cancel_order(3);
        book->add_order(5, true, 100, 98.50);
        rb->push(Tick{0.0, 10, true, 3});
        engine.process_pending();
        check("stop loss: -1.5% triggers and flattens", engine.stop_loss_triggers() == 1 && engine.net_position() == 0);
        check("stop loss: triggering tick is dropped", book->debug_ask_volume(99.55) == 100);
        check("stop loss: flatten sold into the bid", book->debug_bid_volume(98.50) == 51);
    }
    {
        // short side: entry 100.00, ask rises to 101.50 (-1.5% for a short)
        RiskConfig config = unscaled();
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        RiskParams ai = config.fallback;
        ai.max_spread_pct = 0.10;
        store.publish(ai, ParamStore::now_ns());

        book->add_order(1, true, 30, 100.00);
        book->add_order(2, false, 100, 100.05);
        rb->push(Tick{0.0, 30, false, 1});
        engine.process_pending();
        book->cancel_order(2);
        book->add_order(3, false, 100, 101.50);
        book->add_order(4, true, 100, 101.45);
        rb->push(Tick{0.0, 5, true, 2});
        engine.process_pending();
        check("stop loss: short position stopped out when price rises",
              engine.stop_loss_triggers() == 1 && engine.net_position() == 0);
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
