#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <string>
#include "ring_buffer.hpp"

static int passed = 0, failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) { std::cout << "[PASS] " << name << "\n"; ++passed; }
    else           { std::cout << "[FAIL] " << name << "\n"; ++failed; }
}

int main() {
    // ---------- Part 1: single-threaded basics ----------
    {
        auto rb = std::make_unique<RingBuffer<8>>(); // holds 7 (one slot stays empty)
        Tick t{};

        check("pop on empty buffer returns false", !rb->pop(t));

        bool all_pushed = true;
        for (uint64_t i = 0; i < 7; ++i) {
            Tick in{100.0 + i, 10, true, i};
            all_pushed = all_pushed && rb->push(in);
        }
        check("7 pushes succeed on capacity-8 buffer", all_pushed);

        Tick extra{999.0, 1, true, 999};
        check("push on full buffer returns false", !rb->push(extra));

        bool fifo = true;
        for (uint64_t i = 0; i < 7; ++i) {
            fifo = fifo && rb->pop(t) && t.timestamp_ns == i;
        }
        check("pops return items in FIFO order", fifo);
        check("buffer empty again after draining", !rb->pop(t));

        // wraparound: cycle far past the capacity, one in / one out
        bool wrap_ok = true;
        for (uint64_t i = 0; i < 100; ++i) {
            Tick in{50.0, 5, false, i};
            wrap_ok = wrap_ok && rb->push(in) && rb->pop(t) && t.timestamp_ns == i;
        }
        check("wraparound works over 100 cycles", wrap_ok);
    }

    // ---------- Part 2: two threads, one producer, one consumer ----------
    {
        constexpr uint64_t N = 5'000'000;
        auto rb = std::make_unique<RingBuffer<1024>>();

        bool in_order = true;
        bool timed_out = false;
        uint64_t received = 0;

        auto start = std::chrono::steady_clock::now();

        std::thread producer([&] {
            for (uint64_t i = 0; i < N; ++i) {
                Tick t{static_cast<double>(i), static_cast<uint32_t>(i % 1000), (i % 2) == 0, i};
                while (!rb->push(t)) { /* buffer full: spin until consumer catches up */ }
            }
        });

        std::thread consumer([&] {
            Tick t;
            uint64_t expected = 0;
            while (expected < N) {
                if (rb->pop(t)) {
                    if (t.timestamp_ns != expected ||
                        t.price != static_cast<double>(expected) ||
                        t.shares != static_cast<uint32_t>(expected % 1000) ||
                        t.is_buy != ((expected % 2) == 0)) {
                        in_order = false;
                    }
                    ++expected;
                } else if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
                    timed_out = true; // watchdog so a bug can't hang the test forever
                    break;
                }
            }
            received = expected;
        });

        producer.join();
        consumer.join();

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();

        check("two-thread: no timeout / deadlock", !timed_out);
        check("two-thread: all 5,000,000 ticks received", received == N);
        check("two-thread: order and contents intact", in_order);
        std::cout << "Elapsed: " << ms << " ms\n";
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}