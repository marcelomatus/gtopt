# SPDX-License-Identifier: BSD-3-Clause
"""AI provider integration for gtopt_check_lp.

Supports Claude (Anthropic), OpenAI, DeepSeek, and GitHub Copilot AI.
All providers accept an optional API-key override; if omitted the key is read
from the environment variable documented per provider.
"""

import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Optional

_AI_PROVIDERS = ("claude", "openai", "deepseek", "github")
_AI_DEFAULT_PROVIDER = "github"

_AI_DEFAULT_MODEL: dict[str, str] = {
    "claude": "claude-3-5-sonnet-20241022",
    "openai": "gpt-4o",
    "deepseek": "deepseek-chat",
    "github": "gpt-4o",
}

_AI_INFEASIBILITY_PROMPT = """\
You are an expert in linear programming and mathematical optimization.
I will provide you with an infeasibility report from the gtopt LP solver tool.
The report includes output from multiple linear solvers (such as CPLEX, HiGHS,
COIN-OR CLP/CBC, and GLPK) that have analyzed the same infeasible LP problem.

Please analyze the combined report and:
1. Identify the specific variable(s) or constraint(s) causing the infeasibility
   (column or row names if present).
2. Explain the root cause of the infeasibility in plain terms.
3. Suggest concrete steps to fix the problem.
4. If the report includes IIS (Irreducible Infeasible Subsystem) output from
   a solver (CPLEX, HiGHS, CLP/CBC, GLPK), focus on those constraints first.
5. Cross-reference findings from the different solvers to identify the most
   likely cause of infeasibility.

Be concise but precise.  Use the variable and constraint names from the report.

--- BEGIN REPORT ---
{report}
--- END REPORT ---
"""


@dataclass
class AiOptions:
    """Options controlling the AI diagnostics step in :func:`check_lp`.

    Attributes
    ----------
    enabled:
        When ``True`` (default), send the combined report to *provider* for
        an expert diagnosis.
    provider:
        AI backend: ``"claude"``, ``"openai"``, ``"deepseek"``, or
        ``"github"``.
    model:
        Model override.  Empty string → use the provider default.
    prompt:
        Prompt template override.  Empty string → use the built-in
        :data:`_AI_INFEASIBILITY_PROMPT`.
    key:
        API key override.  Empty string → read from the environment.
    timeout:
        HTTP timeout for the AI API call in seconds.
    """

    enabled: bool = True
    provider: str = _AI_DEFAULT_PROVIDER
    model: str = ""
    prompt: str = ""
    key: str = ""
    timeout: int = 60


def query_ai(
    report: str,
    provider: str = _AI_DEFAULT_PROVIDER,
    model: Optional[str] = None,
    prompt_template: str = _AI_INFEASIBILITY_PROMPT,
    api_key: Optional[str] = None,
    timeout: int = 60,
) -> tuple[bool, str]:
    """
    Send the LP analysis *report* to an AI provider and return ``(ok, response)``.

    Parameters
    ----------
    report:
        The full text of the LP analysis report (static + solver output).
    provider:
        AI provider: ``"claude"``, ``"openai"``, ``"deepseek"``, or
        ``"github"``.
    model:
        Model name override.  Defaults to the provider-specific default.
    prompt_template:
        Prompt template containing a ``{report}`` placeholder.
    api_key:
        API key override.  When ``None``, the key is read from the
        corresponding environment variable.
    timeout:
        HTTP request timeout in seconds (default 60).

    Returns
    -------
    ``(True, response_text)`` on success, or ``(False, error_message)`` on
    failure.
    """
    provider = provider.lower()
    if provider not in _AI_PROVIDERS:
        return False, f"Unknown AI provider '{provider}'. Supported: {_AI_PROVIDERS}"

    effective_model = model or _AI_DEFAULT_MODEL.get(provider, "")
    full_prompt = prompt_template.format(report=report)

    if provider == "claude":
        return _query_claude(full_prompt, effective_model, api_key, timeout)
    if provider == "openai":
        return _query_openai(full_prompt, effective_model, api_key, timeout)
    if provider == "deepseek":
        return _query_deepseek(full_prompt, effective_model, api_key, timeout)
    if provider == "github":
        return _query_github(full_prompt, effective_model, api_key, timeout)

    return False, f"Provider '{provider}' is not yet implemented."


# ---------------------------------------------------------------------------
# Provider implementations
# ---------------------------------------------------------------------------


