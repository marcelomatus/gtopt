# SPDX-License-Identifier: BSD-3-Clause
"""State snapshot + output-dir README helpers shared by plexos2gtopt and plp2gtopt.

Each converter writes a self-describing pair of files into its output
directory at the START of every run:

  ``<output_dir>/<tool>_state.json``
      Full CLI invocation + every parsed argument (after defaults).
      A reload via ``--from-state PATH`` reproduces the exact same run.

  ``<output_dir>/README.md``
      Plain-text guide listing what's in the output directory + how to
      re-run the conversion from the state file.

This module is consumed by:

  * :mod:`plexos2gtopt.main`  (CLI ``plexos2gtopt``)
  * :mod:`plp2gtopt.main`     (CLI ``plp2gtopt``)

The C++ ``gtopt`` binary writes its own ``<output_dir>/gtopt_state.json``
(see ``source/gtopt_main.cpp``) plus its README via
:func:`write_gtopt_readme` here so the three tools share the same
``<tool>_state.json`` naming convention and documentation style.
"""

from __future__ import annotations

import argparse
import datetime
import json
import logging
import os
import sys
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


def _jsonable(obj: Any) -> Any:
    """Convert non-JSON-serialisable Python objects to JSON-safe values."""
    if isinstance(obj, Path):
        return str(obj)
    if isinstance(obj, (list, tuple)):
        return [_jsonable(x) for x in obj]
    if isinstance(obj, dict):
        return {str(k): _jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (str, int, float, bool)) or obj is None:
        return obj
    # Fallback: stringify (catches frozenset, enums, etc.)
    return str(obj)


