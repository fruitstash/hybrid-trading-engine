#pragma once
#include<cstdint>

struct Order;

struct LimitLevel{
    uint64_t total_volume = 0;
    uint32_t order_count = 0;

    Order* head_order = nullptr;
    Order* tail_order = nullptr;
};