def _query_claude(
    prompt: str,
    model: str,
    api_key: Optional[str],
    timeout: int,
) -> tuple[bool, str]:
    """Call the Anthropic Claude API."""
    key = api_key or os.environ.get("ANTHROPIC_API_KEY", "")
    if not key:
        return (
            False,
            "No API key found for Claude.  Set the ANTHROPIC_API_KEY environment "
            "variable or pass --ai-key KEY.",
        )
    url = "https://api.anthropic.com/v1/messages"
    payload: dict[str, Any] = {
        "model": model,
        "max_tokens": 2048,
        "messages": [{"role": "user", "content": prompt}],
    }
    headers = {
        "x-api-key": key,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    }
    return _http_post_json(url, payload, headers, timeout)


def _query_openai(
    prompt: str,
    model: str,
    api_key: Optional[str],
    timeout: int,
) -> tuple[bool, str]:
    """Call the OpenAI Chat Completions API."""
    key = api_key or os.environ.get("OPENAI_API_KEY", "")
    if not key:
        return (
            False,
            "No API key found for OpenAI.  Set the OPENAI_API_KEY environment "
            "variable or pass --ai-key KEY.",
        )
    url = "https://api.openai.com/v1/chat/completions"
    payload: dict[str, Any] = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "content-type": "application/json",
    }
    return _http_post_json(url, payload, headers, timeout)


def _query_deepseek(
    prompt: str,
    model: str,
    api_key: Optional[str],
    timeout: int,
) -> tuple[bool, str]:
    """Call the DeepSeek Chat API (OpenAI-compatible).

    Reads the API key from ``DEEPSEEK_API_KEY``.
    """
    key = api_key or os.environ.get("DEEPSEEK_API_KEY", "")
    if not key:
        return (
            False,
            "No API key found for DeepSeek.  Set the DEEPSEEK_API_KEY environment "
            "variable or pass --ai-key KEY.",
        )
    url = "https://api.deepseek.com/v1/chat/completions"
    payload: dict[str, Any] = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "content-type": "application/json",
    }
    return _http_post_json(url, payload, headers, timeout)


def _query_github(
    prompt: str,
    model: str,
    api_key: Optional[str],
    timeout: int,
) -> tuple[bool, str]:
    """Call the GitHub Models AI API (OpenAI-compatible).

    Uses:
    * ``OPENAI_API_BASE`` env var as a custom base URL override (defaults to
      ``https://models.github.ai/inference`` — the GitHub Models endpoint).
      Can also point to ``https://api.githubcopilot.com`` for GitHub Copilot.
    * ``GITHUB_TOKEN`` env var for authentication (falls back to
      ``OPENAI_API_KEY``).  In GitHub Actions ``GITHUB_TOKEN`` is
      injected automatically.
    """
    key = (
        api_key
        or os.environ.get("GITHUB_TOKEN", "")
        or os.environ.get("OPENAI_API_KEY", "")
    )
    if not key:
        return (
            False,
            "No API key found for GitHub AI.  Set the GITHUB_TOKEN (or "
            "OPENAI_API_KEY) environment variable or pass --ai-key KEY.",
        )
    base_url = os.environ.get(
        "OPENAI_API_BASE", "https://models.github.ai/inference"
    ).rstrip("/")
    url = f"{base_url}/chat/completions"
    payload: dict[str, Any] = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
    }
    headers = {
        "Authorization": f"Bearer {key}",
        "content-type": "application/json",
    }
    return _http_post_json(url, payload, headers, timeout)


# ---------------------------------------------------------------------------
# Shared HTTP helper
# ---------------------------------------------------------------------------


def _http_post_json(
    url: str,
    payload: dict[str, Any],
    headers: dict[str, str],
    timeout: int,
) -> tuple[bool, str]:
    """
    POST JSON *payload* to *url* with *headers* and return ``(ok, text)``.

    Supports the Anthropic and OpenAI/compatible response shapes.
    """
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        # Anthropic Claude response shape
        if "content" in body and isinstance(body["content"], list):
            parts = [
                c.get("text", "") for c in body["content"] if c.get("type") == "text"
            ]
            return True, "\n".join(parts).strip()
        # OpenAI / DeepSeek / GitHub compatible response shape
        if "choices" in body and isinstance(body["choices"], list):
            text = body["choices"][0].get("message", {}).get("content", "")
            return True, text.strip()
        return True, json.dumps(body, indent=2)
    except urllib.error.HTTPError as exc:
        try:
            err_body = json.loads(exc.read().decode("utf-8"))
            msg = err_body.get("error", {}).get("message", str(exc))
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            msg = str(exc)
        return False, f"HTTP {exc.code} from AI API: {msg}"
    except urllib.error.URLError as exc:
        return False, f"Network error connecting to AI API: {exc.reason}"
    except TimeoutError:
        return False, f"Request to AI API timed out after {timeout}s."
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        return False, f"Unexpected error calling AI API: {exc}"
