#include "order_book.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>


size_t OrderBook::price_to_index(double price) const {
    if (price <= 0.0) {
        throw std::invalid_argument("price_to_index: price must be positive");
    }
    size_t index = static_cast<size_t>(std::round(price / TICK_SIZE));
    if (index >= NUM_LEVELS) {
        throw std::out_of_range("price_to_index: price outside supported range");
    }
    return index;
}

OrderBook::~OrderBook() {
    for (auto& [id, order] : order_lookup_) {
        delete order;
    }
}

void OrderBook::add_order(uint64_t order_id, bool is_buy, uint32_t shares, double price){
    if(order_lookup_.count(order_id)){
        throw std::invalid_argument("add_order: duplicate order_id");
    }

    // validate price before allocating so a throw can't leak the order
    size_t idx = price_to_index(price);

    Order *order = new Order{
        order_id,
        is_buy,
        shares,
        price,
        0
    };

    LimitLevel& level = is_buy ? bids_[idx] : asks_[idx];

    order->prev = level.tail_order;
    order->next = nullptr;
    if (level.tail_order){
        level.tail_order->next = order;
    } else{
        level.head_order = order;
    }
    level.tail_order = order;

    level.total_volume += shares;
    level.order_count += 1;

    order_lookup_[order_id] = order;
}

void OrderBook::cancel_order(uint64_t order_id) {
    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) {
        throw std::invalid_argument("cancel_order: order_id not found");
    }

    Order* order = it->second;
    size_t idx = price_to_index(order->price);
    LimitLevel& level = order->is_buy ? bids_[idx] : asks_[idx];

    // Unlink from doubly-linked list
    if (order->prev) {
        order->prev->next = order->next;
    } else {
        level.head_order = order->next; // was head
    }

    if (order->next) {
        order->next->prev = order->prev;
    } else {
        level.tail_order = order->prev; // was tail
    }

    // Update level aggregates
    level.total_volume -= order->shares;
    level.order_count  -= 1;

    // Remove from lookup and free order memory
    order_lookup_.erase(it);
    delete order;
}

double OrderBook::best_bid() const {
    for (size_t i = NUM_LEVELS; i > 0; --i) {
        if (bids_[i - 1].order_count > 0) {
            return (i - 1) * TICK_SIZE;
        }
    }
    return 0.0;
}

double OrderBook::best_ask() const {
    for (size_t i = 0; i < NUM_LEVELS; ++i) {
        if (asks_[i].order_count > 0) {
            return i * TICK_SIZE;
        }
    }
    return 0.0;
}

void OrderBook::match_market_order(bool is_buy, uint32_t shares){

    auto walk_and_fill = [&](auto& levels, bool ascending) {
        uint32_t remaining = shares;

        auto consume_level = [&](LimitLevel& level){
            while(remaining>0 && level.head_order){
                Order* order = level.head_order;
                uint32_t fill = std::min(remaining, order->shares);

                order->shares -= fill;
                remaining     -= fill;
                level.total_volume -= fill;

                if(order->shares==0) {
                    level.head_order = order->next;
                    if(level.head_order) level.head_order->prev = nullptr;
                    else level.tail_order = nullptr;

                    order_lookup_.erase(order->order_id);
                    delete order;
                    level.order_count -= 1;
                }
            }
        };

        if(ascending) { 
            for(size_t i=0; i<NUM_LEVELS && remaining > 0; ++i){
                consume_level(levels[i]);
            }
        } else{
            for(size_t i = NUM_LEVELS; i-->0 && remaining>0;){
                consume_level(levels[i]);
            }
        }
    };

    if(is_buy){
        walk_and_fill(asks_, true);
    } else{
        walk_and_fill(bids_,false);
    }
};