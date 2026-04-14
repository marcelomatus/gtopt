# -*- coding: utf-8 -*-

"""Template engine for .tson and .tampl files using Jinja2 syntax.

Uses standard Jinja2 delimiters:

  {{ param }}       — variable substitution (auto JSON-serialized)
  {% ... %}         — block statements (for, if, etc.)
  {# ... #}         — comments

All printed values are automatically serialized as JSON via the finalize
callback, so {{ param }} produces valid JSON output:

  - Strings   → ``"ELTORO"``  (quoted)
  - Numbers   → ``5582.0``    (bare)
  - Booleans  → ``true``/``false``
  - Lists     → ``[1, 2, 3]``
  - Dicts     → ``{"key": "value"}``
  - None      → ``null``
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jinja2

_TEMPLATE_DIR = Path(__file__).parent / "templates"


def _json_finalize(value: Any) -> str:
    """Auto-serialize printed template values as JSON.

    Called by Jinja2 on every value that passes through a variable
    tag (``{{ param }}``).  Converts Python objects to their JSON
    representation so that the rendered template is valid JSON.
    """
    if isinstance(value, jinja2.Undefined):
        # Let StrictUndefined raise its own error
        return str(value)
    return json.dumps(value)


def create_template_env(
    template_dir: Path | str = _TEMPLATE_DIR,
) -> jinja2.Environment:
    """Create a Jinja2 environment with standard delimiters and JSON finalize.

    Args:
        template_dir: Directory containing template files.

    Returns:
        Configured Jinja2 Environment.
    """
    return jinja2.Environment(
        loader=jinja2.FileSystemLoader(template_dir),
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
        undefined=jinja2.StrictUndefined,
        finalize=_json_finalize,
    )


def render_tson(
    template_name: str,
    context: dict[str, Any],
    template_dir: Path | str = _TEMPLATE_DIR,
) -> dict[str, Any]:
    """Render a .tson template and parse the result as JSON.

    The template uses @param@ syntax for variable substitution.
    All values in the context are auto-serialized as JSON when
    printed.

    Args:
        template_name: Name of the template file (e.g. ``"laja.tson"``).
        context: Dictionary of template parameters.
        template_dir: Directory containing template files.

    Returns:
        Parsed JSON dictionary with entity arrays.
    """
    env = create_template_env(template_dir)
    template = env.get_template(template_name)
    rendered = template.render(context)
    return json.loads(rendered)
