#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Single-writer / multi-reader seqlock. Readers never block the writer and the
// writer never blocks readers; a reader that overlaps a write just retries.
// The writer's critical section is a handful of stores, so retries are rare and short.
//
// Data lives in relaxed atomic words rather than a plain T so concurrent
// read/write is well-defined C++ (Boehm, "Can Seqlocks Get Along With
// Programming Language Memory Models?", 2012).
template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock<T> requires a trivially copyable T");
    static constexpr size_t WORDS = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    using Words = std::array<uint64_t, WORDS>;

    public:
        explicit SeqLock(const T& initial) { store(initial); }

        // Must only ever be called from one thread.
        void store(const T& value) {
            Words w{};
            std::memcpy(w.data(), &value, sizeof(T));

            const uint64_t seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_relaxed); // odd: write in progress
            std::atomic_thread_fence(std::memory_order_release);
            for (size_t i = 0; i < WORDS; ++i) {
                data_[i].store(w[i], std::memory_order_relaxed);
            }
            seq_.store(seq + 2, std::memory_order_release); // even: write complete
        }

        T load() const {
            Words w;
            uint64_t before, after;
            do {
                before = seq_.load(std::memory_order_acquire);
                for (size_t i = 0; i < WORDS; ++i) {
                    w[i] = data_[i].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                after = seq_.load(std::memory_order_relaxed);
            } while ((before & 1) || before != after); // torn or mid-write: retry

            T out;
            std::memcpy(&out, w.data(), sizeof(T));
            return out;
        }

    private:
        alignas(64) std::atomic<uint64_t> seq_{0};
        std::array<std::atomic<uint64_t>, WORDS> data_{};
};
