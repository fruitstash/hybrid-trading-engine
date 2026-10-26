# Hybrid Trading Engine — Progress Tracker

Companion to [hybrid-trading-engine-spec.md](hybrid-trading-engine-spec.md). Updated periodically as work lands.

**Last updated:** 2026-10-26
**Current focus:** Start Priority 3 (vcpkg manifest + ZeroMQ IPC).

---

## Summary

| Priority | Area | Status |
|---|---|---|
| 1 | C++ Limit Order Book core | ✅ Done — 45 tests passing |
| 2 | Lock-free SPSC ring buffer | ✅ Done — wired to matching via `MatchingEngine`, tests passing |
| 3 | ZeroMQ IPC + param validator | ⬜ Not started (empty stubs) |
| 4 | Multi-agent Python cold path | ⬜ Not started (empty stubs) |

**Build & test:** `cmake -S . -B build` → `cmake --build build --config Debug` → `ctest --test-dir build -C Debug --output-on-failure`

---

## Done

### Scaffolding
- [x] Directory layout from spec §3 (`src`, `tests`, `cold-path`, `cold-path/agents`, `docs`)
- [x] `CMakeLists.txt` — C++20, `order_book` static lib, 3 test executables registered with ctest
- [x] vcpkg checked out locally, zeromq source fetched

### Priority 1 — Order book
- [x] `order.hpp`, `limit_level.hpp`, `order_book.hpp` per spec §4
- [x] `price_to_index` with validation (price ≤ 0 and out-of-range throw)
- [x] `add_order` (FIFO per level, rejects duplicate IDs)
- [x] `cancel_order` (O(1) via lookup map, throws on unknown ID)
- [x] `best_bid` / `best_ask`
- [x] `match_market_order` (price order, FIFO within level)
- [x] `main.cpp` harness with hardcoded add / cancel / match checks
- [x] Fixed: `add_order` leaked the order if price validation threw
- [x] Fixed: `OrderBook` had no destructor (resting orders leaked); copy now deleted
- [x] `tests/order_book_tests.cpp` — 45 checks:
  - empty book, `price_to_index` rounding and range edges
  - best bid/ask, level aggregates, side separation
  - invalid input: duplicate ID, zero/negative/out-of-range price, unknown cancel
  - price-time priority with partial fills
  - cancel head / middle / tail with list relinking verified
  - multi-level buy and sell sweeps, oversized market order
  - zero-share order, ID reuse, lowest/highest price levels

### Priority 2 — Ring buffer
- [x] `ring_buffer.hpp` — SPSC with acquire/release atomics
- [x] `tests/ring_buffer_tests.cpp` — basics, wraparound, 5M-tick two-thread test
- [x] `src/matching_engine.hpp` — `MatchingEngine<Capacity>` drains ring buffer into `match_market_order` (`process_pending()`, `run(stop)`)
- [x] `tests/matching_engine_tests.cpp` — FIFO application, buy/sell sides, 50k-tick producer→engine two-thread test
- [x] `main.cpp` demos producer thread → ring buffer → engine thread

---

## Remaining

### Priority 1 — cleanup
- [ ] Remove or gate `debug_*` accessors (tests rely on them today)

### Priority 3
- [ ] `vcpkg.json` (pin cppzmq) and CMake linking
- [ ] `ipc_listener` — SUB on `tcp://127.0.0.1:5555`
- [ ] `param_validator` — reject non-positive, clamp, verify signature, discard bad JSON
- [ ] Staleness window + fallback defaults
- [ ] `tests/validator_tests.cpp`
- [ ] Stub `cold-path/agent.py` PUB publisher

### Priority 4
- [ ] `requirements.txt`, three agents, `agent.py` orchestration

### Housekeeping
- [ ] `docs/` is empty (spec expects the spec there)
- [ ] Stray `test.exe`; not a git repo yet
- [ ] Spec §3 file list doesn't mention `matching_engine.hpp` / `matching_engine_tests.cpp`

---

## Known issues / observations

1. Order `timestamp_ns` is never set (always 0).
2. `Order` is not 64-byte aligned despite the spec comment.
3. `new`/`delete` and `unordered_map` on the hot path conflict with "zero heap allocation".
4. `best_bid`/`best_ask` and `match_market_order` scan from the array edge every call (up to 200k levels). Noticeable in the engine test: each 1-share tick rescans ~2,500 empty levels.
5. `match_market_order` returns no fill info.
6. Ticks carry a `price` field that `MatchingEngine` ignores (ticks are treated as market orders).
7. `RingBuffer` head/tail atomics share a cache line (false sharing); fine for now, pad later if profiling shows it.

---

## Change log

- **2026-10-26** — Created tracker from audit of repo vs. spec.
- **2026-10-26** — Priority 1 tests written (45 passing). Fixed two order-book memory leaks. Priority 2 wired via `MatchingEngine`; CMake restructured with ctest. All 3 test suites pass.