def write_state_snapshot(
    *,
    output_dir: Path,
    tool_name: str,
    args: argparse.Namespace,
    tool_version: str = "",
    extra: dict[str, Any] | None = None,
) -> Path:
    """Serialise ``args`` + invocation metadata to
    ``<output_dir>/<tool_name>_state.json``.

    The file carries:

      * ``_meta.tool``        — the CLI command name (e.g. ``plexos2gtopt``).
      * ``_meta.version``     — converter version string (when supplied).
      * ``_meta.timestamp``   — ISO-8601 UTC timestamp of run start.
      * ``_meta.argv``        — the exact ``sys.argv`` list of the run.
      * ``_meta.cwd``         — working directory the run was launched from
                                (relative paths in ``args`` are resolved
                                against this dir on reload).
      * ``args``              — ``vars(args)`` after argparse defaults
                                were applied.  This is the SOLE source of
                                truth for reload — every option seen by
                                the converter is captured here.
      * ``extra``             — caller-supplied keys (the parsed
                                ``options`` dict the tool actually
                                dispatches against, for traceability).

    The file is created atomically (write to ``.tmp`` then rename) so a
    SIGTERM partway through the converter doesn't leave a truncated
    snapshot on disk.

    Returns the path the snapshot was written to.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / f"{tool_name}_state.json"
    payload = {
        "_meta": {
            "tool": tool_name,
            "version": tool_version,
            "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            ),
            "argv": list(sys.argv),
            "cwd": str(Path.cwd()),
        },
        "args": _jsonable(vars(args)),
        "extra": _jsonable(extra or {}),
    }
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False))
    os.replace(tmp, path)
    logger.info("pre-run state snapshot: %s", path)
    return path


def load_state_snapshot(path: Path) -> dict[str, Any]:
    """Load a previously-written state snapshot.

    Returns the parsed JSON payload (``{"_meta": ..., "args": ...,
    "extra": ...}``).  Raises ``FileNotFoundError`` when ``path``
    doesn't exist, ``ValueError`` when the file isn't a recognisable
    state snapshot (missing ``args`` key).
    """
    if not path.is_file():
        raise FileNotFoundError(f"state snapshot not found: {path}")
    payload = json.loads(path.read_text())
    if "args" not in payload:
        raise ValueError(
            f"{path}: not a state snapshot (missing 'args' key); "
            "did you point at the wrong file?"
        )
    return payload


def _coerce_via_action_type(
    parser: argparse.ArgumentParser, ns: argparse.Namespace
) -> None:
    """Walk parser._actions and apply each action's ``type`` callable
    to the corresponding ``ns`` attribute when:
      * the attribute exists,
      * it's a ``str`` (JSON-serialised),
      * the action has a typed ``type`` (Path / int / float / etc.) that's
        different from ``str``.

    Lists carry the action's type as the element type, so we map each
    element through it.  Tuple-of-Path / nested-list shapes don't appear
    in either converter's argparse surface today; if they're added later
    the simplest fix is to extend this helper.
    """
    for action in parser._actions:
        dest = action.dest
        if dest == "help":
            continue
        if not hasattr(ns, dest):
            continue
        val = getattr(ns, dest)
        if val is None:
            continue
        t = action.type
        if t is None or t is str or not callable(t):
            continue
        try:
            if isinstance(val, str):
                setattr(ns, dest, t(val))
            elif isinstance(val, list):
                setattr(
                    ns,
                    dest,
                    [t(x) if isinstance(x, str) else x for x in val],
                )
        except (TypeError, ValueError) as exc:
            logger.warning(
                "apply_state_to_args: could not coerce %s=%r via %s: %s",
                dest,
                val,
                t,
                exc,
            )


def apply_state_to_args(
    parser: argparse.ArgumentParser,
    state_args: dict[str, Any],
    cli_argv: list[str],
) -> argparse.Namespace:
    """Merge a state-file args dict onto CLI-supplied overrides.

    Order of precedence (highest wins):
      1. Anything the operator passed on the current CLI line
         (parsed via ``parser``).
      2. Values from ``state_args``.
      3. Argparse defaults (already baked into ``parser``).

    After merging, JSON-serialised values are coerced back to the
    types declared by argparse (``Path`` for ``type=Path``, ``float``
    for ``type=float``, …).  Without this step, downstream callers
    that compare ``isinstance(args.foo, Path)`` would see a ``str``
    instead.

    The current CLI overrides MAY include the ``--from-state`` flag
    itself; we filter it out of the parser run by stripping it from
    ``cli_argv`` so the reload is idempotent.
    """
    # Strip ``--from-state PATH`` (and its ``=PATH`` form) from the
    # current CLI line so it doesn't shadow itself on the inner
    # ``parser.parse_args`` call.
    filtered_argv: list[str] = []
    skip_next = False
    for tok in cli_argv:
        if skip_next:
            skip_next = False
            continue
        if tok == "--from-state":
            skip_next = True
            continue
        if tok.startswith("--from-state="):
            continue
        filtered_argv.append(tok)

    # Identify which dest fields the operator EXPLICITLY passed (vs
    # filled by argparse defaults).  We only carry over those, so
    # the state's values stay authoritative for anything not on the
    # current CLI line.
    explicit: dict[str, bool] = {}
    opt_strings_to_dest = {o: a.dest for a in parser._actions for o in a.option_strings}
    for tok in filtered_argv:
        flag = tok.split("=", 1)[0]
        if flag in opt_strings_to_dest:
            explicit[opt_strings_to_dest[flag]] = True

    # Parse the FULL filtered command line — gives all defaults
    # filled + any per-invocation override applied + correct types.
    cli_ns = parser.parse_args(filtered_argv)

    # Build merged namespace honouring the documented precedence
    # (defaults < state < explicit CLI):
    #   1. base layer = every argparse default (from the parsed CLI
    #      namespace).  This is what lets an OLD snapshot survive the
    #      addition of new options: a state file written before e.g.
    #      ``--drop-batteries`` existed has no ``drop_batteries`` key, and
    #      without this base layer the merged Namespace would lack that
    #      dest entirely — every downstream ``args.drop_batteries`` then
    #      raises ``AttributeError``.  Seeding from ``cli_ns`` gives the
    #      current default for any option the snapshot predates.
    #   2. overlay the snapshot's stored values (authoritative for
    #      anything the operator did not re-specify this run);
    #   3. overlay the dests the operator EXPLICITLY passed this run.
    merged = argparse.Namespace(**vars(cli_ns))
    for key, value in state_args.items():
        setattr(merged, key, value)
    for dest in explicit:
        if hasattr(cli_ns, dest):
            setattr(merged, dest, getattr(cli_ns, dest))

    # Restore typed fields from JSON-deserialised primitives.
    _coerce_via_action_type(parser, merged)
    return merged


# ---------------------------------------------------------------------------
# README.md generators — one per tool.
# ---------------------------------------------------------------------------


_README_HEADER = """# {tool} output directory

