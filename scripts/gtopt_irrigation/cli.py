# -*- coding: utf-8 -*-

"""Command-line interface for the gtopt irrigation agreement transform.

Usage::

    # Stage 2: laja.json → FlowRight/VolumeRight/UserConstraint + laja.pampl
    gtopt_irrigation laja --in laja.json --out outdir/

    # With explicit stage metadata (calendar months for monthly schedules)
    gtopt_irrigation maule --in maule.json --out outdir/ --stages stages.json

The ``--stages`` file must be a JSON document compatible with the
``BaseParser.get_all()`` shape: ``[{"number": 1, "month": 4}, ...]``.

When ``--stages`` is omitted, monthly arrays are emitted in their raw
hydro-year (Laja) or calendar (Maule) form, suitable for cases that don't
need a per-stage materialization.
"""

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from gtopt_irrigation.laja_agreement import LajaAgreement
from gtopt_irrigation.maule_agreement import MauleAgreement


class _StageList:
    """Tiny stage-parser shim for ``LajaAgreement`` / ``MauleAgreement``.

    The agreement classes only call ``get_all()``; this wrapper exposes a
    pre-loaded list of dicts so callers don't need a full ``BaseParser``.
    """

    def __init__(self, stages: List[Dict[str, Any]]):
        self._stages = stages

    def get_all(self) -> List[Dict[str, Any]]:
        return self._stages


def _load_stages(path: Optional[str]) -> Optional[_StageList]:
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
) -> None:
    """Write the agreement's entities and PAMPL file to ``out_dir``.

    The entities are written as ``<artifact_name>_entities.json`` containing
    ``flow_right_array`` / ``volume_right_array``.  The PAMPL file is
    written as ``<artifact_name>.pampl``.  When the agreement is
    constraint-only (no inline UserConstraint array left over), the
    ``user_constraint_files`` pointer in the entities document references
    the .pampl filename — matching the layout that ``gtopt`` expects via
    its plural ``user_constraint_files`` field.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    entities = agreement.to_json_dict(output_dir=out_dir)
    entities_path = out_dir / f"{artifact_name}_entities.json"
    with open(entities_path, "w", encoding="utf-8") as fh:
        json.dump(entities, fh, indent=2, sort_keys=False)
        fh.write("\n")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gtopt_irrigation",
        description=(
            "Convert canonical laja.json / maule.json agreement descriptions"
            " into gtopt FlowRight/VolumeRight/UserConstraint entities and"
            " their companion PAMPL files."
        ),
    )
    sub = parser.add_subparsers(dest="agreement", required=True)

    for name in ("laja", "maule"):
        sp = sub.add_parser(
            name,
            help=f"emit gtopt entities for the {name.capitalize()} agreement",
        )
        sp.add_argument(
            "--in",
            dest="input_path",
            required=True,
            help=f"path to {name}.json",
        )
        sp.add_argument(
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

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    stages = _load_stages(args.stages_path)
    options = {
        "last_stage": args.last_stage,
        "blocks_per_stage": args.blocks_per_stage,
    }

    if args.agreement == "laja":
        agreement: Any = LajaAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "laja"
    else:
        agreement = MauleAgreement.from_json(
            args.input_path, stage_parser=stages, options=options
        )
        artifact = "maule"

    out_dir = Path(args.output_dir)
    _emit(agreement, out_dir, artifact)
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
