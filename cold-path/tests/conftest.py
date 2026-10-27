import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

# Modules live directly in cold-path/ (run as `python cold-path/agent.py`), so put it on the path.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from market_data import Candle, Headline  # noqa: E402


def make_candles(closes: list[float], spread: float = 0.001) -> list[Candle]:
    start = datetime(2026, 10, 27, 13, 30, tzinfo=timezone.utc)
    out, prev = [], closes[0]
    for i, c in enumerate(closes):
        out.append(Candle(start + timedelta(minutes=15 * i), prev, max(prev, c) * (1 + spread),
                          min(prev, c) * (1 - spread), c, 1000))
        prev = c
    return out


def make_headlines(*titles: str) -> list[Headline]:
    now = datetime(2026, 10, 27, 15, 0, tzinfo=timezone.utc)
    return [Headline(f"h{i}", now - timedelta(minutes=i), "Test", t) for i, t in enumerate(titles)]


@pytest.fixture
def candles_factory():
    return make_candles


@pytest.fixture
def headlines_factory():
    return make_headlines
