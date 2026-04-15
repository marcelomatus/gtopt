# -*- coding: utf-8 -*-
"""SimulationWriter: assembles the ``simulation`` block of the gtopt JSON.

This module is extracted from :class:`~.gtopt_writer.GTOptWriter` to give
the simulation-assembly logic its own testable unit.  It handles:

- ``block_array`` and ``stage_array`` (from block/stage parsers)
- ``phase_array`` (SDDP one-per-stage, monolithic all-in-one, or custom
  ``--stages-phase`` spec)
- ``scenario_array`` and ``scene_array`` (from hydrology indices)
- ``block_hour_map`` (from ``indhor.csv`` if present)
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional

from .block_writer import BlockWriter
from .stage_writer import StageWriter
from .index_utils import parse_index_range, parse_stages_phase
from .indhor_writer import IndhorWriter


class SimulationWriter:
    """Builds the ``simulation`` section of the gtopt JSON planning structure.

    Parameters
    ----------
    parsed_data:
        The ``PLPParser.parsed_data`` dict containing parser instances.
    options:
        Conversion options dict (same dict passed throughout the pipeline).
    """

    def __init__(self, parsed_data: Dict[str, Any], options: Dict[str, Any]) -> None:
        self._parsed_data = parsed_data
        self._options = options
        self._simulation: Dict[str, Any] = {}

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def build(self) -> Dict[str, Any]:
        """Build and return the complete simulation dict."""
        self._build_blocks_and_stages()
        self._build_indhor()
        self._build_scenarios()
        return self._simulation

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _normalize_method(method: str) -> str:
        if method in ("mono", "monolithic"):
            return "monolithic"
        if method == "cascade":
            return "cascade"
        return "sddp"

    def _build_blocks_and_stages(self) -> None:
        stage_parser = self._parsed_data.get("stage_parser")
        block_parser = self._parsed_data.get("block_parser")

        stages = stage_parser.items if stage_parser else []
        blocks_items = block_parser.items if block_parser else []

        for stage in stages:
            stage_blocks = [
                idx
                for idx, blk in enumerate(blocks_items)
                if blk["stage"] == stage["number"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        self._simulation["block_array"] = BlockWriter(
            block_parser=block_parser, options=self._options
        ).to_json_array()

        self._simulation["stage_array"] = StageWriter(
            stage_parser=stage_parser,
            block_parser=block_parser,
            options=self._options,
        ).to_json_array(stages)

        num_stages = len(self._simulation["stage_array"])
        self._simulation["phase_array"] = self._build_phase_array(num_stages)

    def _build_phase_array(self, num_stages: int) -> List[Dict[str, Any]]:
        stages_phase_spec = self._options.get("stages_phase", None)
        if stages_phase_spec:
            phase_groups = (
                stages_phase_spec
                if isinstance(stages_phase_spec, list)
                else parse_stages_phase(stages_phase_spec, num_stages)
            )
            return [
                {
                    "uid": uid,
                    "first_stage": group[0] - 1,
                    "count_stage": len(group),
                }
                for uid, group in enumerate(phase_groups, start=1)
            ]

        method = self._normalize_method(self._options.get("method", "cascade"))
        if method == "monolithic":
            return [{"uid": 1, "first_stage": 0, "count_stage": num_stages}]
        return [
            {"uid": i + 1, "first_stage": i, "count_stage": 1}
            for i in range(num_stages)
        ]

    def _build_indhor(self) -> None:
        indhor_parser = self._parsed_data.get("indhor_parser")
        if indhor_parser is None or indhor_parser.is_empty:
            return
        output_dir = Path(self._options.get("output_dir", "results"))
        block_hour_dir = output_dir / IndhorWriter.SUBDIR
        writer = IndhorWriter(indhor_parser, self._options)
        rel_path = writer.to_parquet(block_hour_dir)
        if rel_path:
            self._simulation["block_hour_map"] = rel_path

    def _build_scenarios(self) -> None:
        hydro_spec = self._options.get("hydrologies", "1")
        hydrologies_1based = parse_index_range(hydro_spec)
        num_scenarios = len(hydrologies_1based)

        prob_factors_raw: Optional[str] = self._options.get("probability_factors")
        if prob_factors_raw:
            probability_factors: List[float] = [
                float(f) for f in prob_factors_raw.split(",")
            ]
        else:
            probability_factors = [1.0 / num_scenarios] * num_scenarios

        scenarios: List[Dict[str, Any]] = []
        for i, factor in enumerate(probability_factors):
            scenarios.append(
                {
                    "uid": i + 1,
                    "probability_factor": factor,
                }
            )
        self._simulation["scenario_array"] = scenarios

        method = self._normalize_method(self._options.get("method", "cascade"))
        if method == "monolithic":
            self._simulation["scene_array"] = [
                {"uid": 1, "first_scenario": 0, "count_scenario": num_scenarios}
            ]
        else:
            self._simulation["scene_array"] = [
                {"uid": i + 1, "first_scenario": i, "count_scenario": 1}
                for i in range(num_scenarios)
            ]
