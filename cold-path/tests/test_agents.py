import pytest

from agents import risk_manager_agent, sentiment_agent, technical_agent
from agents.risk_manager_agent import RiskDecision
from agents.sentiment_agent import SentimentReport
from settings import Limits

LIMITS = Limits()


# ---------- technical ----------

def test_uptrend_detected(candles_factory):
    closes = [600 * (1.001 ** i) for i in range(80)]
    t = technical_agent.analyse(candles_factory(closes))
    assert t.trend == "up" and t.trend_strength > 0
    assert t.rsi14 == 100.0          # only gains
    assert t.return_1h_pct > 0 and t.return_1d_pct > t.return_1h_pct
    assert t.price == pytest.approx(closes[-1])


def test_downtrend_and_flat(candles_factory):
    down = technical_agent.analyse(candles_factory([600 * (0.999 ** i) for i in range(80)]))
    assert down.trend == "down" and down.rsi14 == 0.0  # only losses
    flat = technical_agent.analyse(candles_factory([600.0] * 80))
    assert flat.trend == "flat" and flat.volatility_annual == 0.0 and flat.rsi14 == 50.0


def test_volatility_scales_with_noise(candles_factory):
    calm = [600 + (0.1 if i % 2 else -0.1) for i in range(80)]
    wild = [600 + (3.0 if i % 2 else -3.0) for i in range(80)]
    assert (technical_agent.analyse(candles_factory(wild)).volatility_annual
            > 10 * technical_agent.analyse(candles_factory(calm)).volatility_annual)


def test_needs_two_candles(candles_factory):
    with pytest.raises(ValueError):
        technical_agent.analyse(candles_factory([600.0]))


# ---------- sentiment (rules) ----------

def test_rules_sentiment_positive_negative(headlines_factory):
    pos = sentiment_agent.rules_sentiment(headlines_factory("Stocks rally to record on strong earnings"))
    neg = sentiment_agent.rules_sentiment(headlines_factory("Markets tumble as recession fears grow"))
    assert pos.score > 0.5 and neg.score < -0.5
    assert 0 < pos.confidence <= 0.6


def test_rules_sentiment_flags_risk_events(headlines_factory):
    r = sentiment_agent.rules_sentiment(headlines_factory("Fed holds rates steady", "Calm day"))
    assert r.risk_event and "fed" in r.themes


def test_rules_sentiment_no_headlines():
    r = sentiment_agent.rules_sentiment([])
    assert r.score == 0 and r.confidence == 0 and not r.risk_event


def test_sanitize_clamps_model_output():
    raw = SentimentReport(score=3.0, confidence=-1, risk_event=False,
                          themes=["a", " ", "b", "c", "d", "e", "f"], summary="x" * 500)
    s = sentiment_agent.sanitize(raw)
    assert s.score == 1.0 and s.confidence == 0.0 and s.themes == ["a", "b", "c", "d", "e"]
    assert len(s.summary) == 200


def test_headline_formatting(headlines_factory):
    text = sentiment_agent.format_headlines(headlines_factory("One", "Two"))
    assert text.splitlines()[0].startswith("- [2026-10-27 15:00 UTC] One")
    assert "no headlines" in sentiment_agent.format_headlines([])


# ---------- risk manager ----------

def sentiment(score=0.0, confidence=0.5, risk_event=False):
    return SentimentReport(score=score, confidence=confidence, risk_event=risk_event, themes=[], summary="")


def technical(candles_factory, step=1.0005, noise=0.0):
    closes = [600 * (step ** i) + (noise if i % 2 else -noise) for i in range(80)]
    return technical_agent.analyse(candles_factory(closes))


def test_rules_risk_event_is_low(candles_factory):
    d = risk_manager_agent.rules_decision(sentiment(0.9, 1.0, risk_event=True), technical(candles_factory), LIMITS)
    assert d.risk_level == "LOW"


def test_rules_calm_positive_uptrend_is_high(candles_factory):
    d = risk_manager_agent.rules_decision(sentiment(0.8, 0.6), technical(candles_factory), LIMITS)
    assert d.risk_level == "HIGH" and d.urgency_multiplier > 1.0


def test_rules_volatile_market_is_low_and_small(candles_factory):
    calm = risk_manager_agent.rules_decision(sentiment(), technical(candles_factory, 1.0, 0.1), LIMITS)
    wild = risk_manager_agent.rules_decision(sentiment(), technical(candles_factory, 1.0, 8.0), LIMITS)
    assert wild.risk_level == "LOW"
    assert wild.max_position_size < calm.max_position_size
    assert wild.stop_loss_threshold_pct > calm.stop_loss_threshold_pct


@pytest.mark.parametrize("score,conf,noise", [(-1, 1, 0), (1, 1, 0), (0, 0, 50), (1, 1, 50)])
def test_rules_always_within_limits(candles_factory, score, conf, noise):
    d = risk_manager_agent.rules_decision(sentiment(score, conf), technical(candles_factory, 1.0, noise), LIMITS)
    assert LIMITS.max_position_size[0] <= d.max_position_size <= LIMITS.max_position_size[1]
    assert LIMITS.max_spread_pct[0] <= d.max_spread_pct <= LIMITS.max_spread_pct[1]
    assert LIMITS.urgency_multiplier[0] <= d.urgency_multiplier <= LIMITS.urgency_multiplier[1]
    assert LIMITS.stop_loss_threshold_pct[0] <= d.stop_loss_threshold_pct <= LIMITS.stop_loss_threshold_pct[1]


def test_clamp_to_limits_and_ipc_shape():
    wild = RiskDecision(risk_level="HIGH", max_position_size=10**9, max_spread_pct=-1,
                        urgency_multiplier=99, stop_loss_threshold_pct=0, rationale=" ok ")
    d = risk_manager_agent.clamp_to_limits(wild, LIMITS)
    assert (d.max_position_size, d.max_spread_pct, d.urgency_multiplier, d.stop_loss_threshold_pct) == (
        1000, 0.0005, 2.0, 0.001)
    params = risk_manager_agent.to_ipc_params(d)
    # exactly the field set the C++ ParamValidator requires
    assert set(params) == {"max_spread_pct", "risk_level", "max_position_size",
                           "urgency_multiplier", "stop_loss_threshold_pct"}
    assert isinstance(params["max_position_size"], int)
