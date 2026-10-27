"""Market data for the agents: price history, latest price, news headlines.

LiveData   - Finnhub (quote + news) and Twelve Data (candles), free tiers with API keys
SampleData - deterministic synthetic data, no network, for tests and dry runs
"""

from __future__ import annotations

import math
import random
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

import requests

from settings import Settings

FINNHUB_URL = "https://finnhub.io/api/v1"
TWELVEDATA_URL = "https://api.twelvedata.com"


class DataError(RuntimeError):
    """A data source failed or returned something unusable."""


@dataclass(frozen=True)
class Candle:
    time: datetime
    open: float
    high: float
    low: float
    close: float
    volume: float


@dataclass(frozen=True)
class Headline:
    id: str
    time: datetime
    source: str
    title: str
    summary: str = ""


@dataclass(frozen=True)
class MarketSnapshot:
    symbol: str
    price: float
    candles: list[Candle]   # oldest first
    headlines: list[Headline]  # newest first
    fetched_at: datetime


class LiveData:
    def __init__(self, symbol: str, finnhub_key: str, twelvedata_key: str, *,
                 candle_interval: str = "15min", candle_count: int = 100,
                 max_headlines: int = 15, news_lookback_hours: float = 24,
                 session: requests.Session | None = None, timeout_s: float = 15):
        self.symbol = symbol
        self.candle_interval = candle_interval
        self.candle_count = candle_count
        self.max_headlines = max_headlines
        self.news_lookback_hours = news_lookback_hours
        self.timeout_s = timeout_s
        self._finnhub_key = finnhub_key
        self._twelvedata_key = twelvedata_key
        self._http = session or requests.Session()

    def snapshot(self) -> MarketSnapshot:
        candles = self.candles()
        try:
            price = self.quote()
        except DataError:
            price = candles[-1].close  # quote is a nicety; candles are enough
        return MarketSnapshot(self.symbol, price, candles, self.headlines(), datetime.now(timezone.utc))

    # --- Twelve Data ---
    def candles(self) -> list[Candle]:
        body = self._get(f"{TWELVEDATA_URL}/time_series", params={
            "symbol": self.symbol, "interval": self.candle_interval,
            "outputsize": self.candle_count, "apikey": self._twelvedata_key, "timezone": "UTC",
        }, source="Twelve Data")
        if not isinstance(body, dict) or body.get("status") == "error":
            raise DataError(f"Twelve Data: {body.get('message', 'error') if isinstance(body, dict) else 'bad response'}")
        try:
            candles = [
                Candle(
                    time=datetime.fromisoformat(v["datetime"]).replace(tzinfo=timezone.utc),
                    open=float(v["open"]), high=float(v["high"]), low=float(v["low"]),
                    close=float(v["close"]), volume=float(v.get("volume") or 0),
                )
                for v in body["values"]
            ]
        except (KeyError, TypeError, ValueError) as e:
            raise DataError(f"Twelve Data: unexpected payload ({e})") from e
        if len(candles) < 2:
            raise DataError("Twelve Data: not enough candles")
        return sorted(candles, key=lambda c: c.time)

    # --- Finnhub ---
    def quote(self) -> float:
        body = self._get(f"{FINNHUB_URL}/quote", params={"symbol": self.symbol}, source="Finnhub",
                         headers={"X-Finnhub-Token": self._finnhub_key})
        price = body.get("c") if isinstance(body, dict) else None
        if not isinstance(price, (int, float)) or price <= 0:
            raise DataError("Finnhub: no current price")
        return float(price)

    def headlines(self) -> list[Headline]:
        headers = {"X-Finnhub-Token": self._finnhub_key}
        now = datetime.now(timezone.utc)
        since = now - timedelta(hours=self.news_lookback_hours)
        items: list = []
        # ETF-specific news is often thin, so also pull general market news.
        company = self._get(f"{FINNHUB_URL}/company-news", source="Finnhub", headers=headers, params={
            "symbol": self.symbol, "from": since.date().isoformat(), "to": now.date().isoformat()})
        general = self._get(f"{FINNHUB_URL}/news", source="Finnhub", headers=headers, params={"category": "general"})
        for batch in (company, general):
            if isinstance(batch, list):
                items.extend(batch)

        seen: set[str] = set()
        out: list[Headline] = []
        for item in items:
            try:
                ts = datetime.fromtimestamp(int(item["datetime"]), tz=timezone.utc)
                title = str(item["headline"]).strip()
            except (KeyError, TypeError, ValueError):
                continue
            key = str(item.get("id") or title)
            if not title or key in seen or ts < since:
                continue
            seen.add(key)
            out.append(Headline(key, ts, str(item.get("source", "")), title[:200], str(item.get("summary", ""))[:300]))
        out.sort(key=lambda h: h.time, reverse=True)
        return out[: self.max_headlines]

    def _get(self, url: str, *, source: str, params: dict | None = None, headers: dict | None = None):
        try:
            resp = self._http.get(url, params=params, headers=headers, timeout=self.timeout_s)
        except requests.RequestException as e:
            raise DataError(f"{source}: request failed ({type(e).__name__})") from e
        if resp.status_code == 429:
            raise DataError(f"{source}: rate limited")
        if resp.status_code in (401, 403):
            raise DataError(f"{source}: API key rejected (HTTP {resp.status_code})")
        if resp.status_code != 200:
            raise DataError(f"{source}: HTTP {resp.status_code}")
        try:
            return resp.json()
        except ValueError as e:
            raise DataError(f"{source}: response was not JSON") from e


