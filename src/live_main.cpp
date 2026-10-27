// Live end-to-end harness for Priority 3:
//   Python publisher --ZMQ--> IpcListener --validate--> ParamStore --> MatchingEngine risk check
//
// A synthetic feed pushes one 40-share buy per second so you can watch the
// position limit react as AI parameters arrive, get rejected, or go stale.
//
// Usage: trading_engine_live [--config PATH] [--endpoint URL] [--seconds N]
// Requires the HMAC secret in the HTE_HMAC_SECRET environment variable.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "ipc_listener.hpp"
#include "matching_engine.hpp"
#include "param_store.hpp"
#include "param_validator.hpp"
#include "risk_config.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_release); }

std::mutex g_log_mutex;
void log_line(const std::string& tag, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[" << tag << "] " << msg << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/risk_limits.json";
    std::string endpoint    = "tcp://127.0.0.1:5555";
    int seconds = 0; // 0 = run until Ctrl+C

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)        config_path = argv[++i];
        else if (arg == "--endpoint" && i + 1 < argc) endpoint = argv[++i];
        else if (arg == "--seconds" && i + 1 < argc)  seconds = std::atoi(argv[++i]);
        else {
            std::cerr << "usage: trading_engine_live [--config PATH] [--endpoint URL] [--seconds N]\n";
            return 2;
        }
    }

    const char* secret = std::getenv("HTE_HMAC_SECRET");
    if (!secret || !*secret) {
        std::cerr << "HTE_HMAC_SECRET is not set; refusing to accept unauthenticated parameters.\n";
        return 1;
    }

    RiskConfig config;
    try {
        config = load_risk_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    auto book  = std::make_unique<OrderBook>();
    auto ticks = std::make_unique<RingBuffer<1024>>();
    ParamStore store(config);
    ParamValidator validator(config, secret);
    MatchingEngine<1024> engine(*book, *ticks, &store);
    IpcListener listener(endpoint, validator, store, [](const std::string& m) { log_line("ipc", m); });

    // deep two-sided liquidity (0.1% spread) so synthetic buys always have
    // something to hit and the spread gate has a market to measure
    for (uint64_t id = 0; id < 1000; ++id) {
        book->add_order(id,         /*is_buy=*/false, 100, 100.05);
        book->add_order(10'000 + id, /*is_buy=*/true,  100,  99.95);
    }

    std::signal(SIGINT, on_signal);
    std::atomic<bool> engine_stop{false};
    std::thread ipc_thread([&] { listener.run(g_stop); });
    std::thread engine_thread([&] { engine.run(engine_stop); });

    log_line("main", "config: " + config_path + ", fallback max_position_size=" +
                     std::to_string(config.fallback.max_position_size) +
                     ", staleness window=" + std::to_string(static_cast<int>(config.staleness_window_s)) + "s");

    auto next = std::chrono::steady_clock::now();
    for (int tick = 0; !g_stop.load(std::memory_order_acquire); ++tick) {
        if (seconds > 0 && tick >= seconds) break;

        ticks->push(Tick{0.0, 40, /*is_buy=*/true, static_cast<uint64_t>(tick)});

        next += std::chrono::seconds(1);
        std::this_thread::sleep_until(next);

        ActiveParams active = store.current(ParamStore::now_ns());
        std::ostringstream os;
        os << std::fixed << std::setprecision(1)
           << "source=" << (active.from_ai ? "AI      " : "FALLBACK")
           << " age=" << (active.age_s < 0 ? std::string("never") : std::to_string(static_cast<int>(active.age_s)) + "s")
           << " risk=" << to_string(active.params.risk_level)
           << " limit=" << static_cast<int64_t>(active.params.max_position_size *
                                                config.position_scale(active.params.risk_level))
           << " position=" << engine.net_position()
           << " rejected(pos/spread)=" << engine.rejected_position() << "/" << engine.rejected_spread()
           << " unwinds=" << engine.unwind_orders()
           << " stops=" << engine.stop_loss_triggers()
           << " | msgs ok=" << listener.accepted() << " bad=" << listener.rejected();
        log_line("engine", os.str());
    }

    g_stop.store(true, std::memory_order_release);
    engine_stop.store(true, std::memory_order_release);
    ipc_thread.join();
    engine_thread.join();
    log_line("main", "shut down cleanly");
    return 0;
}
