# Hybrid Quantitative Trading Engine — Project Specification & Build Guide

**Architecture:** Hot Path (C++) / Cold Path (Python AI), bridged via ZeroMQ
**Environment:** Windows · MSVC · CMake · vcpkg
**Status:** Architecture finalized (flat-array order book) · Ready to implement Priority 1

---

## 1. Core Philosophy

Strict separation of concerns:

- **Hot Path (C++):** Live order execution. Microsecond latency. Zero heap allocation during trading. Never waits on the AI.
- **Cold Path (Python):** Macro reasoning, sentiment, and risk parameter generation via LLM agents. Too slow for execution, too valuable for regime/sentiment analysis to ignore.
- **Boundary:** The Cold Path publishes parameters asynchronously. If it hangs, crashes, or is slow, the Hot Path keeps running on the last known-good parameters or safe fallback defaults. The Hot Path never blocks on the AI, and never trusts its output without validation.

---

## 2. Architecture Diagram

```text
+-----------------------------------------------------------------------------------+
|                            THE COLD PATH (Python AI)                              |
|                                                                                     |
|  [News / Sentiment API]           [Technical / Market Data API]                    |
|           |                                 |                                      |
|           v                                 v                                      |
|     (Sentiment Agent)               (Technical Analyst)                            |
|           \                                 /                                      |
|            +-------> (Risk Manager Agent) <+                                       |
|                             |                                                       |
|                             v                                                       |
|                 [ZMQ PUB Socket: Port 5555]                                        |
+-----------------------------|-------------------------------------------------------+
                              |  IPC (JSON over TCP)
+-----------------------------|-------------------------------------------------------+
|                             v                                                       |
|                 [ZMQ SUB Socket: Port 5555]                                        |
|                             |                                                       |
|                  (Parameter Validation Layer) <-- untrusted input, clamped/rejected |
|                             |                                                       |
|                             v                                                       |
|         +---------------------------------------------+                           |
|         |         C++ EXECUTION ENGINE (Hot Path)      |                           |
|         |                                               |                           |
|         |  [Tick Ring Buffer] -> [Matching Engine]      | <--- [Live Market Feed]   |
|         |                             |                  |                           |
|         |                    [Limit Order Book]          |                           |
|         |              (flat array, indexed by tick)     |                           |
|         +---------------------------------------------+                           |
|                                                                                     |
|                            THE HOT PATH (C++)                                      |
+-----------------------------------------------------------------------------------+
```

**Fallback rule:** if no valid parameter update has arrived within a defined staleness window, the execution engine reverts to conservative built-in risk defaults rather than continuing to trade on stale AI parameters.

---

## 3. Repository Structure

```
/hybrid-trading-engine
│
├── CMakeLists.txt
├── vcpkg.json                      # manifest mode, pins cppzmq
│
├── /src
│   ├── main.cpp                    # entry point / test harness
│   ├── order.hpp                   # Order struct
│   ├── limit_level.hpp             # LimitLevel struct
│   ├── order_book.hpp              # OrderBook class declaration
│   ├── order_book.cpp              # OrderBook implementation
│   ├── ring_buffer.hpp             # tick ingestion (Priority 2)
│   ├── ipc_listener.hpp/.cpp       # ZMQ SUB + parsing (Priority 3)
│   └── param_validator.hpp/.cpp    # guardrails on AI-sourced params (Priority 3)
│
├── /tests
│   ├── order_book_tests.cpp        # insert/cancel/match correctness
│   ├── ring_buffer_tests.cpp
│   └── validator_tests.cpp
│
├── /cold-path                      # Python side, built later
│   ├── agent.py                    # ZMQ PUB, multi-agent orchestration
│   ├── requirements.txt
│   └── /agents
│       ├── sentiment_agent.py
│       ├── technical_agent.py
│       └── risk_manager_agent.py
│
└── /docs
    └── hybrid-trading-engine-spec.md   # this file
```

---

## 4. Core Data Structures

