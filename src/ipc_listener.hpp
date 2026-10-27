#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include "param_store.hpp"
#include "param_validator.hpp"

// Cold-path receiver: ZMQ SUB socket -> ParamValidator -> ParamStore.
// Runs on its own thread so receiving and parsing never touch the matching thread.
class IpcListener {
    public:
        // Called for every message with a one-line human-readable outcome.
        using Logger = std::function<void(const std::string&)>;

        IpcListener(std::string endpoint, ParamValidator& validator, ParamStore& store, Logger log = {});

        // Blocks until `stop` is set. Checks `stop` at least every `poll_ms`.
        void run(const std::atomic<bool>& stop, int poll_ms = 200);

        uint64_t accepted() const { return accepted_.load(std::memory_order_relaxed); }
        uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

    private:
        void log(const std::string& msg) const { if (log_) log_(msg); }

        std::string endpoint_;
        ParamValidator& validator_;
        ParamStore& store_;
        Logger log_;
        std::atomic<uint64_t> accepted_{0};
        std::atomic<uint64_t> rejected_{0};
};
