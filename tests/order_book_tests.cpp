#include <iostream>
#include <memory>
#include <string>
#include <cmath>
#include <stdexcept>
#include "order_book.hpp"

static int passed = 0, failed = 0;

void check(const std::string& name, bool condition) {
    if (condition) { std::cout << "[PASS] " << name << "\n"; ++passed; }
    else           { std::cout << "[FAIL] " << name << "\n"; ++failed; }
}

// best_bid/best_ask return index * TICK_SIZE, so compare with a tolerance
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

template <typename Fn>
bool throws(Fn fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}

// OrderBook is ~6.4 MB, so always heap-allocate it
std::unique_ptr<OrderBook> new_book() { return std::make_unique<OrderBook>(); }

int main() {
    // ---------- empty book ----------
    {
        auto book = new_book();
        check("empty book: best_bid is 0.0", book->best_bid() == 0.0);
        check("empty book: best_ask is 0.0", book->best_ask() == 0.0);
        book->match_market_order(true, 100);
        book->match_market_order(false, 100);
        check("empty book: market orders are a no-op", book->best_bid() == 0.0 && book->best_ask() == 0.0);
    }

    // ---------- price_to_index ----------
    {
        auto book = new_book();
        check("price_to_index: 0.01 -> 1", book->debug_price_to_index(0.01) == 1);
        check("price_to_index: 50.10 -> 5010 (float rounding)", book->debug_price_to_index(50.10) == 5010);
        check("price_to_index: 1999.99 -> 199999 (top level)", book->debug_price_to_index(1999.99) == 199999);
    }

    // ---------- add_order and best prices ----------
    {
        auto book = new_book();
        book->add_order(1, true,  100, 50.00);
        book->add_order(2, true,   50, 50.25);
        book->add_order(3, false,  80, 50.50);
        book->add_order(4, false, 120, 50.75);
        book->add_order(5, true,   30, 50.25);

        check("add: best_bid is highest bid", near(book->best_bid(), 50.25));
        check("add: best_ask is lowest ask", near(book->best_ask(), 50.50));
        check("add: level count aggregates", book->debug_bid_order_count(50.25) == 2);
        check("add: level volume aggregates", book->debug_bid_volume(50.25) == 80);
        check("add: bids and asks are separate sides", book->debug_ask_order_count(50.25) == 0);
    }

    // ---------- invalid input ----------
    {
        auto book = new_book();
        book->add_order(1, true, 100, 50.00);

        check("add: duplicate order_id throws", throws([&] { book->add_order(1, false, 10, 60.00); }));
        check("add: duplicate leaves book unchanged",
              book->debug_ask_order_count(60.00) == 0 && book->debug_bid_volume(50.00) == 100);
        check("add: zero price throws", throws([&] { book->add_order(2, true, 10, 0.0); }));
        check("add: negative price throws", throws([&] { book->add_order(3, true, 10, -5.0); }));
        check("add: price above range throws", throws([&] { book->add_order(4, true, 10, 2000.00); }));
        check("add: rejected ids are not registered", throws([&] { book->cancel_order(2); }));
        check("cancel: unknown order_id throws", throws([&] { book->cancel_order(999); }));
    }

    // ---------- price-time priority within a level ----------
    {
        auto book = new_book();
        book->add_order(1, false, 30, 10.00);
        book->add_order(2, false, 40, 10.00);
        book->add_order(3, false, 50, 10.00);

        book->match_market_order(true, 50); // fills all of #1 (30), then 20 of #2

        check("priority: oldest order fully filled and removed", throws([&] { book->cancel_order(1); }));
        check("priority: level volume after partial fill", book->debug_ask_volume(10.00) == 70);
        check("priority: level count after partial fill", book->debug_ask_order_count(10.00) == 2);

        book->cancel_order(2); // only its 20 unfilled shares should come off
        check("priority: cancel after partial fill removes remaining shares only",
              book->debug_ask_volume(10.00) == 50 && book->debug_ask_order_count(10.00) == 1);

        book->match_market_order(true, 50);
        check("priority: last order consumed, level empty",
              book->debug_ask_volume(10.00) == 0 && book->best_ask() == 0.0);
    }

    // ---------- cancel head / middle / tail ----------
    {
        auto book = new_book();
        book->add_order(1, true, 10, 20.00);
        book->add_order(2, true, 20, 20.00);
        book->add_order(3, true, 30, 20.00);
        book->add_order(4, true, 40, 20.00);

        book->cancel_order(2); // middle
        check("cancel middle: volume/count", book->debug_bid_volume(20.00) == 80 && book->debug_bid_order_count(20.00) == 3);
        book->cancel_order(1); // head
        check("cancel head: volume/count", book->debug_bid_volume(20.00) == 70 && book->debug_bid_order_count(20.00) == 2);
        book->cancel_order(4); // tail
        check("cancel tail: volume/count", book->debug_bid_volume(20.00) == 30 && book->debug_bid_order_count(20.00) == 1);

        // list must still be linked correctly: new order goes behind #3
        book->add_order(5, true, 5, 20.00);
        book->match_market_order(false, 30); // should consume exactly #3
        check("cancel: list relinked, FIFO preserved",
              throws([&] { book->cancel_order(3); }) && book->debug_bid_volume(20.00) == 5);

        book->cancel_order(5);
        check("cancel: last order empties the level", book->best_bid() == 0.0 && book->debug_bid_order_count(20.00) == 0);
    }

    // ---------- cancel moves best price ----------
    {
        auto book = new_book();
        book->add_order(1, false, 10, 30.00);
        book->add_order(2, false, 10, 31.00);
        book->cancel_order(1);
        check("cancel: best_ask moves to next level", near(book->best_ask(), 31.00));
    }

    // ---------- multi-level sweeps ----------
    {
        auto book = new_book();
        book->add_order(1, false, 10, 100.00);
        book->add_order(2, false, 10, 100.01);
        book->add_order(3, false, 10, 100.02);
        book->add_order(4, true,  10,  99.99);

        book->match_market_order(true, 25);
        check("buy sweep: lowest asks consumed first",
              book->debug_ask_volume(100.00) == 0 && book->debug_ask_volume(100.01) == 0);
        check("buy sweep: partial fill at next level", book->debug_ask_volume(100.02) == 5);
        check("buy sweep: best_ask updated", near(book->best_ask(), 100.02));
        check("buy sweep: bids untouched", book->debug_bid_volume(99.99) == 10);
    }
    {
        auto book = new_book();
        book->add_order(1, true,  10, 50.00);
        book->add_order(2, true,  10, 49.00);
        book->add_order(3, true,  10, 48.00);
        book->add_order(4, false, 10, 51.00);

        book->match_market_order(false, 15);
        check("sell sweep: highest bid consumed first", book->debug_bid_volume(50.00) == 0);
        check("sell sweep: partial fill at next level", book->debug_bid_volume(49.00) == 5);
        check("sell sweep: deeper level untouched", book->debug_bid_volume(48.00) == 10);
        check("sell sweep: best_bid updated", near(book->best_bid(), 49.00));
        check("sell sweep: asks untouched", book->debug_ask_volume(51.00) == 10);
    }

    // ---------- market order larger than book ----------
    {
        auto book = new_book();
        book->add_order(1, false, 10, 10.00);
        book->add_order(2, false, 10, 11.00);
        book->match_market_order(true, 1000);
        check("oversized order: ask side emptied", book->best_ask() == 0.0);
        check("oversized order: filled orders removed",
              throws([&] { book->cancel_order(1); }) && throws([&] { book->cancel_order(2); }));
    }

    // ---------- edge cases ----------
    {
        auto book = new_book();
        book->add_order(1, false, 10, 10.00);
        book->match_market_order(true, 0);
        check("zero-share market order is a no-op", book->debug_ask_volume(10.00) == 10);

        book->cancel_order(1);
        book->add_order(1, true, 5, 9.00);
        check("order_id reusable after cancel", book->debug_bid_volume(9.00) == 5);

        book->match_market_order(false, 5);
        book->add_order(1, false, 7, 12.00);
        check("order_id reusable after fill", book->debug_ask_volume(12.00) == 7);

        book->add_order(2, true, 1, 0.01);
        book->add_order(3, false, 1, 1999.99);
        check("lowest price level usable", near(book->best_bid(), 0.01));
        book->match_market_order(true, 7);
        check("highest price level usable", near(book->best_ask(), 1999.99));
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
