# -*- coding: utf-8 -*-

"""Time-axis mixin for :class:`plp2gtopt.gtopt_writer.GTOptWriter`.

Holds stage / block / phase / scenario / aperture / indhor (block-hour
map) construction.  Methods rely on ``self._normalize_method`` (provided
by the host class) and on PLP parsed data attached to ``self.parser``.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict

from .aperture_writer import (
    build_aperture_array,
    build_phase_apertures,
    write_aperture_afluents,
)
from .block_writer import BlockWriter
from .index_utils import parse_index_range, parse_stages_phase
from .indhor_writer import IndhorWriter
from .stage_writer import StageWriter


class TimeMixin:
    """Stage / phase / scenario / aperture / indhor processing."""

    parser: Any
    planning: Dict[str, Dict[str, Any]]

    # ``_normalize_method`` is provided by the host class
    # (:class:`plp2gtopt.gtopt_writer.GTOptWriter`) — reachable via MRO.

    def process_stage_blocks(self, options):
        """Calculate first_block and count_block for stages, and build phase_array.

        Phase assignment priority (highest to lowest):

        1. **``stages_phase``** (explicit): A parsed ``--stages-phase`` spec
           (list-of-lists of 1-based PLP stage indices) fully controls the
           mapping regardless of ``method``.
        2. **``method='monolithic'``**: A single phase spanning all stages.
        3. **``method='sddp'``** (default): One phase per PLP stage.

        The ``--stages-phase`` option accepts a string like
        ``"1:4,5,6,7,8,9,10,..."`` (see :func:`~.index_utils.parse_stages_phase`
        for the full syntax).
        """
        stage_parser = self.parser.parsed_data.get("stage_parser", [])
        block_parser = self.parser.parsed_data.get("block_parser", [])

        stages = stage_parser.items
        for stage in stages:
            stage_blocks = [
                index
                for index, block in enumerate(block_parser.items)
                if block["stage"] == stage["number"]
            ]
            stage["first_block"] = stage_blocks[0] if stage_blocks else -1
            stage["count_block"] = len(stage_blocks) if stage_blocks else -1

        self.planning["simulation"]["block_array"] = BlockWriter(
            block_parser=block_parser, options=options
        ).to_json_array()

        self.planning["simulation"]["stage_array"] = StageWriter(
            stage_parser=stage_parser, block_parser=block_parser, options=options
        ).to_json_array(stages)

        num_stages = len(self.planning["simulation"]["stage_array"])
        stages_phase_spec = options.get("stages_phase", None)

        if stages_phase_spec:
            # Explicit stages-phase mapping: parse the spec and build phases.
            # The spec uses 1-based PLP stage indices; convert to 0-based
            # first_stage for gtopt.
            phase_groups = (
                stages_phase_spec
                if isinstance(stages_phase_spec, list)
                else parse_stages_phase(stages_phase_spec, num_stages)
            )
            phase_array = []
            for uid, group in enumerate(phase_groups, start=1):
                # group is a list of 1-based stage indices; convert to
                # 0-based and find the first and count for the contiguous run.
                first_stage_0 = group[0] - 1
                phase_array.append(
                    {
                        "uid": uid,
                        "first_stage": first_stage_0,
                        "count_stage": len(group),
                    }
                )
            self.planning["simulation"]["phase_array"] = phase_array
        else:
            method = self._normalize_method(options.get("method", "sddp"))
            if method == "monolithic":
                # One phase covering all stages
                self.planning["simulation"]["phase_array"] = [
                    {
                        "uid": 1,
                        "first_stage": 0,
                        "count_stage": num_stages,
                    }
                ]
            else:
                # SDDP: one phase per PLP stage, enabling per-stage state
                # variables (Stochastic Dual Dynamic Programming).
                self.planning["simulation"]["phase_array"] = [
                    {
                        "uid": i + 1,
                        "first_stage": i,
                        "count_stage": 1,
                    }
                    for i in range(num_stages)
                ]

    def process_scenarios(self, options):
        """Process scenario data to include block and stage information.

        **Hydrology index convention (Fortran 1-based)**

        The ``-y`` / ``hydrologies`` option uses **raw 1-based hydrology class
        indices** — the column numbers in ``plpaflce.dat``.  For example,
        ``-y 51,52`` selects hydrology classes 51 and 52 directly from the
        plpaflce flow matrix, regardless of what ``plpidsim.dat`` maps to.

        The special value ``"all"`` selects the active hydrology classes:

        * When ``plpidsim.dat`` is present: the union of all hydrology classes
          referenced by any simulation (using the stage-1 mapping), preserving
          the order in which simulations are listed in the file.
        * When ``plpidsim.dat`` is absent: all hydrology columns (1..N_hydro)
          from ``plpaflce.dat``.

        Use ``plp2gtopt --info -i <input_dir>`` to see which hydrology classes
        are active and what the idsim mapping looks like.

        **Internal representation**

        The 0-based hydrology index (``hydro_1based - 1``) is stored in each
        scenario's ``"hydrology"`` field so that
        :class:`~.aflce_writer.AflceWriter` can look up the correct column
        from the flow matrix.  The C++ solver does not use this field.
        """
        # ---------------------------------------------------------------
        # Resolve the '-y' / 'hydrologies' spec to 1-based hydro indices
        # ---------------------------------------------------------------
        idsim_parser = self.parser.parsed_data.get("idsim_parser")
        hydro_spec = options.get("hydrologies", "all")
        spec = (hydro_spec or "all").strip().lower()

        # Helper: collect all available hydrology indices from PLP data
        def _all_hydro_indices() -> list:
            if idsim_parser is not None and idsim_parser.num_simulations > 0:
                indices: list = []
                seen_h: set = set()
                for sim_idx in range(idsim_parser.num_simulations):
                    h = idsim_parser.get_index(sim_idx, 1)
                    if h is not None and h not in seen_h:
                        seen_h.add(h)
                        indices.append(h)
                return indices
            aflce = self.parser.parsed_data.get("aflce_parser")
            if aflce and aflce.items:
                num_hydro = aflce.items[0].get("num_hydrologies", 1)
            else:
                num_hydro = 1
            return list(range(1, num_hydro + 1))

        if spec in ("all", "first"):
            all_hydros = _all_hydro_indices()
            if spec == "first":
                hydro_indices_1based = all_hydros[:1]
            else:
                hydro_indices_1based = all_hydros
        else:
            # Explicit 1-based raw hydrology column indices (Fortran convention).
            # "-y 55,56" = hydrology classes 55 and 56 from plpaflce.dat.
            # No idsim remapping: the user specifies the hydrology numbers directly.
            hydro_indices_1based = parse_index_range(hydro_spec)

            # Validate requested indices against available hydrologies
            all_hydros = _all_hydro_indices()
            available_set = set(all_hydros)
            invalid = sorted(set(hydro_indices_1based) - available_set)
            if invalid:
                raise ValueError(
                    f"Invalid hydrology indices: {invalid}. "
                    f"Available (1-based): "
                    f"{', '.join(str(h) for h in all_hydros)}"
                )

        num_scenarios = len(hydro_indices_1based)

        # Equal probability unless explicitly overridden
        probability_factors = options.get("probability_factors", None)
        if probability_factors is None or len(probability_factors) == 0:
            probability_factors = [1.0 / num_scenarios] * num_scenarios
        else:
            probability_factors = [
                float(factor) for factor in probability_factors.split(",")
            ]

        scenarios = []
        for hydro_1based, factor in zip(hydro_indices_1based, probability_factors):
            # Scenario UID = Fortran 1-based hydrology index (PLP convention).
            # This keeps -y values, scenario UIDs, and aperture source_scenario
            # references all using the same numbering.
            hydro_0based = hydro_1based - 1
            scenarios.append(
                {
                    "uid": hydro_1based,
                    "probability_factor": factor,
                    "hydrology": hydro_0based,
                }
            )
        self.planning["simulation"]["scenario_array"] = scenarios

        method = self._normalize_method(options.get("method", "sddp"))

        if method == "monolithic":
            scenes = [
                {
                    "uid": 1,
                    "first_scenario": 0,
                    "count_scenario": num_scenarios,
                }
            ]
        else:
            scenes = [
                {
                    "uid": i + 1,
                    "first_scenario": i,
                    "count_scenario": 1,
                }
                for i in range(num_scenarios)
            ]
        self.planning["simulation"]["scene_array"] = scenes

    def process_apertures(self, options):
        """Build aperture_array from parsed PLP aperture index files.

        When ``plpidap2.dat`` (or ``plpidape.dat``) is present, the aperture
        definitions are converted to a gtopt ``aperture_array`` where each
        aperture references a ``source_scenario`` by UID.

        If the aperture references hydrologies that are *not* in the
        forward-scenario set, an ``aperture_directory`` is created with the
        extra affluent Parquet files, and the ``sddp_aperture_directory``
        option is set accordingly.

        Aperture configuration is fully handled through aperture_array and
        per-phase apertures in the simulation section.
        """
        if not options:
            return

        idap2_parser = self.parser.parsed_data.get("idap2_parser", None)
        idape_parser = self.parser.parsed_data.get("idape_parser", None)

        if idap2_parser is None and idape_parser is None:
            return

        # Build map: 0-based hydrology index → gtopt scenario UID
        scenarios = self.planning["simulation"].get("scenario_array", [])
        scenario_hydro_map: dict = {}
        forward_hydros: set = set()
        for scen in scenarios:
            hydro_0based = scen.get("hydrology")
            if hydro_0based is not None:
                scenario_hydro_map[hydro_0based] = scen["uid"]
                forward_hydros.add(hydro_0based)

        num_stages = len(self.planning["simulation"].get("stage_array", []))
        max_scenario_uid = max((s["uid"] for s in scenarios), default=0)

        output_dir = Path(options.get("output_dir", ""))
        aperture_dir = output_dir / "apertures"

        result = build_aperture_array(
            idap2_parser=idap2_parser,
            scenario_hydro_map=scenario_hydro_map,
            num_stages=num_stages,
            max_scenario_uid=max_scenario_uid,
            aperture_directory=str(aperture_dir),
        )

        if not result.aperture_array:
            return

        self.planning["simulation"]["aperture_array"] = result.aperture_array

        # Do NOT add aperture-only scenarios to scenario_array — the C++
        # solver builds full LP and output for every scenario in the array.
        # Aperture-only scenarios are served from the aperture_directory;
        # the C++ aperture solver skips source_scenarios it can't find
        # (with an info log) and uses fallback Benders cuts instead.

        # Build hydro→uid map for the extra scenarios (used by parquet writer)
        hydro_uid_map: dict[int, int] = {}
        for es in result.extra_scenarios:
            hydro_uid_map[es["hydrology"]] = es["uid"]

        # Determine which aperture hydros are NOT in the forward set.
        extra_hydros = set(hydro_uid_map.keys())

        # Write aperture-specific affluent data if needed
        if extra_hydros:
            aperture_dir.mkdir(parents=True, exist_ok=True)

            aflce_parser = self.parser.parsed_data.get("aflce_parser", None)
            central_parser = self.parser.parsed_data.get("central_parser", None)
            block_parser = self.parser.parsed_data.get("block_parser", None)

            write_aperture_afluents(
                aflce_parser=aflce_parser,
                central_parser=central_parser,
                block_parser=block_parser,
                aperture_hydros=sorted(extra_hydros),
                forward_hydros=forward_hydros,
                output_dir=aperture_dir,
                options=options,
                hydro_uid_map=hydro_uid_map,
            )

            # Set aperture_directory in sddp_options
            sddp_opts = self.planning["options"].get("sddp_options", {})
            # Use a path relative to the JSON file location (same as
            # input_directory convention).  When the JSON is in output_dir,
            # the aperture directory is just "apertures".
            output_file = Path(options.get("output_file", ""))
            if output_file.parent == output_dir:
                sddp_opts["aperture_directory"] = "apertures"
            else:
                sddp_opts["aperture_directory"] = str(aperture_dir)
            self.planning["options"]["sddp_options"] = sddp_opts

        # NOTE: num_apertures is NOT emitted in sddp_options — the C++
        # SddpOptions JSON contract has no such field.  The aperture count is
        # fully determined by aperture_array and per-phase apertures.

        # Populate per-phase apertures from stage-indexed PLP data
        phase_array = self.planning["simulation"].get("phase_array", [])
        build_phase_apertures(
            idap2_parser=idap2_parser,
            aperture_array=result.aperture_array,
            phase_array=phase_array,
            num_stages=num_stages,
        )

    def process_indhor(self, options):
        """Write block-to-hour map from indhor.csv if present, and record in JSON.

        When the PLP input directory contains ``indhor.csv``, plp_parser will
        have created an ``IndhorParser`` in ``parsed_data["indhor_parser"]``.
        This method writes the data to ``BlockHourMap/block_hour_map.parquet``
        and adds ``"block_hour_map"`` to the simulation section so that
        post-processing tools can reconstruct hourly time-series from
        block-level solver output.

        The ``block_hour_map`` value is the stem path (without file extension)
        relative to the output directory; gtopt resolves the actual format
        (parquet or csv) from the ``input_format`` option.
        """
        indhor_parser = self.parser.parsed_data.get("indhor_parser", None)
        if indhor_parser is None or indhor_parser.is_empty:
            return

        output_dir = Path(options["output_dir"]) if options else Path("results")
        block_hour_dir = output_dir / IndhorWriter.SUBDIR

        writer = IndhorWriter(indhor_parser, options)
        rel_path = writer.to_parquet(block_hour_dir)
        if rel_path:
            self.planning["simulation"]["block_hour_map"] = rel_path
