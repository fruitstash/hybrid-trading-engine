#include "ipc_listener.hpp"

#include <chrono>
#include <string_view>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace {

double wall_clock_s() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

} // namespace

IpcListener::IpcListener(std::string endpoint, ParamValidator& validator, ParamStore& store, Logger log)
    : endpoint_(std::move(endpoint)), validator_(validator), store_(store), log_(std::move(log)) {}

void IpcListener::run(const std::atomic<bool>& stop, int poll_ms) {
    zmq::context_t ctx{1};
    zmq::socket_t sub{ctx, zmq::socket_type::sub};
    sub.set(zmq::sockopt::rcvtimeo, poll_ms);    // wake up periodically to check `stop`
    sub.set(zmq::sockopt::linger, 0);            // don't hang on shutdown
    sub.set(zmq::sockopt::rcvhwm, 100);          // bounded queue if we fall behind
    sub.set(zmq::sockopt::subscribe, "");
    sub.connect(endpoint_);
    log("listening on " + endpoint_);

    std::vector<zmq::message_t> frames;
    while (!stop.load(std::memory_order_acquire)) {
        frames.clear();
        auto received = zmq::recv_multipart(sub, std::back_inserter(frames));
        if (!received) continue; // timeout

        if (frames.size() != 2) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            log("REJECT: expected 2 frames (payload, signature), got " + std::to_string(frames.size()));
            continue;
        }

        std::string_view payload{static_cast<const char*>(frames[0].data()), frames[0].size()};
        std::string_view signature{static_cast<const char*>(frames[1].data()), frames[1].size()};

        ValidationResult result = validator_.validate(payload, signature, wall_clock_s());
        if (!result.ok) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            log("REJECT: " + result.error);
            continue;
        }

        store_.publish(result.params, ParamStore::now_ns());
        accepted_.fetch_add(1, std::memory_order_relaxed);
        log(std::string("ACCEPT") + (result.clamped ? " (clamped)" : "") +
            ": max_position_size=" + std::to_string(result.params.max_position_size) +
            " urgency=" + std::to_string(result.params.urgency_multiplier) +
            " risk=" + to_string(result.params.risk_level));
    }
}