```cpp
// Order — 64-byte aligned for cache efficiency
struct Order {
    uint64_t order_id;
    bool     is_buy;
    uint32_t shares;
    double   price;
    uint64_t timestamp_ns;
    Order*   prev;
    Order*   next;
};

// LimitLevel — one price tier, lives at a fixed array index (no tree pointers)
struct LimitLevel {
    uint64_t total_volume  = 0;
    uint32_t order_count   = 0;
    Order*   head_order    = nullptr;
    Order*   tail_order    = nullptr;
};

// OrderBook — flat-array indexed by tick size
class OrderBook {
    static constexpr double TICK_SIZE  = 0.01;
    static constexpr size_t NUM_LEVELS = 200000; // covers instrument's price range

    std::array<LimitLevel, NUM_LEVELS> bids;
    std::array<LimitLevel, NUM_LEVELS> asks;
    std::unordered_map<uint64_t, Order*> order_lookup; // O(1) cancel

    size_t price_to_index(double price) const;

public:
    void   add_order(uint64_t id, bool is_buy, uint32_t shares, double price);
    void   cancel_order(uint64_t id);
    double best_bid() const;
    double best_ask() const;
    void   match_market_order(bool is_buy, uint32_t shares);
};
```

**Design notes:**
- Array index *is* the price (via `price_to_index`) — no stored price field in `LimitLevel`, no rebalancing.
- `order_lookup` gives O(1) cancellation regardless of price level.
- Best bid/ask found by scanning from the top of the array — sequential memory access, cache-friendly.

---

## 5. IPC Data Contract (Cold Path → Hot Path)

```json
{
  "timestamp": 1776518400.123,
  "strategy_id": "VOLATILITY_ADAPTIVE_V1",
  "parameters": {
    "max_spread_pct": 0.05,
    "risk_level": "MODERATE",
    "max_position_size": 500,
    "urgency_multiplier": 1.25,
    "stop_loss_threshold_pct": 0.015
  },
  "signature": "sha256_hash_validation"
}
```

The Parameter Validation Layer must, before anything touches live risk state:
- Reject negative or zero prices/sizes
- Clamp `max_spread_pct` and `urgency_multiplier` to sane bounds
- Verify `signature` before trusting payload contents
- Discard (not crash on) malformed JSON

---

## 6. Step-by-Step Build Process

### Priority 1 — C++ Limit Order Book Core
1. Scaffold the repo structure above; write `CMakeLists.txt` (C++20, no external deps yet).
2. Implement `Order`, `LimitLevel`, `OrderBook` in `/src`.
3. Implement `add_order`, `cancel_order`, `best_bid`/`best_ask`, `match_market_order`.
4. Write `order_book_tests.cpp` — verify O(1) cancel correctness and price-time priority.
5. Build a minimal `main.cpp` harness that feeds hardcoded orders and prints book state.

### Priority 2 — Lock-Free Ring Buffer
1. Implement a fixed-size circular buffer (`ring_buffer.hpp`) to decouple tick ingestion from matching.
2. Single-producer/single-consumer is sufficient initially — avoids full lock-free complexity while still removing mutex contention.
3. Wire the ring buffer as the input stage feeding `match_market_order`.

### Priority 3 — ZeroMQ IPC Pipeline
1. Add `cppzmq` via vcpkg manifest (`vcpkg.json`), link in CMake.
2. Implement `ipc_listener` — ZMQ SUB socket on `tcp://127.0.0.1:5555`, parses incoming JSON.
3. Implement `param_validator` — enforces the guardrails in Section 5.
4. Write a throwaway Python script (`cold-path/agent.py`, minimal version) that PUB-publishes test payloads, to validate the pipe end-to-end before real agents exist.

### Priority 4 — Multi-Agent Strategist Framework
1. Build out `sentiment_agent.py`, `technical_agent.py`, `risk_manager_agent.py` using LangChain/CrewAI.
2. Wire them into `agent.py`, which aggregates their outputs into the IPC contract and PUB-publishes on the same schedule/cadence.
3. Only integrate once Priorities 1–3 are tested and validated independently — the AI is only as useful as the guardrails protecting the engine from it.

---

## 7. Testing Philosophy

- **Hot Path correctness is tested in total isolation** — no network, no Python, before anything else touches it.
- **Cold Path is tested with a stub publisher first** — real agent logic is the last thing wired in, not the first thing built.
- Any bug in matching/price-time priority is silent and expensive — over-invest in unit tests here relative to the rest of the system.

---

## 8. Immediate Next Action

Scaffold `CMakeLists.txt` + `vcpkg.json`, then implement `order.hpp`, `limit_level.hpp`, and `order_book.hpp/.cpp` per Section 4. Hand this document to your coding agent (e.g. opencode) as project context before starting Priority 1.
