"""Daily cap on AI calls and spend, persisted so restarts don't reset it."""

from __future__ import annotations

import json
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Callable

from settings import Pricing


def _utc_today() -> date:
    return datetime.now(timezone.utc).date()


class UsageBudget:
    def __init__(self, path: Path, max_calls_per_day: int, max_usd_per_day: float,
                 pricing: dict[str, Pricing], today: Callable[[], date] = _utc_today):
        self.path = path
        self.max_calls = max_calls_per_day
        self.max_usd = max_usd_per_day
        self.pricing = pricing
        self._today = today
        self._state = self._load()

    def _fresh(self) -> dict:
        return {"date": self._today().isoformat(), "calls": 0, "input_tokens": 0, "output_tokens": 0, "usd": 0.0}

    def _load(self) -> dict:
        try:
            state = json.loads(self.path.read_text(encoding="utf-8"))
            if state.get("date") == self._today().isoformat():
                return state
        except (OSError, ValueError):
            pass
        return self._fresh()

    def _roll_day(self) -> None:
        if self._state["date"] != self._today().isoformat():
            self._state = self._fresh()

    def allows_call(self) -> bool:
        self._roll_day()
        return self._state["calls"] < self.max_calls and self._state["usd"] < self.max_usd

    def cost(self, model: str, input_tokens: int, output_tokens: int) -> float:
        p = self.pricing.get(model)
        if p is None:
            return 0.0  # local models are free
        return (input_tokens * p.input + output_tokens * p.output) / 1_000_000

    def record(self, model: str, input_tokens: int, output_tokens: int) -> float:
        self._roll_day()
        usd = self.cost(model, input_tokens, output_tokens)
        s = self._state
        s["calls"] += 1
        s["input_tokens"] += input_tokens
        s["output_tokens"] += output_tokens
        s["usd"] = round(s["usd"] + usd, 8)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(s, indent=2), encoding="utf-8")
        return usd

    def summary(self) -> str:
        self._roll_day()
        s = self._state
        return (f"today: {s['calls']}/{self.max_calls} AI calls, ${s['usd']:.4f}/${self.max_usd:.2f} "
                f"({s['input_tokens']} in / {s['output_tokens']} out tokens)")
