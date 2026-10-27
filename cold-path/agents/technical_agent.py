"""Technical analyst: trend, volatility and momentum from price candles.

Pure arithmetic - indicator formulas don't need an LLM, so this agent costs nothing.
"""

from __future__ import annotations

import math
from typing import Literal

from pydantic import BaseModel

from market_data import Candle

TRADING_MINUTES_PER_DAY = 390   # 9:30-16:00
TRADING_DAYS_PER_YEAR = 252


class TechnicalReport(BaseModel):
    price: float
    return_1h_pct: float
    return_1d_pct: float
    volatility_annual: float        # annualised stdev of log returns, e.g. 0.18 = 18%
    trend: Literal["up", "down", "flat"]
    trend_strength: float           # 0..1
    rsi14: float                    # 0..100, >70 overbought, <30 oversold
    atr_pct: float                  # average true range as a fraction of price


def _pct_change(closes: list[float], bars_back: int) -> float:
    if len(closes) < 2:
        return 0.0
    ref = closes[-1 - min(bars_back, len(closes) - 1)]
    return (closes[-1] / ref - 1) * 100


def _sma(values: list[float], n: int) -> float:
    window = values[-n:] if len(values) >= n else values
    return sum(window) / len(window)


def _rsi(closes: list[float], n: int = 14) -> float:
    deltas = [b - a for a, b in zip(closes, closes[1:])][-n:]
    if not deltas:
        return 50.0
    gains = sum(d for d in deltas if d > 0) / len(deltas)
    losses = sum(-d for d in deltas if d < 0) / len(deltas)
    if losses == 0:
        return 100.0 if gains > 0 else 50.0
    return 100 - 100 / (1 + gains / losses)


def _atr_pct(candles: list[Candle], n: int = 14) -> float:
    ranges = []
    for prev, cur in zip(candles, candles[1:]):
        ranges.append(max(cur.high - cur.low, abs(cur.high - prev.close), abs(cur.low - prev.close)))
    ranges = ranges[-n:]
    if not ranges or candles[-1].close <= 0:
        return 0.0
    return (sum(ranges) / len(ranges)) / candles[-1].close


def analyse(candles: list[Candle], interval_minutes: float = 15) -> TechnicalReport:
    if len(candles) < 2:
        raise ValueError("need at least 2 candles")
    closes = [c.close for c in candles]
    bars_per_hour = max(1, round(60 / interval_minutes))
    bars_per_day = max(1, round(TRADING_MINUTES_PER_DAY / interval_minutes))

    log_returns = [math.log(b / a) for a, b in zip(closes, closes[1:]) if a > 0 and b > 0]
    if len(log_returns) >= 2:
        mean = sum(log_returns) / len(log_returns)
        var = sum((r - mean) ** 2 for r in log_returns) / (len(log_returns) - 1)
        volatility = math.sqrt(var) * math.sqrt(bars_per_day * TRADING_DAYS_PER_YEAR)
    else:
        volatility = 0.0

    fast, slow = _sma(closes, 20), _sma(closes, 50)
    gap = (fast - slow) / slow if slow > 0 else 0.0
    trend = "up" if gap > 0.001 else "down" if gap < -0.001 else "flat"

    return TechnicalReport(
        price=closes[-1],
        return_1h_pct=round(_pct_change(closes, bars_per_hour), 4),
        return_1d_pct=round(_pct_change(closes, bars_per_day), 4),
        volatility_annual=round(volatility, 4),
        trend=trend,
        trend_strength=round(min(1.0, abs(gap) / 0.01), 3),
        rsi14=round(_rsi(closes), 2),
        atr_pct=round(_atr_pct(candles), 6),
    )
