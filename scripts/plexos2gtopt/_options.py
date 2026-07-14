"""Typed ``ConversionOptions`` — converter knobs as a single frozen object.

For 18 months the PLEXOS converter has shipped 19 individual ``GTOPT_*``
environment variables (set at :func:`plexos2gtopt.convert_plexos_bundle`
and read at 15+ sites in :mod:`parsers` / :mod:`gtopt_writer`).  The
env-var bridge made it easy to thread CLI flags into deep extractors
without changing every signature, but the cost has been:

* every consumer reimplements its own ``"0"``/``"1"`` /
  ``"midpoint"`` parsing, with no central type checks,
* a six-fold proliferation of ``import os as _os_*`` aliases inside
  per-function scopes,
* the "did the CLI flag actually reach the extractor?" debug loop
  requires grepping for ``GTOPT_<NAME>`` across the tree,
* mypy / pylint cannot see the option contract.

This module is the typed replacement:

* :class:`ConversionOptions` is a frozen dataclass with one field per
  knob and the same defaults as the CLI parser in
  :func:`plexos2gtopt.main.make_parser`.
* :func:`ConversionOptions.from_options_dict` builds one from the raw
  ``dict`` the CLI hands :func:`convert_plexos_bundle`.
* :func:`ConversionOptions.install_env` is the **single** bridge that
  exports the corresponding ``GTOPT_*`` env vars so legacy in-tree
  consumers still work without changes — but the env-var write side
  now lives in ONE place instead of being scattered across a 50-line
  if/elif tree.

Future PRs can migrate consumer call sites one at a time to take an
``opts: ConversionOptions`` parameter; the env-var bridge becomes
unnecessary at that point and can be removed.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, fields
from typing import Any


# ── The dataclass ──────────────────────────────────────────────────────────
@dataclass(frozen=True)
class ConversionOptions:
    """Every knob the PLEXOS → gtopt converter exposes, in one place.

    Field defaults match the CLI parser defaults
    (:func:`plexos2gtopt.main.make_parser`); the ``ENV_VAR`` mapping
    table at the bottom of this module pins each field to the legacy
    ``GTOPT_*`` env-var name it propagates to (for the back-compat
    bridge).

    All fields are kept :class:`Optional` so an empty options dict
    (typical for unit tests) produces a default-everything instance
    that exports NO env vars (leaving the bare process env untouched).
    """

    # ── hydro / waterway topology ────────────────────────────────────────
    vert_routing: str | None = None
    reservoir_spillway: str | None = None
    # Per-reservoir terminal water-value factor for boundary_cuts.csv,
    # e.g. "COLBUN:0.9,RALCO:0.85" (see parse_water_value_factor).
    water_value_factor: str | None = None
    spill_fcost: float | None = None
    spill_fcost_scale: float | None = None
    emin_eod_day1: bool | None = None
    battery_efin_pin: bool | None = None
    # Exclude every PLEXOS Battery object (BESS) from the converted model,
    # identified by PLEXOS class "Battery" (not name-matching).  Gates
    # extraction, SSCC reserve provisions and UserConstraint battery terms
    # consistently so the JSON loads with no unresolved references.
    drop_batteries: bool | None = None
    hydro_min_mode: str | None = None
    # ── line losses / capacity ──────────────────────────────────────────
    nseg_losses: int | None = None
    loss_error_pct: float | None = None
    loss_extend_overload: bool | None = None
    loss_pwl_layout: str | None = None
    loss_tangent_lines: str | None = None
    nseg_tangent: int | None = None
    nseg_uniform: int | None = None
    # L-secant + SOS2 selector (issue #504 task #5).  ``loss_sos2_lines``
    # is the explicit per-line opt-in (CSV); ``loss_sos2_auto`` is the
    # heuristic auto-rule (off / heavy / all-lossy).  The post-pass
    # stamps the UNION of both sources on each LineSpec's
    # ``loss_secant_segments`` and ``loss_use_sos2`` fields.
    loss_sos2_lines: str | None = None
    loss_sos2_auto: str | None = None
    lift_line_caps: str | None = None
    lift_hard: bool = False
    no_lift_lines: str | None = None
    el0_lines: str | None = None
    # ── PLEXOS-solution curve-fit modes ─────────────────────────────────
    use_plexos_commit: bool | None = None
    use_plexos_gen_cap: bool | None = None
    use_plexos_efin: bool | None = None
    # Apply Gen_AuxUse.csv generation auxiliary use as Generator.lossfactor.
    # Default off (PLEXOS applies zero aux); opt in to model self-consumption.
    apply_generation_aux_use: bool | None = None

    # ── Constructors ────────────────────────────────────────────────────
    @classmethod
    def from_options_dict(cls, options: dict[str, Any]) -> ConversionOptions:
        """Build a :class:`ConversionOptions` from the CLI's options ``dict``.

        Unknown keys are silently ignored so :func:`convert_plexos_bundle`
        can keep handling additional non-conversion options (paths,
        write modes, ``run_check``, …) without each one needing an
        opt-in here.

        Empty / ``None`` entries become ``None`` on the dataclass so
        ``install_env`` knows not to clobber the user's shell env.
        """
        kw: dict[str, Any] = {}
        names = {f.name for f in fields(cls)}
        for k, v in options.items():
            if k in names:
                kw[k] = v
        return cls(**kw)

    # ── Env-var bridge (legacy back-compat) ─────────────────────────────
    def install_env(self, env: dict[str, str] | None = None) -> None:
        """Export each set field as the matching ``GTOPT_*`` env var.

        Replaces the 50-line if/elif tree that used to live at the top
        of :func:`plexos2gtopt.convert_plexos_bundle`.  A formatter
        that returns ``None`` signals "skip this field" (used for the
        truthy-only flags whose legacy semantics required leaving the
        env var ABSENT when the option is ``False`` rather than writing
        ``"0"``).

        ``env`` defaults to :data:`os.environ` and is parameterised
        only for unit testing.
        """
        target = os.environ if env is None else env
        for field_name, env_var, formatter in _ENV_BRIDGE:
            value = getattr(self, field_name)
            if value is None:
                continue
            rendered = formatter(value)
            if rendered is None:
                continue
            target[env_var] = rendered


# ── Field → (env-var name, formatter) mapping table ────────────────────
# Mirrors the if-block that used to live at plexos2gtopt.py:111-160.
# Adding a new option = add a field above + one row here; nothing else
# has to change.  Formatters are deliberately tiny so a reader can see
# the wire-form mapping at a glance.
def _str_bool_zero_one(v: bool) -> str:
    """Boolean → ``"1"`` / ``"0"`` (legacy ``"key" in options`` semantics)."""
    return "1" if v else "0"


def _str_bool_one_only(v: bool) -> str | None:
    """Truthy-only flag: ``True`` ⇒ ``"1"``, ``False`` ⇒ skip env-var write.

    Matches the legacy ``if options.get("use_plexos_commit"):`` pattern
    where the env var stayed ABSENT when the flag was missing or
    falsy.  Returning ``None`` signals ``install_env`` to skip the
    write so the consumer's ``os.environ.get(KEY, "0")`` fallback
    fires.
    """
    return "1" if v else None


_ENV_BRIDGE: tuple[tuple[str, str, Any], ...] = (
    ("vert_routing", "GTOPT_VERT_ROUTING", str),
    ("use_plexos_commit", "GTOPT_USE_PLEXOS_COMMIT", _str_bool_one_only),
    ("use_plexos_gen_cap", "GTOPT_USE_PLEXOS_GEN_CAP", _str_bool_one_only),
    ("use_plexos_efin", "GTOPT_USE_PLEXOS_EFIN", _str_bool_one_only),
    (
        "apply_generation_aux_use",
        "GTOPT_APPLY_GENERATION_AUX_USE",
        _str_bool_one_only,
    ),
    ("lift_line_caps", "GTOPT_LIFT_LINE_CAPS", str),
    ("lift_hard", "GTOPT_LIFT_HARD", _str_bool_one_only),
    ("no_lift_lines", "GTOPT_NO_LIFT_LINES", str),
    ("el0_lines", "GTOPT_EL0_LINES", str),
    ("reservoir_spillway", "GTOPT_RESERVOIR_SPILL", str),
    ("water_value_factor", "GTOPT_WATER_VALUE_FACTOR", str),
    ("spill_fcost", "GTOPT_SPILL_FCOST", lambda v: f"{float(v)}"),
    ("spill_fcost_scale", "GTOPT_SPILL_FCOST_SCALE", lambda v: f"{float(v)}"),
    ("nseg_losses", "GTOPT_NSEG_LOSSES", lambda v: str(int(v))),
    ("loss_error_pct", "GTOPT_LOSS_ERROR_PCT", lambda v: f"{float(v)}"),
    ("loss_extend_overload", "GTOPT_LOSS_EXTEND_OVERLOAD", _str_bool_one_only),
    ("loss_pwl_layout", "GTOPT_LOSS_PWL_LAYOUT", str),
    ("hydro_min_mode", "GTOPT_HYDRO_MIN_MODE", str),
    ("loss_tangent_lines", "GTOPT_LOSS_TANGENT_LINES", str),
    ("nseg_tangent", "GTOPT_NSEG_TANGENT", lambda v: str(int(v))),
    ("nseg_uniform", "GTOPT_NSEG_UNIFORM", lambda v: str(int(v))),
    ("loss_sos2_lines", "GTOPT_LOSS_SOS2_LINES", str),
    ("loss_sos2_auto", "GTOPT_LOSS_SOS2_AUTO", str),
    ("emin_eod_day1", "GTOPT_EMIN_EOD_DAY1", _str_bool_zero_one),
    ("battery_efin_pin", "GTOPT_BATTERY_PIN_EFIN", _str_bool_zero_one),
    ("drop_batteries", "GTOPT_DROP_BATTERIES", _str_bool_one_only),
)


__all__ = ["ConversionOptions"]
