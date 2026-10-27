"""Cold-path publisher (Priority 3 stub).

Publishes signed risk-parameter updates to the C++ hot path over ZeroMQ PUB.
Real agents (Priority 4) will replace the hardcoded parameters; the wire
format and signing in `publish_params` stay the same.

Wire format: two-frame message
    frame 0: JSON payload (IPC data contract, no signature field)
    frame 1: hex HMAC-SHA256 of frame 0's exact bytes, keyed by HTE_HMAC_SECRET

Usage:
    python cold-path/agent.py                  # scripted demo: good, bad, then silence
    python cold-path/agent.py --mode steady    # valid update every interval, forever
"""

import argparse
import hashlib
import hmac
import json
import os
import sys
import time

import zmq

ENDPOINT = "tcp://127.0.0.1:5555"
STRATEGY_ID = "VOLATILITY_ADAPTIVE_V1"


def sign(secret: bytes, payload: bytes) -> bytes:
    return hmac.new(secret, payload, hashlib.sha256).hexdigest().encode()


def build_payload(params: dict, timestamp: float | None = None) -> bytes:
    message = {
        "timestamp": time.time() if timestamp is None else timestamp,
        "strategy_id": STRATEGY_ID,
        "parameters": params,
    }
    return json.dumps(message, separators=(",", ":")).encode()


def publish_params(sock: zmq.Socket, secret: bytes, params: dict) -> list[bytes]:
    payload = build_payload(params)
    frames = [payload, sign(secret, payload)]
    sock.send_multipart(frames)
    return frames


def good_params(max_position_size: int = 500) -> dict:
    return {
        "max_spread_pct": 0.05,
        "risk_level": "MODERATE",
        "max_position_size": max_position_size,
        "urgency_multiplier": 1.25,
        "stop_loss_threshold_pct": 0.015,
    }


def log(msg: str) -> None:
    print(f"[agent {time.strftime('%H:%M:%S')}] {msg}", flush=True)


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
    last = send_good()  # kept for the replay test below
    for _ in range(2):
        send_good()

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
    parser.add_argument("--mode", choices=["demo", "steady"], default="demo")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between messages")
    parser.add_argument("--silence", type=float, default=35.0, help="demo: seconds of silence (> staleness window)")
    args = parser.parse_args()

    secret = os.environ.get("HTE_HMAC_SECRET", "").encode()
    if not secret:
        print("HTE_HMAC_SECRET is not set", file=sys.stderr)
        return 1

    ctx = zmq.Context()
    sock = ctx.socket(zmq.PUB)
    sock.setsockopt(zmq.LINGER, 0)
    sock.bind(args.endpoint)
    log(f"publishing on {args.endpoint}")
    time.sleep(1.0)  # PUB/SUB "slow joiner": give subscribers time to connect

    try:
        if args.mode == "steady":
            run_steady(sock, secret, args.interval)
        else:
            run_demo(sock, secret, args.interval, args.silence)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        ctx.term()
    return 0


if __name__ == "__main__":
    sys.exit(main())
