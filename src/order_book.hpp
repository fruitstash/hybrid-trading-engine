#pragma once

#include<array>
#include<unordered_map>
#include<cstdint>
#include "order.hpp"
#include "limit_level.hpp"

// Result of a market order: how much traded and at what total cost.
struct Fill {
    uint32_t shares   = 0;
    double   notional = 0.0; // sum of shares * level price
    double avg_price() const { return shares ? notional / shares : 0.0; }
};

class OrderBook {
    public:
        static constexpr double TICK_SIZE = 0.01;
        static constexpr size_t NUM_LEVELS = 200000;

        OrderBook() = default;
        ~OrderBook();
        // owns raw Order pointers, so copying would double-free
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;

        void   add_order(uint64_t order_id, bool is_buy, uint32_t shares, double price);
        void   cancel_order(uint64_t order_id);
        // O(1): best indices are tracked on every add / cancel / fill. 0.0 means that side is empty.
        double best_bid() const;
        double best_ask() const;
        // Fills up to `shares` against the opposite side, best price first. If `limit_price` > 0,
        // never trades at a worse price than it (buys: <= limit, sells: >= limit).
        // May fill less than `shares` if the book runs dry or the limit is reached.
        Fill   match_market_order(bool is_buy, uint32_t shares, double limit_price = 0.0);
        // just for testing remove after all the tests are complete
        size_t debug_price_to_index(double price) const { return price_to_index(price); }

        uint32_t debug_bid_order_count(double price) const { return bids_[price_to_index(price)].order_count; }
        uint64_t debug_bid_volume(double price)      const { return bids_[price_to_index(price)].total_volume; }
        uint32_t debug_ask_order_count(double price) const { return asks_[price_to_index(price)].order_count; }
        uint64_t debug_ask_volume(double price)      const { return asks_[price_to_index(price)].total_volume; }
    private:

        size_t price_to_index(double price) const;
        void   refresh_best_bid(); // walk down from best_bid_idx_ to the next non-empty level
        void   refresh_best_ask(); // walk up from best_ask_idx_ to the next non-empty level

        // sentinels: index 0 is never a valid price, NUM_LEVELS is past the end
        static constexpr size_t NO_BID = 0;
        static constexpr size_t NO_ASK = NUM_LEVELS;
        size_t best_bid_idx_ = NO_BID;
        size_t best_ask_idx_ = NO_ASK;

        std::array<LimitLevel, NUM_LEVELS> bids_;
        std::array<LimitLevel, NUM_LEVELS> asks_;
        std::unordered_map<uint64_t, Order*> order_lookup_;
};