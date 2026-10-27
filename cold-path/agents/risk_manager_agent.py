"""Risk manager: combines sentiment + technical reports into the five engine parameters.

AI mode asks the model for a decision with a short rationale; rules mode uses
fixed formulas. Either way the result is clamped to the operator limits from
config/risk_limits.json before it leaves this module (the C++ side clamps again).
"""

from __future__ import annotations

from typing import Literal

from langchain_core.prompts import ChatPromptTemplate
from pydantic import BaseModel, Field

from agents.sentiment_agent import SentimentReport
from agents.technical_agent import TechnicalReport
from llm import StructuredLLM, Usage
from settings import Limits


class RiskDecision(BaseModel):
    risk_level: Literal["LOW", "MODERATE", "HIGH"] = Field(
        description="LOW in dangerous or uncertain conditions, HIGH only when conditions are clearly favourable")
    max_position_size: int = Field(description="Maximum shares to hold, within the allowed range")
    max_spread_pct: float = Field(description="Widest bid/ask spread (fraction, e.g. 0.005 = 0.5%) at which to trade")
    urgency_multiplier: float = Field(description="How far past the best price orders may fill; 1.0 is normal")
    stop_loss_threshold_pct: float = Field(description="Loss (fraction, e.g. 0.01 = 1%) at which to exit a position")
    rationale: str = Field(description="One or two sentences explaining the decision")


SYSTEM_PROMPT = """You are the risk manager for an automated trading engine on {symbol}.
You receive a news-sentiment report and a technical report, and you set the
engine's risk parameters for the next {interval} minutes.

Allowed ranges (values outside them are clipped):
- max_position_size: {pos_min} to {pos_max} shares
- max_spread_pct: {spread_min} to {spread_max}
- urgency_multiplier: {urg_min} to {urg_max}
- stop_loss_threshold_pct: {stop_min} to {stop_max}

Guidelines:
- Capital preservation comes first. When signals conflict or confidence is low, be conservative.
- High volatility or a risk event: lower risk_level and position size, wider stop loss.
- Calm, trending, positively-reported markets can take more size and HIGH risk.
- Set the stop loss comfortably beyond normal noise (several times atr_pct) so it isn't hit by routine moves.
- Raise urgency only when sentiment and trend agree strongly."""

PROMPT = ChatPromptTemplate.from_messages([
    ("system", SYSTEM_PROMPT),
    ("human", "Sentiment report:\n{sentiment}\n\nTechnical report:\n{technical}"),
])


def _clamp(v: float, bounds: tuple) -> float:
    return max(bounds[0], min(bounds[1], v))


def clamp_to_limits(d: RiskDecision, limits: Limits) -> RiskDecision:
    return RiskDecision(
        risk_level=d.risk_level,
        max_position_size=int(_clamp(int(d.max_position_size), limits.max_position_size)),
        max_spread_pct=round(_clamp(d.max_spread_pct, limits.max_spread_pct), 6),
        urgency_multiplier=round(_clamp(d.urgency_multiplier, limits.urgency_multiplier), 4),
        stop_loss_threshold_pct=round(_clamp(d.stop_loss_threshold_pct, limits.stop_loss_threshold_pct), 6),
        rationale=d.rationale.strip()[:300],
    )


def to_ipc_params(d: RiskDecision) -> dict:
    """The `parameters` object of the IPC contract (spec §5)."""
    return {
        "max_spread_pct": d.max_spread_pct,
        "risk_level": d.risk_level,
        "max_position_size": d.max_position_size,
        "urgency_multiplier": d.urgency_multiplier,
        "stop_loss_threshold_pct": d.stop_loss_threshold_pct,
    }


def ai_decision(llm: StructuredLLM, symbol: str, interval_minutes: float, sentiment: SentimentReport,
                technical: TechnicalReport, limits: Limits) -> tuple[RiskDecision, Usage]:
    variables = {
        "symbol": symbol, "interval": int(interval_minutes),
        "pos_min": limits.max_position_size[0], "pos_max": limits.max_position_size[1],
        "spread_min": limits.max_spread_pct[0], "spread_max": limits.max_spread_pct[1],
        "urg_min": limits.urgency_multiplier[0], "urg_max": limits.urgency_multiplier[1],
        "stop_min": limits.stop_loss_threshold_pct[0], "stop_max": limits.stop_loss_threshold_pct[1],
        "sentiment": sentiment.model_dump_json(indent=1),
        "technical": technical.model_dump_json(indent=1),
    }
    decision, usage = llm.invoke(PROMPT, variables, RiskDecision)
    return clamp_to_limits(decision, limits), usage


def rules_decision(sentiment: SentimentReport, technical: TechnicalReport, limits: Limits) -> RiskDecision:
    conviction = sentiment.score * sentiment.confidence   # -1..1, damped by confidence
    vol = technical.volatility_annual

    if sentiment.risk_event or vol > 0.30 or conviction < -0.2:
        level = "LOW"
    elif conviction > 0.2 and vol < 0.18 and technical.trend == "up":
        level = "HIGH"
    else:
        level = "MODERATE"

    size_fraction = 0.6 * max(0.0, 1 - vol / 0.5) + 0.4 * max(0.0, conviction)
    size_fraction = max(0.1, min(1.0, size_fraction))

    aligned = (conviction > 0 and technical.trend == "up") or (conviction < 0 and technical.trend == "down")
    urgency = 1.0 + 0.5 * technical.trend_strength if aligned else 1.0 - 0.3 * technical.trend_strength

    decision = RiskDecision(
        risk_level=level,
        max_position_size=round(limits.max_position_size[1] * size_fraction),
        max_spread_pct=0.002 + vol * 0.01,
        urgency_multiplier=urgency,
        stop_loss_threshold_pct=max(0.005, 3 * technical.atr_pct),
        rationale=(f"rules: conviction {conviction:+.2f}, volatility {vol:.0%}, trend {technical.trend}"
                   f"{', risk event in news' if sentiment.risk_event else ''}"),
    )
    return clamp_to_limits(decision, limits)
