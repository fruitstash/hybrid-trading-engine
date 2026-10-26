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

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
