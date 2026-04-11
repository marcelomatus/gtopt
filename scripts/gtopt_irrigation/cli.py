# -*- coding: utf-8 -*-

"""Command-line interface for the gtopt irrigation agreement transform.

Usage::

    # Stage 2: laja.json → FlowRight/VolumeRight/UserConstraint + laja.pampl
    gtopt_irrigation laja --input laja.json --output outdir/

    # With explicit stage metadata (calendar months for monthly schedules)
    gtopt_irrigation maule --input maule.json --output outdir/ \\
        --stages stages.json

The ``--stages`` file must be a JSON document compatible with the
``BaseParser.get_all()`` shape: ``[{"number": 1, "month": 4}, ...]``.

When ``--stages`` is omitted, monthly arrays are emitted in their raw
hydro-year (Laja) or calendar (Maule) form, suitable for cases that don't
need a per-stage materialization.

Exit codes
----------

* ``0`` — success.
* ``2`` — input/validation/IO error (missing file, bad JSON, schema
  violation, template failure).  A one-line ``ERROR:`` is written to
  stderr before exiting.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from gtopt_irrigation import __version__ as _pkg_version
from gtopt_irrigation.laja_agreement import LajaAgreement
from gtopt_irrigation.maule_agreement import MauleAgreement


class _StageList:
    """Tiny stage-parser shim for ``LajaAgreement`` / ``MauleAgreement``.

    The agreement classes only call ``get_all()``; this wrapper exposes a
    pre-loaded list of dicts so callers don't need a full ``BaseParser``.
    """

    def __init__(self, stages: list[dict[str, Any]]):
        self._stages = stages

    def get_all(self) -> list[dict[str, Any]]:
        return self._stages


def _load_stages(path: str | None) -> _StageList | None:
    if path is None:
        return None
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    # Accept either a bare list or a dict containing "stages".
    if isinstance(data, dict) and "stages" in data:
        data = data["stages"]
    if not isinstance(data, list):
        raise ValueError(
            f"--stages file '{path}' must contain a list (or {{'stages': [...]}})"
        )
    return _StageList(data)


def _emit(
    agreement: Any,
    out_dir: Path,
    artifact_name: str,
) -> Path:
    """Write the agreement's entities and PAMPL file to ``out_dir``.

    The entities are written as ``<artifact_name>_entities.json`` containing
    ``flow_right_array`` / ``volume_right_array``.  The PAMPL file is
    written as ``<artifact_name>.pampl`` (by ``to_json_dict`` itself).
    When the agreement is constraint-only (no inline UserConstraint array
    left over), the ``user_constraint_file`` pointer in the entities
    document references the ``.pampl`` filename — both singular
    ``user_constraint_file`` and plural ``user_constraint_files`` are
    accepted by the ``gtopt`` C++ ``System`` parser.

    Returns the path to the written entities JSON file.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    entities = agreement.to_json_dict(output_dir=out_dir)
    entities_path = out_dir / f"{artifact_name}_entities.json"
    with open(entities_path, "w", encoding="utf-8") as fh:
        json.dump(entities, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return entities_path


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gtopt_irrigation",
        description=(
            "Convert canonical laja.json / maule.json agreement descriptions"
            " into gtopt FlowRight/VolumeRight/UserConstraint entities and"
            " their companion PAMPL files."
        ),
        epilog=(
            "See docs/irrigation-agreements.md (section 11) for the full"
            " Stage-1 → Stage-3 pipeline and the canonical laja.json /"
            " maule.json schemas."
        ),
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {_pkg_version}",
    )
    sub = parser.add_subparsers(dest="agreement", required=True)

    for name in ("laja", "maule"):
        sp = sub.add_parser(
            name,
            help=f"emit gtopt entities for the {name.capitalize()} agreement",
        )
        sp.add_argument(
            "--input",
            "--in",
            dest="input_path",
            required=True,
            help=f"path to {name}.json",
        )
        sp.add_argument(
            "--output",
            "--out",
            dest="output_dir",
            required=True,
            help="output directory for entities + PAMPL file",
        )
        sp.add_argument(
            "--stages",
            dest="stages_path",
            default=None,
            help=(
                "optional JSON file with per-stage metadata"
                " (list of {number, month} dicts)"
            ),
        )
        sp.add_argument(
            "--last-stage",
            dest="last_stage",
            type=int,
            default=-1,
            help="truncate the stage list to this stage number (default: all)",
        )
        sp.add_argument(
            "--blocks-per-stage",
            dest="blocks_per_stage",
            type=int,
            default=1,
            help="number of blocks per stage (default: 1)",
        )
        if name == "maule":
            sp.add_argument(
                "--machicura-model",
                dest="machicura_model",
                choices=("pasada", "embalse", "run-of-river", "reservoir"),
                default=None,
                help=(
                    "Machicura topology variant.  'pasada' (default; English"
                    " synonym 'run-of-river') mirrors the historical PLP"
                    " simplification (Machicura is a pass-through junction,"
                    " retiros implicit at Colbun).  'embalse' (English"
                    " synonym 'reservoir') physically re-anchors riego +"
                    " Res 105 retiros at the downstream junction and models"
                    " Machicura as a daily-cycle reservoir.  When omitted,"
                    " the value stored in maule.json is used (or 'pasada' if"
                    " absent)."
                ),
            )

    return parser


def _run(args: argparse.Namespace) -> Path:
    """Execute the requested conversion and return the entities path."""
    stages = _load_stages(args.stages_path)
    options: dict[str, Any] = {
        "last_stage": args.last_stage,
        "blocks_per_stage": args.blocks_per_stage,
    }

    if args.agreement == "laja":
        agreement: Any = LajaAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "laja"
    else:
        if getattr(args, "machicura_model", None) is not None:
            options["machicura_model"] = args.machicura_model
        agreement = MauleAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "maule"

    out_dir = Path(args.output_dir)
    return _emit(agreement, out_dir, artifact)


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        entities_path = _run(args)
    except FileNotFoundError as exc:
        print(f"ERROR: input file not found: {exc}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as exc:
        print(f"ERROR: invalid JSON in input: {exc}", file=sys.stderr)
        return 2
    except (KeyError, ValueError, TypeError) as exc:
        print(f"ERROR: {args.agreement} agreement: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"ERROR: I/O failure: {exc}", file=sys.stderr)
        return 2

    pampl_path = entities_path.with_name(f"{args.agreement}.pampl")
    print(
        f"wrote {entities_path}"
        + (f" and {pampl_path}" if pampl_path.exists() else ""),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
