# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""Shared CLI flag registrars for gtopt converters.

Each registrar mutates an :class:`argparse.ArgumentParser` to declare a
group of flags that has — until now — been hand-rolled by every
converter (``plp2gtopt``, ``plexos2gtopt``, ``sddp2gtopt``, …).
Centralising them here is the Phase 2 implementation of the
unification plan in issue #507:

> CLI flags ``--scale-objective``, ``--demand-fail-cost``,
> ``--use-single-bus``, ``--use-kirchhoff``, ``--line-losses-mode``,
> ``--loss-cost-eps``, ``--lift-line-caps``, ``--aperture-chunk-size``
> are declared **once** in ``gtopt_writer.cli_flags`` and consumed by
> all four converters.

This module is the seed; flags are pulled in as they are unified.  The
acceptance criterion is that adding a new gtopt-wide CLI flag touches
exactly one file (this one) and propagates to every converter that
calls the matching ``add_*_argument(s)`` registrar.

Conventions
-----------

* Each ``add_<feature>_argument(s)`` function takes ``parser`` and
  returns ``None``; it mutates the parser in place.
* Each function accepts an optional ``defaults: dict[str, Any]`` so a
  converter can override the shared default without forking the flag
  spelling / help text.  The function uses ``defaults.get(<dest>,
  <shared_default>)`` for every flag it owns.
* Help text is written for the **general** converter audience.  Tool-
  specific framing (``"plp2gtopt loads"`` vs ``"plexos2gtopt loads"``)
  is intentionally generalised to ``"the converter loads"`` so the
  shared help reads correctly under every binding.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Emissions (gtopt_shared.emissions integration)
# ---------------------------------------------------------------------------


def add_emissions_arguments(
    parser: argparse.ArgumentParser,
    defaults: dict[str, Any] | None = None,
) -> None:
    """Register ``--emissions`` / ``--emissions-file`` / ``--emissions-report``.

    ``--emissions`` is a master switch (off by default).  When set, the
    converter loads per-fuel CO2 factors from ``--emissions-file``
    (default: the bundled IPCC-2006 defaults), injects them onto every
    Fuel element that lacks one, synthesizes the
    ``emission_array['co2']`` pollutant row, and writes the
    ``--emissions-report``.  Any factor the converter source already
    supplied (PLEXOS XML, plp2gtopt's ``--plexos-overlay``, project
    JSON merge) always wins over the defaults.

    See :mod:`gtopt_shared.emissions` for the fill-in semantics.
    """
    d = defaults or {}
    parser.add_argument(
        "--emissions",
        dest="emissions",
        action="store_true",
        default=d.get("emissions", False),
        help=(
            "MASTER SWITCH for emission processing.  When set, the "
            "converter loads per-fuel CO2 factors from --emissions-file "
            "(default: bundled IPCC-2006 defaults), injects them onto "
            "every Fuel element that lacks one, synthesizes the "
            "emission_array['co2'] pollutant row, and writes the "
            "emissions report.  Source-supplied factors (PLEXOS XML, "
            "--plexos-overlay, project JSON merge) always win.  Off by "
            "default."
        ),
    )
    parser.add_argument(
        "--emissions-file",
        dest="emissions_file",
        type=Path,
        metavar="PATH",
        default=d.get("emissions_file", None),
        help=(
            "Custom emissions JSON file.  Only meaningful when "
            "--emissions is set.  Default: the bundled IPCC-2006 Vol 2 "
            "Ch 1 Table 1.4 file at "
            "gtopt_shared/data/ipcc_emission_factors.json."
        ),
    )
    parser.add_argument(
        "--emissions-report",
        dest="emissions_report",
        type=Path,
        metavar="FILE",
        default=d.get("emissions_report", None),
        help=(
            "Write a JSON report of the emissions fill-in (factor_added / "
            "factor_preserved / unknown_fuels / emission_array status) to "
            "FILE.  Only meaningful when --emissions is set.  Defaults to "
            "<output-dir>/plexos_emissions_report.json."
        ),
    )


# ---------------------------------------------------------------------------
# Topology
# ---------------------------------------------------------------------------


def add_use_single_bus_argument(
    parser: argparse.ArgumentParser,
    defaults: dict[str, Any] | None = None,
    *,
    short_flag: str | None = None,
    paired: bool = False,
    default: Any = False,
    help_override: str | None = None,
) -> None:
    """Register ``--use-single-bus`` (copperplate collapse toggle).

    The shared registrar covers the spread of converter-side
    conventions:

    * ``short_flag="-b"`` — plp2gtopt exposes a one-letter short form.
      Other converters use the long flag only.
    * ``paired=True`` — use argparse's ``BooleanOptionalAction`` so
      users get both ``--use-single-bus`` AND ``--no-use-single-bus``.
      Required when ``default=None`` triggers a converter-side
      auto-detect path that must be distinguishable from "user passed
      ``--no-use-single-bus``".
    * ``default`` — overrides the shared default ``False``.  plp2gtopt
      passes ``None`` so its zero-line auto-detect kicks in; other
      converters keep ``False``.
    * ``help_override`` — converter-specific richer wording (e.g.
      plp2gtopt documents the auto-detect rule in the help text).
      Falls back to the generic shared text otherwise.

    The shared default is plain ``store_true`` / ``default=False`` to
    match plexos2gtopt, sddp2gtopt, and pp2gtopt.  Issue #507 Phase 2
    will harmonise the auto-detect semantic across converters and
    retire these knobs.
    """
    _ = defaults or {}
    flag_names: list[str] = []
    if short_flag is not None:
        flag_names.append(short_flag)
    flag_names.append("--use-single-bus")
    help_text = help_override or (
        "collapse the multi-bus topology to a single bus (copperplate); "
        "the converter drops Kirchhoff voltage-angle constraints and "
        "treats the network as a single zone."
    )
    if paired:
        parser.add_argument(
            *flag_names,
            dest="use_single_bus",
            action=argparse.BooleanOptionalAction,
            default=default,
            help=help_text,
        )
    else:
        parser.add_argument(
            *flag_names,
            dest="use_single_bus",
            action="store_true",
            default=default,
            help=help_text,
        )
