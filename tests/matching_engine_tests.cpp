#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
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

    // ---------- Part 3: pre-trade position limit from ParamStore ----------
    {
        RiskConfig config; // fallback max_position_size = 100
        config.staleness_window_s = 0.2;
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);

        for (uint64_t id = 0; id < 100; ++id) {
            book->add_order(id,        false, 100, 101.00); // 10,000 shares of asks
            book->add_order(1000 + id, true,  100,  99.00); // 10,000 shares of bids
        }

        rb->push(Tick{0.0, 60, true, 1});
        rb->push(Tick{0.0, 60, true, 2}); // 60 + 60 = 120 > 100
        engine.process_pending();
        check("risk: fill within fallback limit", engine.net_position() == 60);
        check("risk: tick breaching fallback limit rejected", engine.total_rejected() == 1);
        check("risk: rejected tick never reaches the book", book->debug_ask_volume(101.00) == 10'000 - 60);

        RiskParams ai = config.fallback;
        ai.max_position_size = 500;
        store.publish(ai, ParamStore::now_ns());
        for (uint64_t i = 0; i < 4; ++i) rb->push(Tick{0.0, 60, true, 3 + i});
        engine.process_pending();
        check("risk: raised AI limit lets buys through", engine.net_position() == 300 && engine.total_rejected() == 1);

        rb->push(Tick{0.0, 500, false, 7}); // 300 - 500 = -200, within 500
        engine.process_pending();
        check("risk: limit applies to short side too", engine.net_position() == -200);
        rb->push(Tick{0.0, 400, false, 8}); // -600 breaches 500
        engine.process_pending();
        check("risk: short breach rejected", engine.net_position() == -200 && engine.total_rejected() == 2);

        rb->push(Tick{0.0, 500, true, 9}); // back to +300
        engine.process_pending();

        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // let AI params go stale
        rb->push(Tick{0.0, 10, true, 10});  // 310 > fallback 100 and grows exposure
        rb->push(Tick{0.0, 50, false, 11}); // 250: still over 100, but reduces exposure
        engine.process_pending();
        check("risk: stale -> fallback limit blocks growing exposure", engine.total_rejected() == 3);
        check("risk: reducing exposure allowed while over limit", engine.net_position() == 250);
    }
    {
        RiskConfig config;
        ParamStore store(config);
        auto book = std::make_unique<OrderBook>();
        auto rb = std::make_unique<RingBuffer<16>>();
        MatchingEngine<16> engine(*book, *rb, &store);
        book->add_order(1, false, 10, 50.00);

        rb->push(Tick{0.0, 50, true, 1}); // only 10 available
        engine.process_pending();
        check("risk: position tracks actual fills, not requested size", engine.net_position() == 10);
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
