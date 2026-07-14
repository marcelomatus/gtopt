# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``gtopt_shared.state_snapshot.apply_state_to_args``.

Regression coverage for the documented precedence
``argparse defaults < state snapshot < explicit CLI``.

The headline case is **forward compatibility**: a snapshot written before
an option existed (so its dest is absent from ``state["args"]``) must still
yield a Namespace that carries the option at its *current* default, instead
of an incomplete Namespace that raises ``AttributeError`` the moment any
caller reads ``args.<new_option>``.  This is exactly what broke
``plexos2gtopt --from-state`` after ``--drop-batteries`` was added: an
older ``plexos2gtopt_state.json`` had no ``drop_batteries`` key, and
``main.py`` reads ``args.drop_batteries`` unconditionally.
"""

from __future__ import annotations

import argparse

from gtopt_shared.state_snapshot import apply_state_to_args


def _make_parser() -> argparse.ArgumentParser:
    """A parser with a couple of options, one of them a late arrival."""
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", nargs="?", default=None)
    parser.add_argument("--output-dir", dest="output_dir", default="out")
    parser.add_argument("--nseg", dest="nseg", type=int, default=4)
    # The "newly added" option — a real snapshot on disk predates it.
    parser.add_argument(
        "--drop-batteries",
        dest="drop_batteries",
        action="store_true",
        default=False,
    )
    return parser


def test_missing_new_option_falls_back_to_default() -> None:
    """A snapshot predating an option must not drop that option's dest."""
    # ``drop_batteries`` is deliberately absent (old snapshot).
    state_args = {"bundle": "case1", "output_dir": "prev", "nseg": 7}
    ns = apply_state_to_args(_make_parser(), state_args, cli_argv=[])
    # The pre-existing keys survive from the snapshot ...
    assert ns.output_dir == "prev"
    assert ns.nseg == 7
    # ... and the option the snapshot never knew about is present with its
    # current default rather than missing (would have raised AttributeError).
    assert hasattr(ns, "drop_batteries")
    # getattr (not attribute access) keeps pylint's no-member check quiet on
    # argparse.Namespace's dynamically-populated attributes.
    assert getattr(ns, "drop_batteries") is False  # noqa: B009


def test_state_value_overrides_default() -> None:
    """A value stored in the snapshot wins over the bare argparse default."""
    state_args = {"nseg": 9, "drop_batteries": True}
    ns = apply_state_to_args(_make_parser(), state_args, cli_argv=[])
    assert ns.nseg == 9
    assert getattr(ns, "drop_batteries") is True  # noqa: B009


def test_explicit_cli_overrides_state() -> None:
    """The current CLI line wins over the snapshot (highest precedence)."""
    state_args = {"nseg": 9, "output_dir": "prev"}
    ns = apply_state_to_args(_make_parser(), state_args, cli_argv=["--nseg", "3"])
    # CLI-supplied --nseg overrides the snapshot's 9 ...
    assert ns.nseg == 3
    # ... but the un-overridden snapshot value stays authoritative.
    assert ns.output_dir == "prev"


def test_from_state_flag_is_stripped() -> None:
    """``--from-state`` on the current line must not shadow the reload."""
    state_args = {"nseg": 5}
    ns = apply_state_to_args(
        _make_parser(),
        state_args,
        cli_argv=["--from-state", "snap.json", "--nseg", "2"],
    )
    assert ns.nseg == 2
