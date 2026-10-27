#include "order_book.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>


size_t OrderBook::price_to_index(double price) const {
    if (price <= 0.0) {
        throw std::invalid_argument("price_to_index: price must be positive");
    }
    size_t index = static_cast<size_t>(std::round(price / TICK_SIZE));
    if (index == 0) {
        // index 0 is reserved as the "no bids" sentinel
        throw std::invalid_argument("price_to_index: price rounds to zero ticks");
    }
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

    if (is_buy) {
        if (best_bid_idx_ == NO_BID || idx > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        if (idx < best_ask_idx_) best_ask_idx_ = idx;
    }

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

    // If that emptied the best level, walk to the next non-empty one
    if (level.order_count == 0) {
        if (order->is_buy && idx == best_bid_idx_)   refresh_best_bid();
        if (!order->is_buy && idx == best_ask_idx_)  refresh_best_ask();
    }

    // Remove from lookup and free order memory
    order_lookup_.erase(it);
    delete order;
}

void OrderBook::refresh_best_bid() {
    while (best_bid_idx_ != NO_BID && bids_[best_bid_idx_].order_count == 0) {
        --best_bid_idx_;
    }
}

void OrderBook::refresh_best_ask() {
    while (best_ask_idx_ != NO_ASK && asks_[best_ask_idx_].order_count == 0) {
        ++best_ask_idx_;
    }
}

double OrderBook::best_bid() const {
    return best_bid_idx_ == NO_BID ? 0.0 : best_bid_idx_ * TICK_SIZE;
}

double OrderBook::best_ask() const {
    return best_ask_idx_ == NO_ASK ? 0.0 : best_ask_idx_ * TICK_SIZE;
}

Fill OrderBook::match_market_order(bool is_buy, uint32_t shares, double limit_price){
    Fill fill;
    uint32_t remaining = shares;

    auto consume_level = [&](LimitLevel& level, size_t idx){
        const double level_price = idx * TICK_SIZE;
        while(remaining>0 && level.head_order){
            Order* order = level.head_order;
            uint32_t qty = std::min(remaining, order->shares);

            order->shares -= qty;
            remaining     -= qty;
            level.total_volume -= qty;
            fill.shares   += qty;
            fill.notional += qty * level_price;

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

    // Walk outward from the best price; stop at the book edge, the limit, or when filled.
    // A tiny epsilon keeps a limit that lands exactly on a level from excluding it.
    if(is_buy){
        const size_t last = limit_price > 0.0
            ? std::min(NUM_LEVELS - 1, static_cast<size_t>(std::floor(limit_price / TICK_SIZE + 1e-9)))
            : NUM_LEVELS - 1;
        for(size_t i = best_ask_idx_; i <= last && i < NUM_LEVELS && remaining > 0; ++i){
            consume_level(asks_[i], i);
        }
        refresh_best_ask();
    } else{
        const size_t last = limit_price > 0.0
            ? std::max<size_t>(1, static_cast<size_t>(std::ceil(limit_price / TICK_SIZE - 1e-9)))
            : 1;
        for(size_t i = best_bid_idx_; i >= last && i != NO_BID && remaining > 0; --i){
            consume_level(bids_[i], i);
        }
        refresh_best_bid();
    }
    return fill;
}
