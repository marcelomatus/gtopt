# -*- coding: utf-8 -*-

"""Backward-compatibility shim for the template engine.

The .tson/.tampl template engine moved to ``gtopt_expand._template_engine``
together with the irrigation agreement templates.  This module re-exports the
public helpers so that any external caller using the old module path keeps
working.
"""

from gtopt_expand._template_engine import (  # noqa: F401
    create_template_env,
    render_tson,
)

__all__ = ["create_template_env", "render_tson"]
