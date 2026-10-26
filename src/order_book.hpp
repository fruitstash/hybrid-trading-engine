#pragma once

#include<array>
#include<unordered_map>
#include<cstdint>
#include "order.hpp"
#include "limit_level.hpp"

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
        double best_bid() const;
        double best_ask() const;
        void   match_market_order(bool is_buy,uint32_t shares);
        // just for testing remove after all the tests are complete
        size_t debug_price_to_index(double price) const { return price_to_index(price); }

        uint32_t debug_bid_order_count(double price) const { return bids_[price_to_index(price)].order_count; }
        uint64_t debug_bid_volume(double price)      const { return bids_[price_to_index(price)].total_volume; }
        uint32_t debug_ask_order_count(double price) const { return asks_[price_to_index(price)].order_count; }
        uint64_t debug_ask_volume(double price)      const { return asks_[price_to_index(price)].total_volume; }
    private:

        size_t price_to_index(double price) const;

        std::array<LimitLevel, NUM_LEVELS> bids_;
        std::array<LimitLevel, NUM_LEVELS> asks_;
        std::unordered_map<uint64_t, Order*> order_lookup_;
};