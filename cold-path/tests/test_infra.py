"""Budget, settings, IPC signing, data clients and the LangChain wrapper."""

import hashlib
import hmac
import json
from datetime import date

import pytest
import zmq
from langchain_core.messages import AIMessage
from langchain_core.prompts import ChatPromptTemplate
from langchain_core.runnables import RunnableLambda
from pydantic import BaseModel

import ipc
import llm as llm_mod
from budget import UsageBudget
from market_data import DataError, LiveData, SampleData
from settings import PROVIDERS, Pricing, load_env, load_limits, load_settings, parse_jsonc


# ---------- budget ----------

def test_budget_caps_and_cost(tmp_path):
    b = UsageBudget(tmp_path / "u.json", 2, 1.0, {"m": Pricing(0.10, 0.50)}, today=lambda: date(2026, 1, 1))
    assert b.cost("m", 1_000_000, 1_000_000) == pytest.approx(0.60)
    assert b.cost("local-model", 10**9, 10**9) == 0.0
    assert b.allows_call()
    b.record("m", 2000, 400)
    b.record("m", 2000, 400)
    assert not b.allows_call()                       # call cap
    assert b._state["usd"] == pytest.approx(2 * (2000 * 0.10 + 400 * 0.50) / 1e6)


def test_budget_persists_and_rolls_over(tmp_path):
    day = [date(2026, 1, 1)]
    path = tmp_path / "u.json"
    b = UsageBudget(path, 5, 1.0, {}, today=lambda: day[0])
    b.record("m", 10, 10)
    again = UsageBudget(path, 5, 1.0, {}, today=lambda: day[0])
    assert again._state["calls"] == 1               # restart doesn't reset the day
    day[0] = date(2026, 1, 2)
    assert again.allows_call() and again._state["calls"] == 0


def test_budget_survives_corrupt_file(tmp_path):
    path = tmp_path / "u.json"
    path.write_text("{not json")
    assert UsageBudget(path, 5, 1.0, {}).allows_call()


# ---------- settings ----------

def test_repo_config_loads():
    s = load_settings()
    assert s.symbol == "SPY" and s.llm_provider in PROVIDERS and s.data_source in ("live", "sample")
    assert s.pricing["claude-haiku-5-5"].input == 0.10
    assert s.heartbeat_seconds < 30                  # must beat the engine's staleness window