_SAMPLE_HEADLINES = [
    ("Stocks rally as inflation cools more than expected", "Reuters"),
    ("Fed signals patience on rate cuts amid strong jobs data", "Bloomberg"),
    ("Tech earnings beat forecasts, lifting the S&P 500", "CNBC"),
    ("Oil prices jump after supply disruption fears", "Reuters"),
    ("Markets slip as bond yields climb to multi-month highs", "WSJ"),
    ("Consumer confidence falls to lowest level this year", "Bloomberg"),
    ("Investors rotate into defensive sectors amid uncertainty", "FT"),
    ("Strong retail sales data eases recession worries", "CNBC"),
]


class SampleData:
    """Synthetic random-walk prices and rotating headlines. Deterministic per seed."""

    def __init__(self, symbol: str = "SPY", seed: int = 7, start_price: float = 600.0, candle_count: int = 100):
        self.symbol = symbol
        self._rng = random.Random(seed)
        self._price = start_price
        self._step = 0
        self._candle_count = candle_count
        self._history: list[Candle] = []
        start = datetime.now(timezone.utc) - timedelta(minutes=15 * candle_count)
        for i in range(candle_count):
            self._history.append(self._next_candle(start + timedelta(minutes=15 * i)))

    def _next_candle(self, t: datetime) -> Candle:
        drift = 0.0002 * math.sin(self._step / 20)
        ret = self._rng.gauss(drift, 0.0015)
        o = self._price
        c = max(1.0, o * (1 + ret))
        hi = max(o, c) * (1 + abs(self._rng.gauss(0, 0.0005)))
        lo = min(o, c) * (1 - abs(self._rng.gauss(0, 0.0005)))
        self._price = c
        self._step += 1
        return Candle(t, o, hi, lo, c, self._rng.uniform(1e5, 5e5))

    def snapshot(self) -> MarketSnapshot:
        now = datetime.now(timezone.utc)
        self._history.append(self._next_candle(now))
        self._history = self._history[-self._candle_count:]
        # a new headline every third snapshot, so "no news" cycles happen too
        n = 3 + self._step // 3
        headlines = []
        for i in reversed(range(max(0, n - 5), n)):  # newest first
            title, src = _SAMPLE_HEADLINES[i % len(_SAMPLE_HEADLINES)]
            headlines.append(Headline(f"sample-{i}", now - timedelta(minutes=30 * (n - i)), src, title))
        return MarketSnapshot(self.symbol, self._price, list(self._history), headlines, now)


def make_data_source(settings: Settings, env: Mapping[str, str]) -> LiveData | SampleData:
    if settings.data_source == "sample":
        return SampleData(settings.symbol, candle_count=settings.candle_count)
    missing = [k for k in ("FINNHUB_API_KEY", "TWELVEDATA_API_KEY") if not env.get(k)]
    if missing:
        raise RuntimeError(f"data.source = 'live' needs {', '.join(missing)} in cold-path/.env")
    return LiveData(settings.symbol, env["FINNHUB_API_KEY"], env["TWELVEDATA_API_KEY"],
                    candle_interval=settings.candle_interval, candle_count=settings.candle_count,
                    max_headlines=settings.max_headlines, news_lookback_hours=settings.news_lookback_hours)
