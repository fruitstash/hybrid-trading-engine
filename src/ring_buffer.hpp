#pragma once 
#include<array>
#include<atomic>
#include<cstdint>

struct Tick {
    double price;
    uint32_t shares;
    bool is_buy;
    uint64_t timestamp_ns;  
};


template <size_t Capacity>
class RingBuffer {
    public:
        bool push(const Tick& tick) {
            size_t head = head_.load(std::memory_order_relaxed);
            size_t next_head = (head + 1) % Capacity;
            if(next_head == tail_.load(std::memory_order_acquire)) {
                return false;
            }
            buffer_[head] = tick;
            head_.store(next_head, std::memory_order_release);
            return true;
        }

        bool pop(Tick& out) {
            size_t tail = tail_.load(std::memory_order_relaxed);
            if (tail == head_.load(std::memory_order_acquire)) {
                return false; // buffer empty
            }
            out = buffer_[tail];
            tail_.store((tail + 1) % Capacity, std::memory_order_release);
            return true;
        }
    private:
        std::array<Tick, Capacity> buffer_;
        std::atomic<size_t> head_{0};
        std::atomic<size_t> tail_{0};
};
