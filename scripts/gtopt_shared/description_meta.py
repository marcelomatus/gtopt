# SPDX-License-Identifier: BSD-3-Clause
"""Side-channel metadata embedded in element ``description`` fields.

Format
------

A trailing ``[gtopt-meta key1=value1 key2=value2 …]`` block appended to
the free-form description, with a leading ``; `` separator when prose
is present.  The block must be the LAST thing in the description (prose
can contain any ``[`` except the closing ``]`` of an existing meta
block).

Grammar
~~~~~~~

* Sentinel: literal ``[gtopt-meta `` (lowercase, trailing space) ... ``]``
* Key: ``[a-z][a-z0-9_]*`` (snake_case, lowercase)
* Value: any sequence of ``[A-Za-z0-9_.,/-]+`` (no spaces, no ``]``)
* Boolean shorthand: bare key (no ``=``) ⇒ ``True``
* Multi-value: comma-separated (``tags=a,b,c``); caller splits

Examples
~~~~~~~~

>>> append_meta("PLEXOS biomass cogen", cogen_mode="dispatched")
'PLEXOS biomass cogen; [gtopt-meta cogen_mode=dispatched]'

>>> parse_meta("Foo; [gtopt-meta phantom_bus=1 owner=BAT_X]")
{'owner': 'BAT_X', 'phantom_bus': '1'}

>>> append_meta("PLEXOS gen", is_phantom=True, is_cogen=False)  # False/None skipped
'PLEXOS gen; [gtopt-meta is_phantom]'

Design rationale
----------------

The gtopt C++ JSON parser accepts ``description`` as opaque text — adding
the meta block requires no schema change.  Downstream consumers
(``gtopt_marginal_units``, ``gtopt_check``) can read structured info from
the block via :func:`parse_meta`; producers (``plexos2gtopt``,
``plp2gtopt``) write via :func:`append_meta`.  Idempotent on re-write
(existing block stripped before the new one is appended).

For data with LP-modeling implications, prefer a first-class schema
field (e.g. ``Generator.cogen_mode``).  Reserve this side-channel for
audit / provenance / downstream-only hints that don't justify schema
churn.
"""

from __future__ import annotations

import re
from typing import Union

#: Sentinel-bounded regex.  Captures the inner key=value tokens.
_META_RE = re.compile(r"\[gtopt-meta\s+([^\]]*?)\]")

MetaValue = Union[str, bool, int, float, None]


def parse_meta(description: str | None) -> dict[str, str | bool]:
    """Extract structured metadata from a ``description`` field.

    Returns a dict mapping key → value.  Bare keys (no ``=``) map to
    ``True``.  Values are always returned as strings (caller does any
    type coercion).  Returns ``{}`` when no meta block is present or
    ``description`` is empty / None.

    When the description contains multiple ``[gtopt-meta …]`` blocks
    (shouldn't happen in well-formed data; defensive), the RIGHTMOST
    one wins — :func:`append_meta` always strips before appending so
    rewrites are clean.
    """
    if not description:
        return {}
    matches = _META_RE.findall(description)
    if not matches:
        return {}
    # Rightmost wins (defensive against duplicates).
    body = matches[-1]
    out: dict[str, str | bool] = {}
    for tok in body.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
        else:
            out[tok] = True
    return out


def strip_meta(description: str | None) -> str:
    """Remove every ``[gtopt-meta …]`` block from the description.

    Cleans the trailing ``;`` / whitespace separator we add in
    :func:`append_meta`.  Returns ``""`` when ``description`` is None.
    """
    if not description:
        return ""
    cleaned = _META_RE.sub("", description)
    # Trim our own ``; `` separator + any trailing/leading whitespace.
    cleaned = re.sub(r";\s*$", "", cleaned.strip())
    return cleaned.strip()


def append_meta(description: str | None, **meta: MetaValue) -> str:
    """Append (or rewrite) a ``[gtopt-meta …]`` block on the description.

    Idempotent: any existing meta block is stripped first, so callers
    can safely re-apply.

    Value handling:
      * ``True``  → bare key (``is_phantom`` ≡ ``is_phantom=true``)
      * ``False``, ``None`` → key omitted (clean way to drop a flag)
      * everything else → str-cast and emitted as ``key=value``

    Raises ``ValueError`` when a key or value contains characters that
    would break the grammar (whitespace, ``]``, ``[``).  Callers should
    URL-encode or split-then-rejoin if they need exotic content; the
    grammar is intentionally narrow.
    """
    base = strip_meta(description)
    tokens: list[str] = []
    for k in sorted(meta):
        v = meta[k]
        if v is False or v is None:
            continue
        _validate_key(k)
        if v is True:
            tokens.append(k)
            continue
        sv = str(v)
        _validate_value(sv)
        tokens.append(f"{k}={sv}")
    if not tokens:
        return base
    block = "[gtopt-meta " + " ".join(tokens) + "]"
    return f"{base}; {block}" if base else block


def update_meta(description: str | None, **meta: MetaValue) -> str:
    """Like :func:`append_meta` but PRESERVES existing keys not in ``meta``.

    Reads the existing block, merges new keys (overriding overlaps), and
    re-writes.  Useful when multiple converter passes contribute keys.
    """
    existing = parse_meta(description)
    merged: dict[str, MetaValue] = dict(existing)
    for k, v in meta.items():
        merged[k] = v
    # ``append_meta`` strips False/None, so passing the merged dict
    # cleanly drops any key set to False here.
    return append_meta(description, **merged)


_KEY_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_VALUE_RE = re.compile(r"^[A-Za-z0-9_.,/+-]+$")


def _validate_key(key: str) -> None:
    if not _KEY_RE.match(key):
        raise ValueError(
            f"description_meta key must match [a-z][a-z0-9_]*, got {key!r}"
        )


def _validate_value(value: str) -> None:
    if not value:
        raise ValueError("description_meta value cannot be empty")
    if not _VALUE_RE.match(value):
        raise ValueError(
            f"description_meta value must match [A-Za-z0-9_.,/+-]+, got {value!r}"
        )


def get_meta(
    description: str | None,
    key: str,
    default: MetaValue = None,
) -> MetaValue:
    """Convenience: parse + lookup a single key.  Returns ``default``
    when the key is absent.  Bare keys come back as ``True``."""
    return parse_meta(description).get(key, default)
