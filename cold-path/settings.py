"""Load cold-path settings (config.toml), secrets (.env) and the C++ operator limits."""

from __future__ import annotations

import json
import os
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

COLD_PATH_DIR = Path(__file__).resolve().parent
REPO_DIR = COLD_PATH_DIR.parent
PROVIDERS = ("rules", "groq", "claude", "ollama")


@dataclass(frozen=True)
class Pricing:
    input: float   # USD per 1M input tokens
    output: float  # USD per 1M output tokens


@dataclass(frozen=True)
class Settings:
    symbol: str = "SPY"
    analysis_interval_minutes: float = 15
    heartbeat_seconds: float = 5
    max_decision_age_minutes: float = 60
    reanalyse_price_move_pct: float = 0.25

    llm_provider: str = "rules"
    claude_model: str = "claude-haiku-5-5"
    claude_max_tokens: int = 1024
    claude_effort: str = "low"
    groq_model: str = "openai/gpt-oss-20b"
    groq_max_tokens: int = 2048
    groq_reasoning_effort: str | None = "low"
    ollama_model: str = "qwen2.5:7b"
    ollama_base_url: str = "http://127.0.0.1:11434"

    max_llm_calls_per_day: int = 250
    max_usd_per_day: float = 0.25
    pricing: dict[str, Pricing] = field(default_factory=dict)

    data_source: str = "sample"
    candle_interval: str = "15min"
    candle_count: int = 100
    max_headlines: int = 15
    news_lookback_hours: float = 24


def load_settings(path: Path | None = None) -> Settings:
    path = path or COLD_PATH_DIR / "config.toml"
    raw = tomllib.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    llm = raw.get("llm", {})
    claude = llm.get("claude", {})
    groq = llm.get("groq", {})
    ollama = llm.get("ollama", {})
    budget = raw.get("budget", {})
    data = raw.get("data", {})
    d = Settings()

    s = Settings(
        symbol=raw.get("symbol", d.symbol),
        analysis_interval_minutes=raw.get("analysis_interval_minutes", d.analysis_interval_minutes),
        heartbeat_seconds=raw.get("heartbeat_seconds", d.heartbeat_seconds),
        max_decision_age_minutes=raw.get("max_decision_age_minutes", d.max_decision_age_minutes),
        reanalyse_price_move_pct=raw.get("reanalyse_price_move_pct", d.reanalyse_price_move_pct),
        llm_provider=llm.get("provider", d.llm_provider),
        claude_model=claude.get("model", d.claude_model),
        claude_max_tokens=claude.get("max_tokens", d.claude_max_tokens),
        claude_effort=claude.get("effort", d.claude_effort),
        groq_model=groq.get("model", d.groq_model),
        groq_max_tokens=groq.get("max_tokens", d.groq_max_tokens),
        groq_reasoning_effort=groq.get("reasoning_effort", d.groq_reasoning_effort) or None,
        ollama_model=ollama.get("model", d.ollama_model),
        ollama_base_url=ollama.get("base_url", d.ollama_base_url),
        max_llm_calls_per_day=budget.get("max_llm_calls_per_day", d.max_llm_calls_per_day),
        max_usd_per_day=budget.get("max_usd_per_day", d.max_usd_per_day),
        pricing={name: Pricing(**p) for name, p in budget.get("pricing", {}).items()},
        data_source=data.get("source", d.data_source),
        candle_interval=data.get("candle_interval", d.candle_interval),
        candle_count=data.get("candle_count", d.candle_count),
        max_headlines=data.get("max_headlines", d.max_headlines),
        news_lookback_hours=data.get("news_lookback_hours", d.news_lookback_hours),
    )
    if s.llm_provider not in PROVIDERS:
        raise ValueError(f"config: llm.provider must be one of {', '.join(PROVIDERS)}, got {s.llm_provider!r}")
    if s.data_source not in ("live", "sample"):
        raise ValueError(f"config: data.source must be live or sample, got {s.data_source!r}")
    if s.heartbeat_seconds <= 0 or s.analysis_interval_minutes <= 0:
        raise ValueError("config: heartbeat_seconds and analysis_interval_minutes must be > 0")
    return s


def load_env(path: Path | None = None) -> None:
    """Load KEY=VALUE lines from cold-path/.env into os.environ (existing vars win)."""
    path = path or COLD_PATH_DIR / ".env"
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        line = re.sub(r"\s+#.*$", "", line)  # inline comment; a '#' inside a value is kept
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip().strip('"').strip("'")
        if key and value and key not in os.environ:
            os.environ[key] = value


def require_env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise RuntimeError(f"{name} is not set (add it to cold-path/.env)")
    return value


# ---------- operator limits shared with the C++ engine ----------

@dataclass(frozen=True)
class Limits:
    """Ranges the C++ validator clamps AI parameters into (config/risk_limits.json)."""
    max_spread_pct: tuple[float, float] = (0.0005, 0.10)
    urgency_multiplier: tuple[float, float] = (0.5, 2.0)
    stop_loss_threshold_pct: tuple[float, float] = (0.001, 0.10)
    max_position_size: tuple[int, int] = (1, 1000)


_LINE_COMMENT = re.compile(r'("(?:\\.|[^"\\])*")|//[^\n]*')


def parse_jsonc(text: str) -> dict:
    """Parse JSON that may contain // line comments (string contents are preserved)."""
    return json.loads(_LINE_COMMENT.sub(lambda m: m.group(1) or "", text))


def load_limits(path: Path | None = None) -> Limits:
    path = path or REPO_DIR / "config" / "risk_limits.json"
    if not path.exists():
        return Limits()
    limits = parse_jsonc(path.read_text(encoding="utf-8")).get("limits", {})
    d = Limits()

    def pair(key: str, default: tuple):
        b = limits.get(key, {})
        return (type(default[0])(b.get("min", default[0])), type(default[1])(b.get("max", default[1])))

    return Limits(
        max_spread_pct=pair("max_spread_pct", d.max_spread_pct),
        urgency_multiplier=pair("urgency_multiplier", d.urgency_multiplier),
        stop_loss_threshold_pct=pair("stop_loss_threshold_pct", d.stop_loss_threshold_pct),
        max_position_size=pair("max_position_size", d.max_position_size),
    )
