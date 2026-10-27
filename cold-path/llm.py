"""Switchable chat model behind one interface: Claude (Anthropic API), Groq (hosted, free tier)
or a local Ollama model.

Every call returns a schema-validated Pydantic object plus token usage, so the
budget can be charged. Any failure raises LLMError and callers fall back to rules.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, TypeVar

from langchain_core.prompts import ChatPromptTemplate
from pydantic import BaseModel

T = TypeVar("T", bound=BaseModel)


class LLMError(RuntimeError):
    """The model call failed, was refused, or returned unparseable output."""


@dataclass(frozen=True)
class Usage:
    input_tokens: int
    output_tokens: int


class StructuredLLM(Protocol):
    model_name: str

    def invoke(self, prompt: ChatPromptTemplate, variables: dict, schema: type[T]) -> tuple[T, Usage]: ...


class LangChainLLM:
    """Runs `prompt | chat_model.with_structured_output(schema)` and reports usage."""

    def __init__(self, chat_model, model_name: str, transient_errors: tuple[type[BaseException], ...],
                 structured_kwargs: dict | None = None):
        self._chat = chat_model
        self.model_name = model_name
        self._transient = transient_errors
        self._structured_kwargs = structured_kwargs or {"method": "json_schema"}

    def invoke(self, prompt: ChatPromptTemplate, variables: dict, schema: type[T]) -> tuple[T, Usage]:
        chain = prompt | self._chat.with_structured_output(schema, include_raw=True, **self._structured_kwargs)
        try:
            result = chain.invoke(variables)
        except self._transient as e:
            raise LLMError(f"{self.model_name}: {type(e).__name__}: {e}") from e

        raw = result["raw"]
        meta = getattr(raw, "usage_metadata", None) or {}
        usage = Usage(int(meta.get("input_tokens", 0)), int(meta.get("output_tokens", 0)))
        resp_meta = getattr(raw, "response_metadata", None) or {}
        stop = resp_meta.get("stop_reason") or resp_meta.get("finish_reason")  # Anthropic / OpenAI-style
        if stop == "refusal":
            raise LLMError(f"{self.model_name}: request refused")
        if stop in ("max_tokens", "length"):
            raise LLMError(f"{self.model_name}: output truncated at max_tokens")
        if result.get("parsing_error") is not None or result.get("parsed") is None:
            raise LLMError(f"{self.model_name}: output did not match schema ({result.get('parsing_error')})")
        return result["parsed"], usage


def _claude_thinking(model: str) -> dict | None:
    # Thinking tokens bill as output, so turn it off where the model allows.
    if model.startswith("claude-haiku-5-5"):
        return {"type": "disabled"}       # allowed at effort high or below
    if model.startswith("claude-sonnet-5-5"):
        return {"type": "between_tools"}  # Sonnet 5.5's thinking-off mode
    return None                           # Opus 5.5 can't disable; low effort keeps it short


def make_claude(model: str, max_tokens: int, effort: str) -> LangChainLLM:
    import anthropic
    from langchain_anthropic import ChatAnthropic

    kwargs = {}
    thinking = _claude_thinking(model)
    if thinking:
        kwargs["thinking"] = thinking
    chat = ChatAnthropic(model=model, max_tokens=max_tokens, output_config={"effort": effort},
                         max_retries=2, timeout=60, **kwargs)
    # Retryable and non-retryable API errors both end the call; the SDK already retried 429/5xx.
    transient = (anthropic.RateLimitError, anthropic.APIStatusError, anthropic.APIConnectionError)
    return LangChainLLM(chat, model, transient)


# Groq models with strict schema-constrained output; others fall back to tool calling.
_GROQ_JSON_SCHEMA_MODELS = {"openai/gpt-oss-20b", "openai/gpt-oss-120b"}


def make_groq(model: str, max_tokens: int, reasoning_effort: str | None) -> LangChainLLM:
    import groq
    from langchain_groq import ChatGroq

    kwargs = {"reasoning_effort": reasoning_effort, "reasoning_format": "hidden"} if reasoning_effort else {}
    chat = ChatGroq(model=model, max_tokens=max_tokens, temperature=0.2, max_retries=2, timeout=60, **kwargs)
    structured = ({"method": "json_schema", "strict": True} if model in _GROQ_JSON_SCHEMA_MODELS
                  else {"method": "function_calling"})
    transient = (groq.RateLimitError, groq.APIStatusError, groq.APIConnectionError)
    return LangChainLLM(chat, model, transient, structured)


def make_ollama(model: str, base_url: str) -> LangChainLLM:
    import httpx
    import ollama
    from langchain_ollama import ChatOllama

    chat = ChatOllama(model=model, base_url=base_url, num_predict=1024)
    return LangChainLLM(chat, model, (ollama.ResponseError, httpx.HTTPError, ConnectionError))


def make_llm(settings) -> StructuredLLM | None:
    """None means rules-only mode (no AI)."""
    if settings.llm_provider == "claude":
        return make_claude(settings.claude_model, settings.claude_max_tokens, settings.claude_effort)
    if settings.llm_provider == "groq":
        return make_groq(settings.groq_model, settings.groq_max_tokens, settings.groq_reasoning_effort)
    if settings.llm_provider == "ollama":
        return make_ollama(settings.ollama_model, settings.ollama_base_url)
    return None
