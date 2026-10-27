# Hybrid Trading Engine — Progress Tracker

Companion to [hybrid-trading-engine-spec.md](hybrid-trading-engine-spec.md). Updated periodically as work lands.

**Last updated:** 2026-10-27
**Current focus:** Priority 4 (multi-agent cold path), or hot-path cleanups from Known Issues first.

---

## Summary

| Priority | Area | Status |
|---|---|---|
| 1 | C++ Limit Order Book core | ✅ Done — 45 tests |
| 2 | Lock-free SPSC ring buffer | ✅ Done — wired via `MatchingEngine` |
| 3 | ZeroMQ IPC + param validator | ✅ Done — 64 validator + 12 store tests, end-to-end demo verified |
| 4 | Multi-agent Python cold path | ⬜ Not started (empty stubs) |

---

## How to build, test and run

```powershell
# configure once (vcpkg installs cppzmq, nlohmann-json, openssl from vcpkg.json)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/hybrid_trading_engine/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

# live end-to-end demo (two terminals, same secret in both)
$env:HTE_HMAC_SECRET = "<any long random string>"
.\build\Debug\trading_engine_live.exe            # terminal 1: C++ engine
python cold-path\agent.py                         # terminal 2: stub AI (pip install -r cold-path\requirements.txt)
```

---

## Done

### Scaffolding
- [x] Directory layout from spec §3
- [x] `CMakeLists.txt` — C++20; libs `order_book`, `risk`, `ipc`; 5 test suites under ctest
- [x] `vcpkg.json` manifest (cppzmq, nlohmann-json, openssl; baseline pinned)
- [x] Git repo on GitHub: https://github.com/fruitstash/hybrid-trading-engine (public)

### Priority 1 — Order book
- [x] `Order`, `LimitLevel`, `OrderBook` per spec §4; add / cancel / best bid-ask / market match
- [x] Fixed two memory leaks (bad-price `add_order`, missing destructor)
- [x] `match_market_order` now returns shares filled
- [x] `tests/order_book_tests.cpp` — 45 checks

### Priority 2 — Ring buffer
- [x] `ring_buffer.hpp` SPSC + tests (5M-tick two-thread test)
- [x] `matching_engine.hpp` — drains ring buffer into the book

### Priority 3 — IPC pipeline
- [x] **Wire format:** 2 ZMQ frames — JSON payload + hex HMAC-SHA256 of its exact bytes (spec §5 updated)
- [x] `risk_params.hpp` — `RiskParams`, `RiskLevel`
- [x] `risk_config.hpp/.cpp` — operator limits with compiled-in defaults, overridable via `config/risk_limits.json` (comments allowed; unknown keys / bad types / inconsistent limits stop startup)
- [x] `hmac.hpp/.cpp` — OpenSSL HMAC-SHA256, constant-time verify
- [x] `param_validator.hpp/.cpp` — signature checked **before** parsing; malformed JSON discarded; exact field set; type + sign checks; clamping; freshness window; replay protection (timestamps must strictly increase); all-or-nothing
- [x] `seqlock.hpp` + `param_store.hpp` — lock-free single-writer hand-off to the matching thread; staleness judged on local steady clock → fallback defaults
- [x] `ipc_listener.hpp/.cpp` — SUB thread, 200 ms poll for clean shutdown, bounded queue
- [x] **Risk check** in `MatchingEngine`: rejects ticks that would push |net position| past `max_position_size`, except ticks that reduce exposure; position tracks actual fills
- [x] `live_main.cpp` → `trading_engine_live` — end-to-end harness with synthetic 40-share buy each second
- [x] `cold-path/agent.py` stub — `publish_params()` reusable by Priority 4; demo mode sends good / hostile / clamped messages, then goes silent
- [x] Tests: `validator_tests.cpp` (64, incl. RFC 4231 HMAC vectors and config loading), `param_store_tests.cpp` (12, incl. 2M-write torn-read test), `matching_engine_tests.cpp` (+9 risk checks → 17)
- [x] **End-to-end verified 2026-10-27:** 6/6 hostile messages rejected with correct reasons; extreme values clamped (1,000,000 → 1000, urgency 50 → 2.0); fallback kicked in exactly at 30 s of silence; recovered on next valid message

#### Default limits (edit `config/risk_limits.json`)

| Setting | Fallback | Allowed range |
|---|---|---|
| max_spread_pct | 0.01 | 0.0005 – 0.10 |
| urgency_multiplier | 1.0 | 0.5 – 2.0 |
| stop_loss_threshold_pct | 0.01 | 0.001 – 0.10 |
| max_position_size | 100 | 1 – 1000 |
| risk_level | LOW | LOW / MODERATE / HIGH |
| staleness window | 30 s | |
| max message age / future skew | 5 s / 2 s | |

---

## Remaining

### Priority 4
- [ ] `requirements.txt` additions (LangChain/CrewAI), three agents, `agent.py` orchestration using `publish_params()`

### Hot path follow-ups
- [ ] Use the other AI params: only `max_position_size` is enforced today; `max_spread_pct`, `stop_loss_threshold_pct`, `urgency_multiplier`, `risk_level` are validated and delivered but unused
- [ ] Remove or gate `debug_*` accessors

### Housekeeping
- [ ] `docs/` is empty (spec expects the spec there)
- [ ] Spec §3 file list predates `matching_engine.hpp`, `seqlock.hpp`, `param_store.hpp`, `risk_*`, `hmac.*`, `live_main.cpp`, `config/`

---

## Known issues / observations

1. Order `timestamp_ns` is never set (always 0).
2. `Order` is not 64-byte aligned despite the spec comment.
3. `new`/`delete` and `unordered_map` on the hot path conflict with "zero heap allocation".
4. `best_bid`/`best_ask` and `match_market_order` scan from the array edge every call (up to 200k levels).
5. Ticks carry a `price` field that `MatchingEngine` ignores (ticks are treated as market orders).
6. `RingBuffer` head/tail atomics share a cache line (false sharing).
7. Replay protection relies on publisher timestamps strictly increasing; restarting the C++ side resets it (a replay within the 5 s age window after restart would be accepted once).
8. HMAC secret is a shared env var; no rotation mechanism yet.

---

## Change log

- **2026-10-26** — Created tracker from audit of repo vs. spec.
- **2026-10-26** — Priority 1 tests (45). Fixed two order-book leaks. Priority 2 wired via `MatchingEngine`.
- **2026-10-26** — Repo pushed to GitHub (fruitstash/hybrid-trading-engine).
- **2026-10-27** — Priority 3 complete: vcpkg manifest, HMAC-signed ZMQ pipeline, validator, seqlock param store, staleness fallback, configurable limits, position-limit risk check, Python stub publisher. 5/5 suites pass; live demo verified.
