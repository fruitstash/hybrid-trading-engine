from dataclasses import replace
from datetime import date

import pytest

from agents.risk_manager_agent import RiskDecision
from agents.sentiment_agent import SentimentReport
from budget import UsageBudget
from llm import LLMError, Usage
from market_data import DataError, MarketSnapshot
from orchestrator import Orchestrator
from settings import Limits, Pricing, Settings

PRICING = {"fake-model": Pricing(input=1.0, output=5.0)}  # $ per 1M tokens


class FakeLLM:
    """Returns canned answers per schema; can be told to fail."""
    model_name = "fake-model"

    def __init__(self, fail: bool = False):
        self.calls: list[str] = []
        self.fail = fail

    def invoke(self, prompt, variables, schema):
        self.calls.append(schema.__name__)
        if self.fail:
            raise LLMError("boom")
        if schema is SentimentReport:
            return SentimentReport(score=0.5, confidence=0.8, risk_event=False, themes=["x"], summary="ok"), Usage(1000, 100)
        return RiskDecision(risk_level="HIGH", max_position_size=5000, max_spread_pct=0.004,
                            urgency_multiplier=1.2, stop_loss_threshold_pct=0.02, rationale="ai"), Usage(800, 120)


class FakeData:
    def __init__(self, candles, headlines):
        self.candles, self.headlines, self.price, self.fail = candles, headlines, candles[-1].close, False

    def snapshot(self):
        if self.fail:
            raise DataError("offline")
        return MarketSnapshot("SPY", self.price, self.candles, self.headlines, None)


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture
def setup(tmp_path, candles_factory, headlines_factory):
    def make(llm=None, max_calls=100, max_usd=1.0, **settings_kw):
        settings = replace(Settings(), **settings_kw)
        data = FakeData(candles_factory([600 * (1.0005 ** i) for i in range(80)]),
                        headlines_factory("Stocks rally", "Fed meets"))
        budget = UsageBudget(tmp_path / "usage.json", max_calls, max_usd, PRICING, today=lambda: date(2026, 10, 27))
        logs: list[str] = []
        clock = Clock()
        orch = Orchestrator(settings, data, llm, budget, Limits(), log=logs.append, clock=clock)
        return orch, data, budget, logs, clock
    return make


def test_rules_mode_makes_no_ai_calls(setup):
    orch, _, budget, _, _ = setup(llm=None)
    d = orch.analyse()
    assert d.mode == "rules" and budget._state["calls"] == 0
    assert orch.current_params() == d.params


def test_ai_mode_uses_both_agents_and_charges_budget(setup):
    llm = FakeLLM()
    orch, _, budget, _, _ = setup(llm=llm)
    d = orch.analyse()
    assert d.mode == "ai" and llm.calls == ["SentimentReport", "RiskDecision"]
    assert budget._state["calls"] == 2
    assert budget._state["usd"] == pytest.approx((1800 * 1.0 + 220 * 5.0) / 1e6)
    assert d.params["max_position_size"] == 1000  # AI asked for 5000, clamped to the limit


def test_unchanged_market_reuses_ai_decision_without_calling(setup):
    llm = FakeLLM()
    orch, data, _, logs, clock = setup(llm=llm)
    first = orch.analyse()
    clock.t += 900
    second = orch.analyse()
    assert len(llm.calls) == 2                       # no new AI calls
    assert second.mode == "ai, reused" and second.params == first.params
    assert second.decided_at == clock.t              # refreshed, so it doesn't expire
    assert any("reusing last decision" in m for m in logs)


def test_new_headline_or_price_move_triggers_reanalysis(setup, headlines_factory):
    llm = FakeLLM()
    orch, data, _, _, _ = setup(llm=llm)
    orch.analyse()
    data.headlines = headlines_factory("Stocks rally", "Fed meets", "Brand new headline")  # adds id h2
    orch.analyse()
    assert len(llm.calls) == 4
    data.price *= 1.01  # 1% move > 0.25% threshold
    orch.analyse()
    assert len(llm.calls) == 6


def test_budget_exhausted_falls_back_to_rules(setup):
    llm = FakeLLM()
    orch, _, budget, logs, _ = setup(llm=llm, max_calls=1)
    d = orch.analyse()
    assert llm.calls == ["SentimentReport"]          # risk manager skipped the AI
    assert d.mode == "mixed" and budget._state["calls"] == 1
    assert any("daily AI budget reached" in m for m in logs)


def test_dollar_cap_enforced(setup):
    llm = FakeLLM()
    orch, _, _, _, _ = setup(llm=llm, max_usd=0.000001)
    orch.analyse()
    assert llm.calls == ["SentimentReport"]           # first call pushed spend over the cap


def test_ai_failure_falls_back_to_rules(setup):
    orch, _, budget, logs, _ = setup(llm=FakeLLM(fail=True))
    d = orch.analyse()
    assert d is not None and d.mode == "rules"
    assert budget._state["calls"] == 0
    assert sum("AI failed" in m for m in logs) == 2


def test_rules_fallback_decision_is_not_reused(setup):
    llm = FakeLLM(fail=True)
    orch, _, _, _, _ = setup(llm=llm)
    orch.analyse()
    llm.fail = False
    d = orch.analyse()                                 # market unchanged, but last answer wasn't AI
    assert d.mode == "ai"


def test_data_failure_keeps_previous_decision(setup):
    orch, data, _, logs, _ = setup(llm=None)
    first = orch.analyse()
    data.fail = True
    assert orch.analyse() is first
    assert any("data unavailable" in m for m in logs)


def test_decision_expires(setup):
    orch, data, _, _, clock = setup(llm=None, max_decision_age_minutes=60)
    orch.analyse()
    data.fail = True
    clock.t += 59 * 60
    orch.analyse()
    assert orch.current_params() is not None
    clock.t += 2 * 60
    assert orch.current_params() is None             # engine will fall back to its defaults


def test_no_decision_before_first_analysis(setup):
    orch, _, _, _, _ = setup()
    assert orch.current_params() is None