This directory contains the output of a `{tool}` run.

| File / dir | Purpose |
|---|---|
"""

_README_RERUN = """
## Reproducing this run

Every `{tool}` invocation writes a **state snapshot** to
`{state_file}` at the START of the run, capturing the exact
command-line + every parsed option (after defaults).

### Re-run with the same options

```bash
{tool} --from-state {state_file}
```

The snapshot file is self-contained; the `--from-state` flag rebuilds
the argparse namespace from it and dispatches the converter as if you
had typed the original command.

### Override individual options on reload

```bash
{tool} --from-state {state_file} --some-flag NEW_VALUE
```

CLI flags passed alongside `--from-state` override the values stored
in the snapshot (the snapshot is a baseline; the current CLI line
wins).

### Inspect the snapshot

```bash
jq '.args' {state_file}            # all parsed options
jq '._meta' {state_file}           # tool / version / timestamp / argv / cwd
jq '.extra' {state_file}           # parsed options dict the converter consumes
```
"""


def _write_readme(output_dir: Path, body: str) -> Path:
    """Atomic write of ``output_dir/README.md`` with the given body."""
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / "README.md"
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(body)
    os.replace(tmp, path)
    return path


def write_plexos2gtopt_readme(output_dir: Path) -> Path:
    """README for ``plexos2gtopt`` output directory."""
    rows = [
        ("`<bundle_stem>.json`", "Converted gtopt planning JSON — the primary output."),
        (
            "`<bundle_stem>.provenance.json`",
            "Conversion provenance sidecar: PLEXOS source classes, units, "
            "and per-element transforms.",
        ),
        (
            "`plexos_emissions_report.json`",
            "Which fuels received IPCC default emission factors, which were "
            "preserved from PLEXOS, EmissionZone synthesis status.",
        ),
        (
            "`boundary_cuts.csv`",
            "Future-cost-function cuts emitted alongside the planning JSON.",
        ),
        (
            "`uc_*.pampl`",
            "PAMPL-formatted UserConstraint families "
            "(uc_commitment, uc_security, uc_operational, etc.).",
        ),
        (
            "`solvers/`",
            "Solver parameter files (e.g. `cplex.prm`) copied from the bundle.",
        ),
        (
            "`plexos2gtopt_state.json`",
            "**State snapshot** — full CLI invocation + every parsed option, "
            "written at the START of the run.  See *Reproducing this run* below.",
        ),
        ("`README.md`", "This file."),
    ]
    table_body = "\n".join(f"| {n} | {d} |" for n, d in rows)
    body = (
        _README_HEADER.format(tool="plexos2gtopt")
        + table_body
        + _README_RERUN.format(
            tool="plexos2gtopt", state_file="plexos2gtopt_state.json"
        )
    )
    return _write_readme(output_dir, body)


def write_plp2gtopt_readme(output_dir: Path) -> Path:
    """README for ``plp2gtopt`` output directory."""
    rows = [
        ("`<case_stem>.json`", "Converted gtopt planning JSON — the primary output."),
        (
            "`<case_stem>/`",
            "Per-class input-data directory (Generator/, Line/, Demand/, "
            "Reservoir/, Battery/, Waterway/, …) with Parquet time-series.",
        ),
        ("`logs/`", "Per-stage / per-block converter logs."),
        (
            "`boundary_cuts.csv`",
            "Future-cost-function cuts emitted alongside the planning JSON.",
        ),
        (
            "`solvers/`",
            "Solver parameter files (e.g. `cplex.prm`) copied from defaults.",
        ),
        (
            "`plp2gtopt_state.json`",
            "**State snapshot** — full CLI invocation + every parsed option, "
            "written at the START of the run.  See *Reproducing this run* below.",
        ),
        ("`README.md`", "This file."),
    ]
    table_body = "\n".join(f"| {n} | {d} |" for n, d in rows)
    body = (
        _README_HEADER.format(tool="plp2gtopt")
        + table_body
        + _README_RERUN.format(tool="plp2gtopt", state_file="plp2gtopt_state.json")
    )
    return _write_readme(output_dir, body)


def write_gtopt_readme(output_dir: Path) -> Path:
    """README for the ``gtopt`` standalone-binary output directory.

    Distinct from the converter READMEs: gtopt's state-snapshot mechanism
    lives in C++ (``source/gtopt_main.cpp``) and writes the
    fully-merged Planning JSON to ``<output_dir>/gtopt_state.json``
    BEFORE solve.  Reload uses the existing ``-s`` flag rather than a
    Python-side ``--from-state``.
    """
    rows = [
        (
            "`planning.json`",
            "Post-solve planning sidecar (Planning as gtopt left it after "
            "solving — useful for downstream tooling).",
        ),
        (
            "`gtopt_state.json`",
            "**Pre-solve state snapshot** — fully-merged Planning (all `-s` "
            "inputs unified + every CLI override applied) written immediately "
            "BEFORE solve.  See *Reproducing this run* below.",
        ),
        (
            "`solver_status.json`",
            "One-line solver outcome (status, method, elapsed time, scenes done).",
        ),
        (
            "`<Class>/`",
            "Per-LP-class output Parquet streams (Generator/generation_sol.parquet, "
            "Bus/balance_dual.parquet, Line/flowp_sol.parquet, …).  Which fields "
            "appear depends on `--write-out`.",
        ),
        ("`logs/`", "Per-(scene, phase) solver logs (cplex_sc0_ph0.log, gtopt_1.log)."),
        ("`README.md`", "This file."),
    ]
    table_body = "\n".join(f"| {n} | {d} |" for n, d in rows)
    body = (
        _README_HEADER.format(tool="gtopt")
        + table_body
        + """
