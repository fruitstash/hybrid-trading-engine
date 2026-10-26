#include <iostream>
#include <memory>
#include <cassert>
#include <atomic>
#include <thread>
#include "order_book.hpp"
#include "matching_engine.hpp"

int main() {
    auto book = std::make_unique<OrderBook>();

    std::cout << "--- Testing OrderBook ---\n";

    // 1. Add orders
    book->add_order(1, /*is_buy=*/true,  100, 50.00);
    book->add_order(2, /*is_buy=*/true,   50, 50.25);
    book->add_order(3, /*is_buy=*/false,  80, 50.50);
    book->add_order(4, /*is_buy=*/false, 120, 50.75);

    std::cout << "Best bid (expect 50.25): " << book->best_bid() << "\n";
    std::cout << "Best ask (expect 50.50): " << book->best_ask() << "\n";

    // 2. Test cancel_order on best bid
    std::cout << "\n--- Testing cancel_order ---\n";
    book->cancel_order(2); // cancel 50.25
    std::cout << "Best bid after canceling order 2 (expect 50.00): " << book->best_bid() << "\n";

    // Cancel non-existent order (should throw)
    try {
        book->cancel_order(999);
        std::cout << "FAILED: expected exception for invalid order_id\n";
    } catch (const std::exception& e) {
        std::cout << "PASSED: caught expected exception: " << e.what() << "\n";
    }

    // 3. Test match_market_order
    std::cout << "\n--- Testing match_market_order ---\n";
    // Buy market order of 100 shares -> should consume all 80 @ 50.50 and 20 @ 50.75
    book->match_market_order(/*is_buy=*/true, 100);
    std::cout << "Best ask after buying 100 shares (expect 50.75): " << book->best_ask() << "\n";
    std::cout << "Remaining volume at 50.75 (expect 100): " << book->debug_ask_volume(50.75) << "\n";

    // 4. Tick pipeline: producer thread -> ring buffer -> matching engine
    std::cout << "\n--- Testing tick pipeline ---\n";
    auto ticks = std::make_unique<RingBuffer<1024>>();
    MatchingEngine<1024> engine(*book, *ticks);
    std::atomic<bool> stop{false};

    std::thread consumer([&] { engine.run(stop); });
    std::thread producer([&] {
        // 4 buys of 25 -> consumes the remaining 100 @ 50.75
        for (uint64_t i = 0; i < 4; ++i) {
            while (!ticks->push(Tick{0.0, 25, true, i})) {}
        }
        // 1 sell of 60 -> leaves 40 of the 100 bid @ 50.00
        while (!ticks->push(Tick{0.0, 60, false, 4})) {}
        stop.store(true, std::memory_order_release);
    });
    producer.join();
    consumer.join();

    std::cout << "Ticks processed (expect 5): " << engine.total_processed() << "\n";
    std::cout << "Best ask (expect 0, side empty): " << book->best_ask() << "\n";
    std::cout << "Remaining volume at 50.00 bid (expect 40): " << book->debug_bid_volume(50.00) << "\n";

    std::cout << "\nAll basic tests completed successfully!\n";
    return 0;
}
