"""Signed ZeroMQ publishing to the C++ hot path.

Wire format: two-frame message
    frame 0: JSON payload (IPC data contract, no signature field)
    frame 1: hex HMAC-SHA256 of frame 0's exact bytes, keyed by HTE_HMAC_SECRET
"""

from __future__ import annotations

import hashlib
import hmac
import json
import time

import zmq

ENDPOINT = "tcp://127.0.0.1:5555"
STRATEGY_ID = "VOLATILITY_ADAPTIVE_V1"


def sign(secret: bytes, payload: bytes) -> bytes:
    return hmac.new(secret, payload, hashlib.sha256).hexdigest().encode()


def build_payload(params: dict, timestamp: float | None = None, strategy_id: str = STRATEGY_ID) -> bytes:
    message = {
        "timestamp": time.time() if timestamp is None else timestamp,
        "strategy_id": strategy_id,
        "parameters": params,
    }
    return json.dumps(message, separators=(",", ":")).encode()


def publish_params(sock: zmq.Socket, secret: bytes, params: dict) -> list[bytes]:
    payload = build_payload(params)
    frames = [payload, sign(secret, payload)]
    sock.send_multipart(frames)
    return frames


def open_publisher(endpoint: str = ENDPOINT) -> tuple[zmq.Context, zmq.Socket]:
    ctx = zmq.Context()
    sock = ctx.socket(zmq.PUB)
    sock.setsockopt(zmq.LINGER, 0)
    sock.bind(endpoint)
    return ctx, sock
