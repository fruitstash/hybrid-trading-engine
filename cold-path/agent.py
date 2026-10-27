"""Cold-path entry point: multi-agent risk strategist publishing to the C++ hot path.

Modes:
    python cold-path/agent.py                 # live: analyse every N minutes, publish every few seconds
    python cold-path/agent.py --mode once     # one analysis, print it, exit (nothing published)
    python cold-path/agent.py --mode demo     # scripted good / hostile / silent messages (Priority 3 test)
    python cold-path/agent.py --mode steady   # fixed valid update every interval, forever

Settings: cold-path/config.toml. Secrets: cold-path/.env (see .env.example).
--provider / --data override config.toml for one run.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import sys
import threading
import time

import zmq

from budget import UsageBudget
from ipc import ENDPOINT, build_payload, open_publisher, publish_params, sign
from llm import make_llm
from market_data import make_data_source
from orchestrator import Decision, Orchestrator
from settings import COLD_PATH_DIR, PROVIDERS, Settings, load_env, load_limits, load_settings, require_env


def log(msg: str) -> None:
    print(f"[agent {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def describe(d: Decision) -> str:
    p, s, t = d.params, d.sentiment, d.technical
    return (f"decision ({d.mode}): risk={p['risk_level']} max_pos={p['max_position_size']} "
            f"spread<={p['max_spread_pct']:.4f} urgency={p['urgency_multiplier']:.2f} "
            f"stop={p['stop_loss_threshold_pct']:.4f}\n"
            f"    price={t.price:.2f} trend={t.trend} vol={t.volatility_annual:.1%} rsi={t.rsi14:.0f} | "
            f"sentiment={s.score:+.2f} (conf {s.confidence:.2f}{', RISK EVENT' if s.risk_event else ''})\n"
            f"    why: {d.risk.rationale}")


def build_orchestrator(settings: Settings) -> Orchestrator:
    if settings.llm_provider == "groq":
        require_env("GROQ_API_KEY")
    if settings.llm_provider == "claude":
        require_env("ANTHROPIC_API_KEY")
        if settings.claude_model not in settings.pricing:
            raise RuntimeError(f"no [budget.pricing] entry for {settings.claude_model}; "
                               "the spend cap can't be enforced without it")
    budget = UsageBudget(COLD_PATH_DIR / ".state" / "usage.json", settings.max_llm_calls_per_day,
                         settings.max_usd_per_day, settings.pricing)
    return Orchestrator(settings, make_data_source(settings, os.environ), make_llm(settings),
                        budget, load_limits(), log=log)


def run_once(settings: Settings) -> None:
    orch = build_orchestrator(settings)
    log(f"provider={settings.llm_provider} data={settings.data_source} symbol={settings.symbol}")
    decision = orch.analyse()
    if decision is None:
        log("no decision (data unavailable)")
        return
    log(describe(decision))
    log(orch.budget.summary())


def run_live(sock: zmq.Socket, secret: bytes, settings: Settings) -> None:
    orch = build_orchestrator(settings)
    log(f"provider={settings.llm_provider} data={settings.data_source} symbol={settings.symbol}, "
        f"analysing every {settings.analysis_interval_minutes:g} min, publishing every {settings.heartbeat_seconds:g}s")
    log(orch.budget.summary())

    # Analysis (network + AI, can take a while) runs on a worker thread so the
    # heartbeat keeps the engine's parameters fresh in the meantime.
    worker: threading.Thread | None = None
    next_analysis = 0.0
    publishing = False

    def analyse() -> None:
        before = orch.decision
        decision = orch.analyse()
        if decision is not None and decision is not before:
            log(describe(decision))

    while True:
        now = time.monotonic()
        if now >= next_analysis and (worker is None or not worker.is_alive()):
            worker = threading.Thread(target=analyse, daemon=True)
            worker.start()
            next_analysis = now + settings.analysis_interval_minutes * 60

        params = orch.current_params()
        if params is not None:
            publish_params(sock, secret, params)
            if not publishing:
                log("publishing to engine")
        elif publishing:
            log("no fresh decision - stopped publishing; engine will fall back to its safe defaults")
        publishing = params is not None
        time.sleep(settings.heartbeat_seconds)


# ---------- Priority 3 test modes ----------

def good_params(max_position_size: int = 500) -> dict:
    return {
        "max_spread_pct": 0.05,
        "risk_level": "MODERATE",
        "max_position_size": max_position_size,
        "urgency_multiplier": 1.25,
        "stop_loss_threshold_pct": 0.015,
    }


def run_steady(sock: zmq.Socket, secret: bytes, interval: float) -> None:
    while True:
        publish_params(sock, secret, good_params())
        log("sent valid update (max_position_size=500)")
        time.sleep(interval)


def run_demo(sock: zmq.Socket, secret: bytes, interval: float, silence: float) -> None:
    def send_raw(frames: list[bytes], label: str) -> None:
        sock.send_multipart(frames)
        log(f"sent {label}")
        time.sleep(interval)

    def send_good(size: int = 500, label: str | None = None) -> list[bytes]:
        frames = publish_params(sock, secret, good_params(size))
        log(f"sent {label or f'valid update (max_position_size={size})'}")
        time.sleep(interval)
        return frames

    log("phase 1: valid updates")
    for _ in range(2):
        send_good()
    last = send_good()  # most recent valid message, replayed below while still fresh

    log("phase 2: hostile / broken messages (all should be rejected)")
    bad_json = b'{"timestamp": 17765'
    send_raw([bad_json, sign(secret, bad_json)], "malformed JSON (validly signed)")

    negative = good_params()
    negative["max_position_size"] = -500
    payload = build_payload(negative)
    send_raw([payload, sign(secret, payload)], "negative max_position_size")

    payload = build_payload(good_params(900))
    send_raw([payload, sign(b"wrong-secret", payload)], "update signed with the wrong secret")

    send_raw(last, "replay of an earlier valid message")

    payload = build_payload(good_params(), timestamp=time.time() - 60)
    send_raw([payload, sign(secret, payload)], "60-second-old message")

    send_raw([b"just one frame"], "single-frame message")

    log("phase 3: extreme but well-formed values (accepted, clamped)")
    extreme = good_params(1_000_000)
    extreme["urgency_multiplier"] = 50.0
    payload = build_payload(extreme)
    send_raw([payload, sign(secret, payload)], "max_position_size=1,000,000 urgency=50")

    send_good(800)

    log(f"phase 4: going silent for {silence:.0f}s (engine should fall back to defaults)")
    time.sleep(silence)

    log("phase 5: back online")
    for _ in range(3):
        send_good()
    log("demo finished")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--endpoint", default=ENDPOINT)
    parser.add_argument("--mode", choices=["live", "once", "demo", "steady"], default="live")
    parser.add_argument("--provider", choices=list(PROVIDERS), help="override [llm] provider")
    parser.add_argument("--data", choices=["live", "sample"], help="override [data] source")
    parser.add_argument("--interval", type=float, default=1.0, help="demo/steady: seconds between messages")
    parser.add_argument("--silence", type=float, default=35.0, help="demo: seconds of silence (> staleness window)")
    args = parser.parse_args()

    load_env()
    settings = load_settings()
    overrides = {k: v for k, v in (("llm_provider", args.provider), ("data_source", args.data)) if v}
    settings = dataclasses.replace(settings, **overrides)

    try:
        if args.mode == "once":
            run_once(settings)
            return 0
        secret = require_env("HTE_HMAC_SECRET").encode()
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1

    ctx, sock = open_publisher(args.endpoint)
    log(f"publishing on {args.endpoint}")
    time.sleep(1.0)  # PUB/SUB "slow joiner": give subscribers time to connect

    try:
        if args.mode == "live":
            run_live(sock, secret, settings)
        elif args.mode == "steady":
            run_steady(sock, secret, args.interval)
        else:
            run_demo(sock, secret, args.interval, args.silence)
    except KeyboardInterrupt:
        pass
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    finally:
        sock.close()
        ctx.term()
    return 0


if __name__ == "__main__":
    sys.exit(main())
