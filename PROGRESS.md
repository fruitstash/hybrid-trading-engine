# Hybrid Trading Engine — Progress Tracker

Companion to [hybrid-trading-engine-spec.md](hybrid-trading-engine-spec.md). Updated periodically as work lands.

**Last updated:** 2026-10-27
**Current focus:** Priority 4 (multi-agent cold path).

---

## Summary

| Priority | Area | Status |
|---|---|---|
| 1 | C++ Limit Order Book core | ✅ Done — 65 tests |
| 2 | Lock-free SPSC ring buffer | ✅ Done — wired via `MatchingEngine` |
| 3 | ZeroMQ IPC + param validator | ✅ Done — all 5 AI params enforced by the engine; 74 validator + 12 store + 44 engine tests |
| 4 | Multi-agent Python cold path | ✅ Done — Groq AI + live SPY data verified end-to-end with the C++ engine; 57 Python tests |

---

## How to build, test and run

```powershell
# configure once (vcpkg installs cppzmq, nlohmann-json, openssl from vcpkg.json)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/hybrid_trading_engine/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

# full system (two terminals). Keys live in cold-path\.env (copy .env.example).
pip install -r cold-path\requirements.txt
python -m pytest cold-path\tests
$env:HTE_HMAC_SECRET = "<same value as in cold-path\.env>"
.\build\Debug\trading_engine_live.exe            # terminal 1: C++ engine
python cold-path\agent.py                         # terminal 2: AI agents (groq + live data by default)

# one-off analysis, nothing published:  python cold-path\agent.py --mode once
# overrides:  --provider rules|groq|claude|ollama   --data live|sample
# Priority 3 hostile-message demo:      python cold-path\agent.py --mode demo
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
- [x] `tests/order_book_tests.cpp` — 65 checks
- [x] Best bid/ask tracked on every add/cancel/fill → O(1) lookup; market orders start at the best level (was: scan up to 200k levels)
- [x] `match_market_order` returns a `Fill` (shares + notional) and takes an optional limit price
- [x] Prices that round to zero ticks are rejected

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
- [x] **All five AI parameters enforced** in `MatchingEngine`, checked in this order on every tick:
  0. cooldown — after a stop loss, ignore every tick for `stop_loss_cooldown_s`
  1. `stop_loss_threshold_pct` — tracks volume-weighted entry price; if the position is down more than this (marked at the exit side), flatten it, start the cooldown, drop the tick
  2. unwind — if over the limit (e.g. it was just lowered), trade up to `unwind_step_shares` back toward it, within price protection
  3. `max_spread_pct` — reject if either side is empty or (ask − bid) / mid is wider
  4. `max_position_size` × `risk_level` scale — reject if |position| could exceed it, unless the tick reduces exposure
  5. `urgency_multiplier` — price protection: fill no further than `base_slippage_pct` × urgency past the best price
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
| risk-level position scale | | LOW 0.5 · MODERATE 0.75 · HIGH 1.0 |
| base_slippage_pct | 0.002 (× urgency) | |
| stop_loss_cooldown_s | 60 s | ≥ 0 (0 = off) |
| unwind_step_shares | 50 per tick | ≥ 1 |
| staleness window | 30 s | |
| max message age / future skew | 5 s / 2 s | |

---

## Remaining

### Priority 4 — working in rules mode
Decisions (2026-10-27): LangChain; switchable AI — `rules` (free) / **`groq` (free hosted, chosen)** / `claude` (Haiku 5.5, ~$1/month) / `ollama` (local; user installing it themselves — download speed ~0.2 MB/s made it slow); SPY; 24/7 every 15 min; Finnhub (news + quote) + Twelve Data (candles).

- [x] `cold-path/config.toml` (provider, model, schedule, budget, data source) + `.env.example` for keys
- [x] `settings.py` (config, `.env` loader, reads C++ `risk_limits.json` so AI uses the same ranges)
- [x] `ipc.py` (signing/publishing moved out of `agent.py`)
- [x] `market_data.py` — Finnhub + Twelve Data clients; `sample` source for key-free testing
- [x] `llm.py` — one interface over ChatAnthropic / ChatOllama; native structured output (`json_schema`); thinking off + effort `low` for Claude; usage returned per call
- [x] `budget.py` — daily caps on calls and USD, persisted in `cold-path/.state/usage.json`
- [x] Agents: `technical_agent.py` (pure maths, no AI), `sentiment_agent.py` (AI or keyword rules), `risk_manager_agent.py` (AI or formulas; always clamped to limits)
- [x] `orchestrator.py` — reuse last AI decision when no new headlines and price moved < 0.25%; budget hit → rules; AI error → rules; data error → keep last decision until it expires (60 min)
- [x] `agent.py` — modes `live` / `once` / `demo` / `steady`; analysis on a worker thread so the 5 s heartbeat never stalls
- [x] `llm.py` Groq provider (`langchain-groq`): `openai/gpt-oss-20b` default with strict JSON-schema output, reasoning effort low; other Groq models use tool calling
- [x] Tests: `cold-path/tests/` — **57 passing** (agents, orchestrator, budget, settings, IPC, data clients, LangChain wrapper incl. Groq) — no network, no spend
- [x] **End-to-end verified 2026-10-27:** `agent.py --mode live` (rules, sample data) → signed ZMQ → C++ engine accepted and applied the decision (limit 521 × LOW 50% = 260)
- [x] Keys in `cold-path/.env` (git-ignored, verified): HMAC secret, Groq, Finnhub, Twelve Data
- [x] **Groq verified 2026-10-27:** `openai/gpt-oss-20b`, ~1,550 in / ~350 out tokens per cycle, ~2–4 s, $0
- [x] **Live data verified:** Twelve Data 100 × 15-min SPY candles; Finnhub quote ($765.22) + 15 headlines (Yahoo, Reuters, Seeking Alpha)
- [x] AI vs rules on the same real headlines: rules scored +1.00 (keyword artefact) and flagged a "risk event" on the word "war"; Groq scored +0.05 "mixed", MODERATE — AI clearly better
- [x] **Full system verified:** `agent.py` (groq + live) → engine accepted MODERATE / 500 → limit 375 applied
- [x] Defaults in `config.toml` now `provider = "groq"`, `source = "live"`
- [ ] Optional: user installs Ollama (`ollama pull qwen2.5:3b` recommended on slow connection) → test `--provider ollama`
- [ ] Commit + push Priority 4

### Next up (agreed 2026-10-27)
- [ ] **P&L tracking + trade log** in the engine: realised / unrealised P&L, per-trade CSV log, summary stats
- [ ] **Real market prices into the engine** (paper trading) — today it trades a synthetic book with synthetic ticks, so any P&L would be meaningless
- [ ] Multi-symbol support — deferred; SPY only for now

### Hot path follow-ups
- [ ] Remove or gate `debug_*` accessors

### Housekeeping
- [ ] `docs/` is empty (spec expects the spec there)
- [ ] Spec §3 file list predates `matching_engine.hpp`, `seqlock.hpp`, `param_store.hpp`, `risk_*`, `hmac.*`, `live_main.cpp`, `config/`

---

## Known issues / observations

1. Order `timestamp_ns` is never set (always 0).
2. `Order` is not 64-byte aligned despite the spec comment.
3. `new`/`delete` and `unordered_map` on the hot path conflict with "zero heap allocation".
4. Ticks carry a `price` field that `MatchingEngine` ignores (ticks are treated as market orders).
5. `RingBuffer` head/tail atomics share a cache line (false sharing).
6. Replay protection relies on publisher timestamps strictly increasing; restarting the C++ side resets it (a replay within the 5 s age window after restart would be accepted once).
7. HMAC secret is a shared env var; no rotation mechanism yet.
8. Stop loss flattens with no price limit (deliberate: getting out beats getting a good price).
9. Position check uses the requested tick size; a fill capped by price protection may leave headroom unused (conservative).
10. Unwind runs only when a tick arrives (no ticks → no unwinding), and stops if the exit side is empty or entirely outside the price-protection band.

---

## Change log

- **2026-10-26** — Created tracker from audit of repo vs. spec.
- **2026-10-26** — Priority 1 tests (45). Fixed two order-book leaks. Priority 2 wired via `MatchingEngine`.
- **2026-10-26** — Repo pushed to GitHub (fruitstash/hybrid-trading-engine).
- **2026-10-27** — Priority 3 complete: vcpkg manifest, HMAC-signed ZMQ pipeline, validator, seqlock param store, staleness fallback, configurable limits, position-limit risk check, Python stub publisher. 5/5 suites pass; live demo verified.
- **2026-10-27** — Engine now enforces all five AI parameters (stop loss, spread gate, risk-level-scaled position limit, urgency-scaled price protection). Order book tracks best bid/ask in O(1) and reports fills with notional. New configurable `base_slippage_pct` and `risk_level_position_scale`.
- **2026-10-27** — Priority 4 built: LangChain multi-agent cold path (technical / sentiment / risk manager), switchable rules / Groq / Claude / Ollama, daily AI budget, reuse-when-unchanged, Finnhub + Twelve Data + sample data. 57 Python tests; end-to-end with the C++ engine verified in rules mode.
- **2026-10-27** — Decisions: risk-level scales stay 50/75/100; stop loss now starts a 60 s cooldown; positions over a lowered limit are unwound 50 shares/tick within price protection. 44 engine + 74 validator tests; live demo shows unwind 600 → 300 during fallback.