def test_bad_provider_rejected(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text('[llm]\nprovider = "gpt"\n')
    with pytest.raises(ValueError):
        load_settings(p)


def test_limits_match_cpp_config():
    limits = load_limits()
    assert limits.max_position_size == (1, 1000) and limits.urgency_multiplier == (0.5, 2.0)


def test_parse_jsonc_keeps_slashes_in_strings():
    assert parse_jsonc('{"url": "http://x//y", // comment\n "n": 1}') == {"url": "http://x//y", "n": 1}


def test_load_env(tmp_path, monkeypatch):
    monkeypatch.delenv("HTE_TEST_A", raising=False)
    monkeypatch.setenv("HTE_TEST_B", "keep")
    p = tmp_path / ".env"
    p.write_text("# comment\nHTE_TEST_A = 'se#cret'   # note\nHTE_TEST_B=override\nEMPTY=\n")
    load_env(p)
    import os
    assert os.environ["HTE_TEST_A"] == "se#cret" and os.environ["HTE_TEST_B"] == "keep"
    monkeypatch.delenv("HTE_TEST_A")


# ---------- ipc ----------

def test_signature_is_hmac_of_exact_payload_bytes():
    payload = ipc.build_payload({"a": 1}, timestamp=1.5)
    assert payload == b'{"timestamp":1.5,"strategy_id":"VOLATILITY_ADAPTIVE_V1","parameters":{"a":1}}'
    assert ipc.sign(b"k", payload) == hmac.new(b"k", payload, hashlib.sha256).hexdigest().encode()


def test_publish_sends_two_frames():
    ctx = zmq.Context()
    pub, sub = ctx.socket(zmq.PUB), ctx.socket(zmq.SUB)
    pub.bind("inproc://t"); sub.connect("inproc://t"); sub.setsockopt(zmq.SUBSCRIBE, b"")
    sub.setsockopt(zmq.RCVTIMEO, 2000)
    import time
    time.sleep(0.05)
    ipc.publish_params(pub, b"k", {"x": 1})
    payload, sig = sub.recv_multipart()
    assert json.loads(payload)["parameters"] == {"x": 1} and sig == ipc.sign(b"k", payload)
    pub.close(); sub.close(); ctx.term()


# ---------- data ----------

class FakeResponse:
    def __init__(self, status, body):
        self.status_code, self._body = status, body

    def json(self):
        if isinstance(self._body, Exception):
            raise self._body
        return self._body


class FakeSession:
    def __init__(self, routes):
        self.routes, self.requests = routes, []

    def get(self, url, params=None, headers=None, timeout=None):
        self.requests.append((url, params, headers))
        for suffix, resp in self.routes.items():
            if url.endswith(suffix):
                return resp
        raise AssertionError(url)


def live(routes, **kw):
    return LiveData("SPY", "fh-key", "td-key", session=FakeSession(routes), **kw)


def test_live_parses_candles_quote_and_news():
    import time
    now = int(time.time())
    routes = {
        "/time_series": FakeResponse(200, {"status": "ok", "values": [
            {"datetime": "2026-10-27 15:15:00", "open": "601", "high": "602", "low": "600", "close": "601.5", "volume": "10"},
            {"datetime": "2026-10-27 15:00:00", "open": "600", "high": "601", "low": "599", "close": "601", "volume": "12"},
        ]}),
        "/quote": FakeResponse(200, {"c": 602.25}),
        "/company-news": FakeResponse(200, [{"id": 1, "datetime": now - 60, "headline": "SPY up", "source": "A"}]),
        "/news": FakeResponse(200, [
            {"id": 1, "datetime": now - 60, "headline": "SPY up", "source": "A"},           # duplicate
            {"id": 2, "datetime": now - 30, "headline": "Newest", "source": "B"},
            {"id": 3, "datetime": now - 10 * 86400, "headline": "Too old", "source": "C"},
            {"id": 4, "headline": "missing time"},
        ]),
    }
    data = live(routes)
    snap = data.snapshot()
    assert [c.close for c in snap.candles] == [601.0, 601.5]       # sorted oldest first
    assert snap.price == 602.25
    assert [h.title for h in snap.headlines] == ["Newest", "SPY up"]
    # Finnhub key goes in a header, not the URL
    fh = [r for r in data._http.requests if "finnhub" in r[0]]
    assert all(r[2] == {"X-Finnhub-Token": "fh-key"} and "token" not in (r[1] or {}) for r in fh)


@pytest.mark.parametrize("resp,message", [
    (FakeResponse(429, {}), "rate limited"),
    (FakeResponse(401, {}), "API key rejected"),
    (FakeResponse(500, {}), "HTTP 500"),
    (FakeResponse(200, ValueError()), "not JSON"),
    (FakeResponse(200, {"status": "error", "message": "bad symbol"}), "bad symbol"),
])
def test_live_candle_errors(resp, message):
    with pytest.raises(DataError, match=message):
        live({"/time_series": resp}).candles()


def test_quote_failure_falls_back_to_last_close():
    routes = {
        "/time_series": FakeResponse(200, {"values": [
            {"datetime": "2026-10-27 15:00:00", "open": "1", "high": "1", "low": "1", "close": "600", "volume": "1"},
            {"datetime": "2026-10-27 15:15:00", "open": "1", "high": "1", "low": "1", "close": "605", "volume": "1"},
        ]}),
        "/quote": FakeResponse(200, {"c": 0}),
        "/company-news": FakeResponse(200, []), "/news": FakeResponse(200, []),
    }
    assert live(routes).snapshot().price == 605.0


def test_sample_data_is_deterministic_and_rotates_news():
    a, b = SampleData(seed=1), SampleData(seed=1)
    sa, sb = a.snapshot(), b.snapshot()
    assert sa.price == sb.price and len(sa.candles) == 100
    ids = [frozenset(h.id for h in a.snapshot().headlines) for _ in range(6)]
    assert len(set(ids)) > 1 and len(set(ids)) < 6   # sometimes new news, sometimes not


# ---------- LangChain wrapper ----------

class Out(BaseModel):
    value: int


class FakeChat:
    """Stands in for ChatAnthropic/ChatOllama: with_structured_output returns a runnable."""

    def __init__(self, result=None, exc=None):
        self.result, self.exc, self.kwargs = result, exc, None

    def with_structured_output(self, schema, **kwargs):
        self.kwargs = kwargs

        def run(_):
            if self.exc:
                raise self.exc
            return self.result
        return RunnableLambda(run)


PROMPT = ChatPromptTemplate.from_messages([("human", "{q}")])


def raw(stop="end_turn", inp=10, out=5):
    return AIMessage(content="", usage_metadata={"input_tokens": inp, "output_tokens": out, "total_tokens": inp + out},
                     response_metadata={"stop_reason": stop})


def test_wrapper_returns_parsed_and_usage():
    chat = FakeChat({"raw": raw(inp=123, out=45), "parsed": Out(value=7), "parsing_error": None})
    parsed, usage = llm_mod.LangChainLLM(chat, "m", (TimeoutError,)).invoke(PROMPT, {"q": "hi"}, Out)
    assert parsed.value == 7 and (usage.input_tokens, usage.output_tokens) == (123, 45)
    assert chat.kwargs == {"method": "json_schema", "include_raw": True}   # native structured output


@pytest.mark.parametrize("result,exc,match", [
    ({"raw": raw("refusal"), "parsed": None, "parsing_error": None}, None, "refused"),
    ({"raw": raw("max_tokens"), "parsed": None, "parsing_error": None}, None, "truncated"),
    ({"raw": raw(), "parsed": None, "parsing_error": ValueError("bad")}, None, "did not match"),
    (None, TimeoutError("slow"), "TimeoutError"),
])
def test_wrapper_failures_raise_llm_error(result, exc, match):
    with pytest.raises(llm_mod.LLMError, match=match):
        llm_mod.LangChainLLM(FakeChat(result, exc), "m", (TimeoutError,)).invoke(PROMPT, {"q": "x"}, Out)


def test_claude_client_configured_for_low_cost(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test-not-real")
    client = llm_mod.make_claude("claude-haiku-5-5", 1024, "low")
    chat = client._chat
    assert chat.model == "claude-haiku-5-5" and chat.max_tokens == 1024
    assert chat.thinking == {"type": "disabled"} and chat.output_config == {"effort": "low"}
    assert chat.temperature is None                  # sampling params are rejected by current models


def test_openai_style_length_stop_is_truncation():
    msg = AIMessage(content="", usage_metadata={"input_tokens": 1, "output_tokens": 1, "total_tokens": 2},
                    response_metadata={"finish_reason": "length"})
    with pytest.raises(llm_mod.LLMError, match="truncated"):
        llm_mod.LangChainLLM(FakeChat({"raw": msg, "parsed": None, "parsing_error": None}), "m", ()).invoke(
            PROMPT, {"q": "x"}, Out)


def test_groq_client_configured(monkeypatch):
    monkeypatch.setenv("GROQ_API_KEY", "gsk-test-not-real")
    client = llm_mod.make_groq("openai/gpt-oss-20b", 2048, "low")
    assert client._chat.model_name == "openai/gpt-oss-20b" and client._chat.max_tokens == 2048
    assert client._chat.reasoning_effort == "low" and client._chat.reasoning_format == "hidden"
    assert client._structured_kwargs == {"method": "json_schema", "strict": True}
    other = llm_mod.make_groq("llama-3.3-70b-versatile", 1024, None)
    assert other._structured_kwargs == {"method": "function_calling"}
    assert other._chat.reasoning_effort is None


def test_structured_kwargs_passed_through():
    chat = FakeChat({"raw": raw(), "parsed": Out(value=1), "parsing_error": None})
    llm_mod.LangChainLLM(chat, "m", (), {"method": "function_calling"}).invoke(PROMPT, {"q": "x"}, Out)
    assert chat.kwargs == {"method": "function_calling", "include_raw": True}


def test_repo_config_has_groq_defaults():
    s = load_settings()
    assert s.groq_model and s.groq_max_tokens > 0


def test_thinking_mode_per_model():
    assert llm_mod._claude_thinking("claude-haiku-5-5") == {"type": "disabled"}
    assert llm_mod._claude_thinking("claude-sonnet-5-5") == {"type": "between_tools"}
    assert llm_mod._claude_thinking("claude-opus-5-5") is None
