#pragma once

#include <cstdint>

struct Order {
    uint64_t order_id;
    bool     is_buy;
    uint32_t shares;
    double   price;
    uint64_t timestamp_ns;

    Order* prev = nullptr;
    Order* next = nullptr;
};