"""One analysis cycle: data -> technical -> sentiment -> risk manager -> engine parameters.

Cost controls live here:
- the AI is only called while the daily budget allows; otherwise rules mode (free)
- an AI decision is reused when there are no new headlines and the price barely moved
- any AI failure falls back to rules for that step, never to "no decision"
"""

from __future__ import annotations

import time
from dataclasses import dataclass, replace
from typing import Callable

from agents import risk_manager_agent, sentiment_agent, technical_agent
from agents.risk_manager_agent import RiskDecision
from agents.sentiment_agent import SentimentReport
from agents.technical_agent import TechnicalReport
from budget import UsageBudget
from llm import LLMError, StructuredLLM, Usage
from market_data import DataError, LiveData, SampleData
from settings import Limits, Settings


@dataclass(frozen=True)
class Decision:
    params: dict                 # IPC `parameters` object, ready to publish
    risk: RiskDecision
    sentiment: SentimentReport
    technical: TechnicalReport
    mode: str                    # "ai", "rules", "mixed" (one step fell back), or "<mode>, reused"
    price: float
    headline_ids: frozenset[str]
    decided_at: float            # clock() time


class Orchestrator:
    def __init__(self, settings: Settings, data: LiveData | SampleData, llm: StructuredLLM | None,
                 budget: UsageBudget, limits: Limits, log: Callable[[str], None] = print,
                 clock: Callable[[], float] = time.monotonic):
        self.settings = settings
        self.data = data
        self.llm = llm
        self.budget = budget
        self.limits = limits
        self.log = log
        self.clock = clock
        self.decision: Decision | None = None

    # ---------- public ----------

    def analyse(self) -> Decision | None:
        """Run one cycle. On a data failure the previous decision is kept (and will expire)."""
        try:
            snap = self.data.snapshot()
            technical = technical_agent.analyse(snap.candles, _interval_minutes(self.settings.candle_interval))
        except (DataError, ValueError) as e:
            self.log(f"data unavailable, keeping previous decision: {e}")
            return self.decision

        headline_ids = frozenset(h.id for h in snap.headlines)
        if self._can_reuse(snap.price, headline_ids):
            prev = self.decision
            mode = prev.mode if prev.mode.endswith("reused") else f"{prev.mode}, reused"
            self.decision = replace(prev, technical=technical, decided_at=self.clock(), mode=mode)
            self.log(f"no new headlines and price moved < {self.settings.reanalyse_price_move_pct}% - "
                     f"reusing last decision (no AI call)")
            return self.decision

        sentiment, sentiment_mode = self._sentiment(snap.headlines)
        risk, risk_mode = self._risk(sentiment, technical)
        mode = sentiment_mode if sentiment_mode == risk_mode else "mixed"
        self.decision = Decision(
            params=risk_manager_agent.to_ipc_params(risk),
            risk=risk, sentiment=sentiment, technical=technical, mode=mode,
            price=snap.price, headline_ids=headline_ids, decided_at=self.clock(),
        )
        return self.decision

    def current_params(self) -> dict | None:
        """Parameters to publish now, or None if there is no decision fresh enough to trust."""
        d = self.decision
        if d is None or self.clock() - d.decided_at > self.settings.max_decision_age_minutes * 60:
            return None
        return d.params

    # ---------- internals ----------

    def _can_reuse(self, price: float, headline_ids: frozenset[str]) -> bool:
        d = self.decision
        if d is None or self.llm is None or not d.mode.startswith("ai"):
            return False  # only skip the AI when the last answer actually came from it
        moved_pct = abs(price - d.price) / d.price * 100 if d.price > 0 else 100.0
        return headline_ids <= d.headline_ids and moved_pct < self.settings.reanalyse_price_move_pct

    def _ai_available(self, step: str) -> bool:
        if self.llm is None:
            return False
        if not self.budget.allows_call():
            self.log(f"{step}: daily AI budget reached ({self.budget.summary()}) - using rules")
            return False
        return True

    def _charge(self, usage: Usage) -> None:
        usd = self.budget.record(self.llm.model_name, usage.input_tokens, usage.output_tokens)
        self.log(f"  {self.llm.model_name}: {usage.input_tokens} in / {usage.output_tokens} out tokens, "
                 f"${usd:.5f} - {self.budget.summary()}")

    def _sentiment(self, headlines) -> tuple[SentimentReport, str]:
        if self._ai_available("sentiment"):
            try:
                report, usage = sentiment_agent.ai_sentiment(self.llm, self.settings.symbol, headlines)
                self._charge(usage)
                return report, "ai"
            except LLMError as e:
                self.log(f"sentiment: AI failed, using rules: {e}")
        return sentiment_agent.rules_sentiment(headlines), "rules"

    def _risk(self, sentiment: SentimentReport, technical: TechnicalReport) -> tuple[RiskDecision, str]:
        if self._ai_available("risk manager"):
            try:
                decision, usage = risk_manager_agent.ai_decision(
                    self.llm, self.settings.symbol, self.settings.analysis_interval_minutes,
                    sentiment, technical, self.limits)
                self._charge(usage)
                return decision, "ai"
            except LLMError as e:
                self.log(f"risk manager: AI failed, using rules: {e}")
        return risk_manager_agent.rules_decision(sentiment, technical, self.limits), "rules"


def _interval_minutes(interval: str) -> float:
    """'15min' -> 15, '1h' -> 60, '1day' -> 390 (one trading day)."""
    units = {"min": 1, "h": 60, "day": 390}
    for suffix, factor in units.items():
        if interval.endswith(suffix):
            return float(interval[: -len(suffix)]) * factor
    raise ValueError(f"unsupported candle_interval {interval!r}")