## Reproducing this run

`gtopt` writes a **pre-solve state snapshot** to
`gtopt_state.json` at the START of every run, capturing the
fully-merged Planning (all `-s` input files unified + every CLI
override applied — `--set`, `--no-mip`, `--solver`, `--no-scale`,
`--method`, etc.).

### Re-run with the same options

```bash
gtopt -s gtopt_state.json
```

The snapshot is self-contained; PAMPL `user_constraint_file` references
are stripped before write (constraints are inlined into
`user_constraint_array`), so a reload doesn't re-parse them on top of
already-merged constraints.

### Reproducibility contract

Re-running with no other CLI flags produces a **stable** LP across
iterations: snapshot N=2 vs snapshot N=3 written from a snapshot
reload are byte-identical, including the per-block LP coefficients.
Use the snapshot for bug reports, audit trails, and regression-test
fixtures.

### Inspect the snapshot

```bash
jq '.options' gtopt_state.json
jq '.simulation' gtopt_state.json
jq '.system | keys' gtopt_state.json
```
"""
    )
    return _write_readme(output_dir, body)


__all__ = [
    "apply_state_to_args",
    "load_state_snapshot",
    "write_gtopt_readme",
    "write_plexos2gtopt_readme",
    "write_plp2gtopt_readme",
    "write_state_snapshot",
]
