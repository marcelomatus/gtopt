# -*- coding: utf-8 -*-

"""GTOPT output writer classes.

Handles conversion of parsed PLP data to GTOPT JSON format.
"""

import json
import logging
from pathlib import Path
from typing import Any, Dict, Mapping

from .aflce_writer import AflceWriter
from .aperture_writer import (
    build_aperture_array,
    build_phase_apertures,
    write_aperture_afluents,
)
from .battery_writer import BatteryWriter
from .block_writer import BlockWriter
from .bus_writer import BusWriter
from .central_writer import CentralWriter
from .demand_writer import DemandWriter
from .generator_profile_writer import GeneratorProfileWriter
from .index_utils import parse_index_range, parse_stages_phase
from .indhor_writer import IndhorWriter
from .junction_writer import JunctionWriter
from .line_writer import LineWriter
from .planos_writer import write_boundary_cuts_csv, write_hot_start_cuts_csv
from .plp_parser import PLPParser
from .stage_writer import StageWriter
from .tech_detect import detect_technology, load_centipo_csv

_logger = logging.getLogger(__name__)


def _strip_internal_keys(planning: Dict) -> Dict:
    """Return a shallow copy of ``planning`` with internal-only keys removed.

    The gtopt C++ parser uses ``StrictParsePolicy`` (daw::json
    ``UseExactMappingsByDefault=yes``), so any field not declared in the
    corresponding struct causes a parse error.  Python-side metadata
    (pipeline annotations, Excel hints) is preserved on the writer
    instance but excluded from the emitted JSON.
    """
    return {k: v for k, v in planning.items() if not k.startswith("_")}


class GTOptWriter:
    """Handles conversion of parsed PLP data to GTOPT JSON format."""

    def __init__(self, parser: PLPParser, options=None):
        """Initialize GTOptWriter with a PLPParser instance."""
        self.parser = parser
        self.options = options
        self.output_path = None

        self.planning: Dict[str, Dict[str, Any]] = {
            "options": {},
            "system": {},
            "simulation": {},
        }

    @staticmethod
    def _normalize_method(method: str) -> str:
        """Normalize solver type string.

        Accepts 'sddp', 'mono', 'monolithic', or 'cascade'; returns
        'sddp', 'monolithic', or 'cascade'.
        """
        if method in ("mono", "monolithic"):
            return "monolithic"
        if method == "cascade":
            return "cascade"
        return "sddp"

    @staticmethod
    def _build_default_cascade_options(
        model_opts: dict[str, Any],
        sddp_opts: dict[str, Any],
    ) -> dict[str, Any]:
        """Build a 3-level default cascade configuration.

        Iteration budget split:
          - Level 0 (uninodal):  1/2 of max_iterations — single-bus relaxation
          - Level 1 (transport): 1/4 of max_iterations — lines enabled, no
            losses, no kirchhoff (pure transport model)
          - Level 2 (full):      remaining iterations — full network with the
            user's original model_options

        Each level inherits state-variable targets from the previous level
        via elastic constraints (``inherit_targets = -1``).
        """
        total_iter = sddp_opts.get("max_iterations", 100)
        convergence_tol = sddp_opts.get("convergence_tol", 0.01)

        l0_iter = max(total_iter // 2, 1)
        l1_iter = max(total_iter // 4, 1)
        l2_iter = max(total_iter - l0_iter - l1_iter, 1)

        transition = {
            "inherit_targets": -1,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500.0,
        }

        level_array = [
            {
                "uid": 1,
                "name": "uninodal",
                "model_options": {
                    "use_single_bus": True,
                },
                "sddp_options": {
                    "max_iterations": l0_iter,
                    "convergence_tol": convergence_tol,
                },
            },
            {
                "uid": 2,
                "name": "transport",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": False,
                    "use_line_losses": False,
                },
                "sddp_options": {
                    "max_iterations": l1_iter,
                    "convergence_tol": convergence_tol,
                },
                "transition": transition,
            },
            {
                "uid": 3,
                "name": "full_network",
                "model_options": {
                    k: v
                    for k, v in model_opts.items()
                    if k
                    in (
                        "use_single_bus",
                        "use_kirchhoff",
                        "use_line_losses",
                        "kirchhoff_threshold",
                        "loss_segments",
                    )
                },
                "sddp_options": {
                    "max_iterations": l2_iter,
                    "convergence_tol": convergence_tol,
                },
                "transition": transition,
            },
        ]

        return {
            "model_options": model_opts,
            "sddp_options": sddp_opts,
            "level_array": level_array,
        }

    def process_options(self, options):
        """Process options data to include input and output paths.

        The solver type is emitted at the top level as ``method`` so that
        the gtopt C++ JSON parser maps it directly to ``PlanningOptions::method``.
        All other SDDP-specific settings are still grouped under the nested
        ``sddp_options`` key.
        """
        if not options:
            options = {}
        discount_rate = options.get("discount_rate", 0.0)
        output_format = options.get("output_format", "parquet")
        input_format = options.get("input_format", output_format)
        compression = options.get("compression", "zstd")
        method = self._normalize_method(options.get("method", "cascade"))

        # Build the nested sddp_options block (all sddp_* fields except method).
        # NOTE: num_apertures is NOT emitted here — the C++ SddpOptions JSON
        # contract has no "num_apertures" field (only "apertures", an array of
        # UIDs).  Aperture configuration is fully handled by aperture_array and
        # per-phase apertures in the simulation section, plus
        # aperture_directory in sddp_options (all set by process_apertures).
        sddp_opts: dict = {}

        cut_sharing_mode = options.get("cut_sharing_mode")
        if cut_sharing_mode is not None:
            sddp_opts["cut_sharing_mode"] = cut_sharing_mode

        max_iter = options.get("max_iterations")
        if max_iter is None:
            # Fall back to PDMaxIte from plpmat.dat if available.
            # PDMaxIte=1 means monolithic (single LP solve) in PLP, so only
            # use values > 1 as SDDP iteration limits.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "max_iterations", 0) > 1:
                    max_iter = plpmat.max_iterations
        if max_iter is not None:
            sddp_opts["max_iterations"] = max_iter

        convergence_tol = options.get("convergence_tol")
        if convergence_tol is None:
            # Fall back to PDError/10 from plpmat.dat; use 0.01 if absent.
            # PLP's PDError is loosely enforced — nobody reads the gap out
            # of PLP, so it's typically set very permissively (e.g. 0.1 =
            # 10%).  Tighten by 10x to get a tolerance that matches what
            # gtopt/SDDP users actually expect.
            parsed = getattr(self.parser, "parsed_data", None)
            if isinstance(parsed, dict):
                plpmat = parsed.get("plpmat_parser")
                if plpmat is not None and getattr(plpmat, "pd_error", 0.0) > 0.0:
                    convergence_tol = plpmat.pd_error / 10.0
            if convergence_tol is None:
                convergence_tol = 0.01
        sddp_opts["convergence_tol"] = convergence_tol

        # Secondary (stationary gap) convergence criterion:
        # When the gap stops improving over a window of iterations, declare
        # convergence even if gap > convergence_tol.
        # Default: stationary_tol = convergence_tol / 10, stationary_window = 4.
        stationary_tol = options.get("stationary_tol", convergence_tol / 10.0)
        sddp_opts["stationary_tol"] = stationary_tol

        stationary_window = options.get("stationary_window", 4)
        sddp_opts["stationary_window"] = stationary_window

        # Cut coefficient tolerances (PLP OptiEPS equivalent).
        # cut_coeff_eps: drop coefficients with |value| < eps (default 1e-8).
        # cut_coeff_max: rescale entire cut when max|coeff| > threshold.
        sddp_opts["cut_coeff_eps"] = options.get("cut_coeff_eps", 1e-8)
        sddp_opts["cut_coeff_max"] = options.get("cut_coeff_max", 1e6)

        # When the JSON file lives inside the output directory (the default),
        # input_directory is "." so paths are relative to the JSON location.
        # When -f places the JSON elsewhere, use the full output_dir path.
        output_dir = Path(options.get("output_dir", ""))
        output_file = Path(options.get("output_file", ""))
        if output_file.parent == output_dir:
            input_dir_val = "."
        else:
            input_dir_val = str(output_dir)

        src_model = options.get("model_options", {})
        model_opts = {
            "use_single_bus": src_model.get("use_single_bus", False),
            "use_kirchhoff": src_model.get("use_kirchhoff", True),
            "demand_fail_cost": src_model.get("demand_fail_cost", 1000),
            "state_fail_cost": src_model.get("state_fail_cost", 1000),
        }
        # Only emit scale_objective if explicitly set (C++ default is 1000).
        if "scale_objective" in src_model:
            model_opts["scale_objective"] = src_model["scale_objective"]
        # Only emit scale_theta if explicitly set (C++ auto_scale_theta
        # computes the optimal value from median line reactance).
        if "scale_theta" in src_model:
            model_opts["scale_theta"] = src_model["scale_theta"]
        if "reserve_fail_cost" in src_model:
            model_opts["reserve_fail_cost"] = src_model["reserve_fail_cost"]
        if "use_line_losses" in src_model:
            model_opts["use_line_losses"] = src_model["use_line_losses"]

        planning_opts: dict[str, Any] = {
            "method": method,
            "input_directory": input_dir_val,
            "input_format": input_format,
            "output_directory": "results",
            "output_format": output_format,
            "output_compression": compression,
            "model_options": model_opts,
            "sddp_options": sddp_opts,
        }

        if method == "cascade":
            planning_opts["cascade_options"] = self._build_default_cascade_options(
                model_opts, sddp_opts
            )

        self.planning["options"] = planning_opts

        # Set annual_discount_rate on the simulation section.
        self.planning["simulation"]["annual_discount_rate"] = discount_rate

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
            method = self._normalize_method(options.get("method", "cascade"))
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

        method = self._normalize_method(options.get("method", "cascade"))

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

    def process_generator_profiles(self, options):
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", [])
        scenarios = self.planning["simulation"]["scenario_array"]

        self.planning["system"]["generator_profile_array"] = GeneratorProfileWriter(
            centrals,
            blocks,
            buses,
            aflces,
            scenarios,
            options,
        ).to_json_array()

    def process_ror_spec(self, options):
        """Resolve ``--ror-as-reservoirs`` once so downstream writers share it.

        Runs before ``process_afluents`` and ``process_junctions`` so that
        the discharge parquet can un-scale promoted pasada inflows and the
        junction writer can re-use the same resolved spec without re-parsing
        the CSV.  Stores two keys on ``options``:

        * ``_ror_spec_resolved``: ``{name: RorSpec}`` from the resolver.
        * ``_pasada_unscale_map``: ``{name: 1.0/production_factor}`` for
          every promoted **pasada** central (serie centrals keep their
          physical flow and are not included).
        """
        from .ror_equivalence_parser import (  # noqa: PLC0415
            pasada_unscale_map,
            resolve_ror_reservoir_spec,
        )

        centrals = self.parser.parsed_data.get("central_parser", None)
        if not centrals:
            return

        cot = getattr(centrals, "centrals_of_type", None)
        if not cot:
            return

        # Match JunctionWriter's item filter: eligible centrals are
        # pasada+serie with bus>0 (and pasada must also be routed to the
        # hydro path — not profile/solar).
        pasada_hydro_names = options.get("_pasada_hydro_names", set())
        items: list[dict[str, Any]] = []
        for c in cot.get("serie", []):
            if c.get("bus", 0) > 0:
                items.append(c)
        for c in cot.get("pasada", []):
            if c.get("bus", 0) > 0 and c["name"] in pasada_hydro_names:
                items.append(c)
        for c in cot.get("embalse", []):
            items.append(c)

        resolved = resolve_ror_reservoir_spec(options, items)
        options["_ror_spec_resolved"] = resolved
        options["_pasada_unscale_map"] = pasada_unscale_map(resolved, items)

        if resolved and options.get("expand_ror", True):
            self._dump_ror_promoted(resolved, options)

    @staticmethod
    def _dump_ror_promoted(
        resolved: Dict[str, Any],
        options: Mapping[str, Any],
    ) -> None:
        """Emit ``ror_promoted.json`` — the ``gtopt_expand ror`` audit artifact.

        Mirrors the schema produced by ``gtopt_expand.ror_expand.
        expand_ror_from_file``: ``{"promoted": [{name, vmax_hm3,
        production_factor, pmax_mw?}, ...]}``.  Skipped when
        ``--no-expand-ror`` is set or no output directory is configured.
        """
        output_dir = options.get("output_dir")
        if not output_dir:
            return

        promoted: list[dict[str, Any]] = []
        for name in sorted(resolved):
            spec = resolved[name]
            entry: dict[str, Any] = {
                "name": name,
                "vmax_hm3": spec.vmax_hm3,
                "production_factor": spec.production_factor,
            }
            if getattr(spec, "pmax_mw", None) is not None:
                entry["pmax_mw"] = spec.pmax_mw
            promoted.append(entry)

        target = Path(output_dir) / "ror_promoted.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, "w", encoding="utf-8") as fh:
            json.dump({"promoted": promoted}, fh, indent=2, sort_keys=False)
            fh.write("\n")
        _logger.info(
            "ror: audit artifact → %s (%d promoted central(s))",
            target.name,
            len(promoted),
        )

    def process_afluents(self, options):
        """Write affluent/discharge Parquet files for Flow elements.

        Excludes:
        - Pasada centrals with bus<=0 (isolated, no turbine).
        - Pasada centrals routed to profile mode (solar/wind) — their
          data is written to GeneratorProfile/ by the profile writer.
        """
        centrals = self.parser.parsed_data.get("central_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", [])
        scenarios = self.planning["simulation"]["scenario_array"]

        # Build set of names to exclude from Flow parquet
        excluded: set[str] = set()
        cot = getattr(centrals, "centrals_of_type", None) if centrals else None
        if cot:
            for c in cot.get("pasada", []):
                if c.get("bus", 0) <= 0:
                    excluded.add(c["name"])
        # Also exclude profile-mode centrals (their data goes to GeneratorProfile/)
        profile_names = options.get("_pasada_profile_names", set())
        excluded.update(profile_names)

        if excluded and aflces:
            aflces_items = [f for f in aflces.flows if f.get("name") not in excluded]
        else:
            aflces_items = None  # use default (all)

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir = output_dir / "Flow"
        output_dir.mkdir(parents=True, exist_ok=True)

        aflce_writer = AflceWriter(
            aflces,
            centrals,
            blocks,
            scenarios,
            options,
            pasada_unscale_map=options.get("_pasada_unscale_map") or None,
        )

        aflce_writer.to_parquet(output_dir, items=aflces_items)

    def process_junctions(self, options):
        """Process generator profile data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", None)
        stages = self.parser.parsed_data.get("stage_parser", None)
        aflces = self.parser.parsed_data.get("aflce_parser", None)
        extracs = self.parser.parsed_data.get("extrac_parser", None)
        manems = self.parser.parsed_data.get("manem_parser", None)
        cenre = self.parser.parsed_data.get("cenre_parser", None)
        cenfi = self.parser.parsed_data.get("cenfi_parser", None)
        filemb = self.parser.parsed_data.get("filemb_parser", None)
        ralco = self.parser.parsed_data.get("ralco_parser", None)
        minembh = self.parser.parsed_data.get("minembh_parser", None)
        jw = JunctionWriter(
            central_parser=centrals,
            stage_parser=stages,
            aflce_parser=aflces,
            extrac_parser=extracs,
            manem_parser=manems,
            cenre_parser=cenre,
            cenfi_parser=cenfi,
            filemb_parser=filemb,
            ralco_parser=ralco,
            minembh_parser=minembh,
            options=options,
        )
        json_junctions = jw.to_json_array()
        # Store names of isolated centrals that were skipped (for reporting)
        skipped = getattr(jw, "_skipped_isolated", [])
        if skipped:
            self.planning["_skipped_isolated"] = skipped

        if not json_junctions:
            return

        for j in json_junctions:
            for key, val in j.items():
                self.planning["system"][key] = val

    def process_water_rights(self, options):
        """Emit Laja / Maule Stage-2 artifacts.

        When ``expand_water_rights`` is True (the default), dispatches
        ``gtopt_expand laja|maule`` in-process against the already-parsed
        config (no ``*_dat.json`` intermediate is written to disk —
        those parser dumps would never be shipped anyway).  The Stage-2
        entities are merged into ``planning["system"]``, companion
        ``laja.pampl`` / ``maule.pampl`` files are written, and
        per-agreement system fragments ``laja_water_rights.json`` /
        ``maule_water_rights.json`` are emitted (these DO go into the
        manifest so gtopt can merge them alongside the main planning
        JSON).

        LNG is deliberately NOT handled here — see ``process_lng``.

        Machicura auto-detection consults both
        ``planning["system"]["reservoir_array"]`` (populated by
        ``process_junctions`` and any ``--ror-as-reservoirs`` promotion)
        AND, as a belt-and-suspenders check, the ``ror_promoted.json``
        audit file written by ``process_ror_spec``.  When ``MACHICURA``
        appears in either set, ``MauleAgreement`` picks the ``embalse``
        template variant; otherwise the ``pasada`` default.  Hand-
        authored fixtures can still pin the variant by setting
        ``cfg["machicura_model"]`` explicitly.
        """
        if not options.get("emit_water_rights", False):
            return

        output_dir = Path(options["output_dir"]) if options.get("output_dir") else None
        if output_dir is None:
            return

        if not options.get("expand_water_rights", True):
            return

        stage_parser = self.parser.parsed_data.get("stage_parser")

        laja_parser = self.parser.parsed_data.get("laja_parser")
        if laja_parser is not None:
            self._expand_laja(laja_parser.config, stage_parser, output_dir)

        maule_parser = self.parser.parsed_data.get("maule_parser")
        if maule_parser is not None:
            cfg = dict(maule_parser.config)
            extrac_parser = self.parser.parsed_data.get("extrac_parser")
            if extrac_parser is not None:
                cfg["extrac_entries"] = list(extrac_parser.get_all())
            self._expand_maule(cfg, stage_parser, output_dir)

    def process_lng(self, options):
        """Emit LNG Stage-2 expansion.

        Independent of ``process_water_rights``: when ``expand_lng`` is
        True (the default), dispatches ``gtopt_expand lng`` against the
        already-parsed config and merges the resulting
        ``lng_terminal_array`` into ``planning["system"]``.  No
        intermediate ``lng_dat.json`` is written — parser dumps are
        never shipped.
        """
        if not options.get("emit_water_rights", False):
            return
        if not options.get("expand_lng", True):
            return

        gnl_parser = self.parser.parsed_data.get("gnl_parser")
        if gnl_parser is None:
            return

        self._expand_lng(gnl_parser.config)

    def process_pumped_storage(self, options):
        """Emit pumped-storage expansions from ``--pumped-storage FILE[s]``.

        For each config file in ``options["pumped_storage_files"]``,
        runs the ``gtopt_expand.pumped_storage_expand`` transform and
        merges the resulting entities into the planning JSON.  The
        per-unit artifact ``{name}.json`` is written to ``output_dir``
        wrapped as ``{"system": {...}}``.  The ``name`` comes from the
        file's ``"name"`` field (or the filename stem as fallback) and
        drives all emitted element names
        (``hydro_{name}``, ``tur_{name}``, …).

        ``vmin`` / ``vmax`` at ``0`` (or absent) fall back to the upper
        reservoir's ``emin`` / ``emax`` in plpcnfce.dat.  Requires each
        unit's ``lower_reservoir`` to be a reservoir — real embalse or
        RoR-promoted via --ror-as-reservoirs.  Raises on missing
        prerequisites.
        """
        ps_files = options.get("pumped_storage_files") or []
        if not ps_files:
            return

        output_dir = Path(options["output_dir"]) if options.get("output_dir") else None
        if output_dir is None:
            return

        from gtopt_expand.pumped_storage_expand import (  # noqa: PLC0415
            expand_pumped_storage,
        )

        central_parser = self.parser.parsed_data.get("central_parser")
        embalses = (
            central_parser.centrals_of_type.get("embalse", [])
            if central_parser is not None
            else []
        )

        def _plpcnfce_vmin_vmax(
            upper_name: str,
        ) -> tuple[float | None, float | None]:
            for c in embalses:
                if c.get("name") == upper_name:
                    vmin = float(c["emin"]) if "emin" in c else None
                    vmax = float(c["emax"]) if "emax" in c else None
                    return vmin, vmax
            return None, None

        reservoir_names = self._reservoir_names(output_dir)
        reservoirs = self.planning["system"].get("reservoir_array", [])
        reservoirs_list = list(reservoirs) if isinstance(reservoirs, list) else []

        def _resolve(
            c: Dict[str, Any], key: str, fallback: float | None
        ) -> float | None:
            val = c.get(key)
            if val is None or float(val) == 0.0:
                return fallback
            return float(val)

        for idx, params_path in enumerate(ps_files):
            path = Path(params_path)
            with open(path, "r", encoding="utf-8") as fh:
                loaded = json.load(fh)
            if not isinstance(loaded, dict):
                raise ValueError(
                    f"--pumped-storage {path}: expected a JSON object, "
                    f"got {type(loaded).__name__}"
                )
            cfg: Dict[str, Any] = dict(loaded)

            # Unit name: config wins, then filename stem.
            unit_name = str(cfg.get("name") or path.stem).strip()
            if not unit_name:
                raise ValueError(
                    f"--pumped-storage {path}: unit 'name' cannot be empty"
                )
            cfg["name"] = unit_name

            # Backfill vmin/vmax from plpcnfce.dat when the user left
            # them at 0 (or absent).  The upper reservoir drives the
            # PF curve; default to COLBUN for backwards compatibility
            # with the HB Maule workflow.
            upper_name = str(cfg.get("upper_reservoir") or "COLBUN")
            plp_vmin, plp_vmax = _plpcnfce_vmin_vmax(upper_name)

            resolved_vmin = _resolve(cfg, "vmin", plp_vmin)
            resolved_vmax = _resolve(cfg, "vmax", plp_vmax)
            if resolved_vmin is None or resolved_vmax is None:
                raise ValueError(
                    f"pumped-storage '{unit_name}' needs upper reservoir "
                    f"'{upper_name}' vmin/vmax: not provided in "
                    f"{path} and no '{upper_name}' embalse found in "
                    f"plpcnfce.dat"
                )
            cfg["vmin"] = resolved_vmin
            cfg["vmax"] = resolved_vmax

            entities = expand_pumped_storage(
                config=cfg,
                name=unit_name,
                reservoirs=reservoirs_list,
                reservoir_names=reservoir_names,
                uid_start=900_000 + idx * 16,
            )

            target = output_dir / f"{unit_name}.json"
            with open(target, "w", encoding="utf-8") as fh:
                json.dump({"system": entities}, fh, indent=2, sort_keys=False)
                fh.write("\n")

            self._merge_entities(entities)
            _logger.info(
                "pumped_storage: emitted '%s' + %s.json "
                "(2 waterways, 1 turbine, 1 pump, 1 RPF)",
                unit_name,
                unit_name,
            )

    def _merge_entities(self, entities: Mapping[str, Any]) -> None:
        """Merge gtopt_expand entity arrays into ``planning["system"]``.

        ``*_array`` keys are appended (so Laja and Maule can contribute
        to the same ``flow_right_array`` / ``volume_right_array`` /
        ``user_constraint_array``).  Singular ``user_constraint_file``
        strings are aggregated into the plural ``user_constraint_files``
        list because each agreement emits its own ``.pampl`` and gtopt
        accepts multiple files via that plural field.
        """
        system = self.planning["system"]
        for key, val in entities.items():
            if key == "user_constraint_file":
                system.setdefault("user_constraint_files", []).append(val)
            elif isinstance(val, list) and key.endswith("_array"):
                system.setdefault(key, []).extend(val)
            else:
                system[key] = val

    def _reservoir_names(self, output_dir: Path | None = None) -> set[str]:
        """Return reservoir names currently known to the writer.

        Includes:

        * Names in ``planning["system"]["reservoir_array"]`` (populated
          by ``process_junctions`` and any ``--ror-as-reservoirs``
          promotion).
        * When ``output_dir`` is given and ``ror_promoted.json`` exists
          in it, the promoted names from that audit file.  This covers
          Stage-2-only runs where ``process_junctions`` has not yet
          mutated ``planning["system"]``.
        """
        names = {
            r.get("name", "")
            for r in self.planning["system"].get("reservoir_array", [])
            if r.get("name")
        }
        if output_dir is not None:
            audit = Path(output_dir) / "ror_promoted.json"
            if audit.exists():
                try:
                    data = json.loads(audit.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    data = {}
                for entry in data.get("promoted", []):
                    name = entry.get("name") if isinstance(entry, dict) else None
                    if name:
                        names.add(name)
        return names

    @staticmethod
    def _dump_water_rights_fragment(
        tag: str, entities: Mapping[str, Any], output_dir: Path
    ) -> Path | None:
        """Write a per-agreement ``<tag>_water_rights.json`` system fragment.

        The fragment mirrors the manifest-mergeable structure
        ``{"system": {...entity arrays..., "user_constraint_files": [...]}}``
        so gtopt can load it directly via the planning-file merge path.
        Returns the path written, or None if ``entities`` is empty.
        """
        if not entities:
            return None
        system: Dict[str, Any] = {}
        for key, val in entities.items():
            if key == "user_constraint_file":
                system.setdefault("user_constraint_files", []).append(val)
            elif isinstance(val, list) and key.endswith("_array"):
                system.setdefault(key, []).extend(val)
            else:
                system[key] = val
        target = output_dir / f"{tag}_water_rights.json"
        with open(target, "w", encoding="utf-8") as fh:
            json.dump({"system": system}, fh, indent=2, sort_keys=False)
            fh.write("\n")
        return target

    def _expand_laja(
        self,
        cfg: Mapping[str, Any],
        stage_parser: Any,
        output_dir: Path,
    ) -> Dict[str, Any]:
        """Run the Stage-2 Laja transform, merge entities, return them."""
        from gtopt_expand.laja_agreement import LajaAgreement  # noqa: PLC0415

        agreement = LajaAgreement(dict(cfg), stage_parser=stage_parser)
        entities = agreement.to_json_dict(output_dir=output_dir)
        self._merge_entities(entities)
        self._dump_water_rights_fragment("laja", entities, output_dir)
        _logger.info(
            "laja: expanded to %d flow_right(s), %d volume_right(s)%s"
            " + laja_water_rights.json",
            len(entities.get("flow_right_array", [])),
            len(entities.get("volume_right_array", [])),
            " + laja.pampl" if "user_constraint_file" in entities else "",
        )
        return entities

    def _expand_maule(
        self,
        cfg: Mapping[str, Any],
        stage_parser: Any,
        output_dir: Path,
    ) -> Dict[str, Any]:
        """Run the Stage-2 Maule transform, merge entities, return them."""
        from gtopt_expand.maule_agreement import MauleAgreement  # noqa: PLC0415

        agreement = MauleAgreement(
            dict(cfg),
            stage_parser=stage_parser,
            options={"reservoir_names": self._reservoir_names(output_dir)},
        )
        entities = agreement.to_json_dict(output_dir=output_dir)
        self._merge_entities(entities)
        self._dump_water_rights_fragment("maule", entities, output_dir)
        _logger.info(
            "maule: expanded to %d flow_right(s), %d volume_right(s)%s"
            " + maule_water_rights.json",
            len(entities.get("flow_right_array", [])),
            len(entities.get("volume_right_array", [])),
            " + maule.pampl" if "user_constraint_file" in entities else "",
        )
        return entities

    def _expand_lng(self, cfg: Mapping[str, Any]) -> None:
        """Run the Stage-2 LNG transform and merge ``lng_terminal_array``."""
        from gtopt_expand.lng_expand import expand_lng  # noqa: PLC0415

        num_stages = len(self.planning["simulation"].get("stage_array", []))
        entities = expand_lng(dict(cfg), num_stages=num_stages)
        self._merge_entities(entities)
        _logger.info(
            "lng: expanded to %d terminal(s)",
            len(entities.get("lng_terminal_array", [])),
        )

    def process_flow_turbines(self, options):
        """Create Flow + Turbine(flow=ref) for hydro pasada centrals.

        Only pasada centrals classified as hydro (in
        ``_pasada_hydro_names``) get flow+turbine elements.  Solar/wind
        pasada centrals are handled by ``process_generator_profiles``.
        """
        hydro_names = options.get("_pasada_hydro_names", set())
        if not hydro_names:
            return

        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return

        centrals_of_type = getattr(central_parser, "centrals_of_type", None)
        if not centrals_of_type:
            return
        pasada_centrals = centrals_of_type.get("pasada", [])
        if not pasada_centrals:
            return

        flows = self.planning["system"].setdefault("flow_array", [])
        turbines = self.planning["system"].setdefault("turbine_array", [])

        aflce_parser = self.parser.parsed_data.get("aflce_parser")

        for central in pasada_centrals:
            central_name = central["name"]
            if central_name not in hydro_names:
                continue

            central_id = central["number"]

            # Determine discharge: file ref if aflce data exists, else scalar
            afluent: float | str = central.get("afluent", 0.0)
            if aflce_parser and aflce_parser.get_item_by_name(central_name):
                afluent = "discharge"

            if isinstance(afluent, (int, float)) and afluent == 0.0:
                continue

            # Create Flow element
            flows.append(
                {
                    "uid": central_id,
                    "name": central_name,
                    "discharge": afluent,
                }
            )

            # Create Turbine with flow reference (not waterway)
            turbines.append(
                {
                    "uid": central_id,
                    "name": central_name,
                    "flow": central_name,
                    "generator": central_name,
                    "production_factor": central.get("efficiency", 1.0),
                }
            )

    def classify_pasada_centrals(self, options):
        """Classify pasada centrals by detected technology.

        Populates ``options["_pasada_hydro_names"]`` (set of names for
        hydro run-of-river centrals → flow+turbine mode) and
        ``options["_pasada_profile_names"]`` (set of names for
        solar/wind/renewable centrals → generator profile mode).

        Modes:
        - ``auto`` (default): per-central routing based on detected
          technology.  Solar/wind → profile, hydro → flow+turbine.
        - ``profile``: ALL pasada go to generator profile mode.
        - ``flow-turbine``: ALL pasada go to flow+turbine mode.
        - ``hydro``: ALL pasada go to full hydro topology (junctions).
        """
        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return

        centrals_of_type = getattr(central_parser, "centrals_of_type", None)
        if not centrals_of_type:
            return
        pasada_centrals = centrals_of_type.get("pasada", [])
        if not pasada_centrals:
            return

        pasada_mode = options.get("pasada_mode", "auto")
        active_names = {c["name"] for c in pasada_centrals if c.get("bus", 0) > 0}

        # Global modes: all pasada go to the same path
        if pasada_mode == "profile":
            options["_pasada_hydro_names"] = set()
            options["_pasada_profile_names"] = active_names
            return
        if pasada_mode == "hydro":
            options["_pasada_hydro_names"] = active_names
            options["_pasada_profile_names"] = set()
            return
        if pasada_mode == "flow-turbine":
            options["_pasada_hydro_names"] = active_names
            options["_pasada_profile_names"] = set()
            return

        # Auto mode: per-central routing based on detected technology
        hydro_names: set[str] = set()
        profile_names: set[str] = set()

        user_overrides = options.get("tech_overrides")
        centipo_overrides = (
            load_centipo_csv(options.get("input_dir", ""))
            if options.get("input_dir")
            else {}
        )
        effective_overrides = {**centipo_overrides}
        if user_overrides:
            effective_overrides.update(user_overrides)

        auto_detect = options.get("auto_detect_tech", False)

        # Renewable types that should use generator profile mode
        _profile_types = {"solar", "wind", "csp", "renewable"}

        for central in pasada_centrals:
            if central.get("bus", 0) <= 0:
                continue
            name = central["name"]

            tech = detect_technology(
                "pasada",
                name,
                overrides=effective_overrides,
                auto_detect=auto_detect,
            )

            if tech in _profile_types:
                profile_names.add(name)
                _logger.info("  pasada '%s' → profile mode (tech=%s)", name, tech)
            else:
                hydro_names.add(name)

        options["_pasada_hydro_names"] = hydro_names
        options["_pasada_profile_names"] = profile_names

        if profile_names:
            _logger.info(
                "  pasada routing: %d hydro (flow+turbine), %d renewable (profile)",
                len(hydro_names),
                len(profile_names),
            )

    def process_centrals(self, options):
        """Process central data to include block and stage information."""
        centrals = self.parser.parsed_data.get("central_parser", None)
        stages = self.parser.parsed_data.get("stage_parser", None)
        blocks = self.parser.parsed_data.get("block_parser", None)
        costs = self.parser.parsed_data.get("cost_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        mances = self.parser.parsed_data.get("mance_parser", None)
        self.planning["system"]["generator_array"] = CentralWriter(
            centrals,
            stages,
            blocks,
            costs,
            buses,
            mances,
            options,
        ).to_json_array()

    def _falla_by_bus(self) -> Dict[int, Dict[str, Any]]:
        """Build bus → falla central mapping from central_parser.

        PLP "falla" centrals represent the cost of unserved energy at
        each bus.  When multiple falla centrals sit on the same bus,
        the one with the smallest gcost is chosen (most conservative
        curtailment cost).

        Returns a dict mapping bus number to the chosen falla central dict.
        """
        central_parser = self.parser.parsed_data.get("central_parser")
        if not central_parser:
            return {}

        falla_by_bus: Dict[int, Dict[str, Any]] = {}
        for central in central_parser.centrals:
            if central.get("type") != "falla":
                continue
            bus = central.get("bus", 0)
            if bus <= 0:
                continue
            gcost = central.get("gcost", 0.0)
            prev = falla_by_bus.get(bus)
            if prev is None or gcost < prev.get("gcost", 0.0):
                falla_by_bus[bus] = central

        if falla_by_bus:
            costs = [c.get("gcost", 0.0) for c in falla_by_bus.values()]
            _logger.debug(
                "  falla centrals: %d bus(es) with fcost (range %.2f–%.2f $/MWh)",
                len(falla_by_bus),
                min(costs),
                max(costs),
            )

        return falla_by_bus

    def process_demands(self, options):
        """Process demand data to include block and stage information."""
        demands = self.parser.parsed_data.get("demand_parser", [])
        if not demands:
            return

        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        dems = demands.get_all()
        for demand in dems:
            bus = buses.get_bus_by_name(demand["name"])
            if bus is None:
                demand["bus"] = 0  # mark as unknown; DemandWriter skips bus==0
            else:
                demand["bus"] = bus["number"]

        blocks = self.parser.parsed_data.get("block_parser", [])
        demand_writer = DemandWriter(demands, blocks, options)
        demand_array = demand_writer.to_json_array()

        # Set fcost from falla centrals (bus → min gcost falla)
        falla_by_bus = self._falla_by_bus()
        if falla_by_bus:
            # Write Demand/fcost for fallas with cost schedules
            filed_buses = demand_writer.write_fcost(
                demand_array,
                falla_by_bus,
                self.parser.parsed_data.get("cost_parser"),
                self.parser.parsed_data.get("stage_parser"),
                self.parser.parsed_data.get("central_parser"),
            )
            for dem in demand_array:
                bus = dem.get("bus")
                if bus in falla_by_bus:
                    if bus in filed_buses:
                        dem["fcost"] = "fcost"
                    else:
                        dem["fcost"] = falla_by_bus[bus].get("gcost", 0.0)

        self.planning["system"]["demand_array"] = demand_array

    def process_buses(self):
        """Process bus data to include block and stage information."""
        buses = self.parser.parsed_data.get("bus_parser", [])
        if not buses:
            return

        self.planning["system"]["bus_array"] = BusWriter(buses).to_json_array()

    def process_lines(self, options):
        """Process line data to include block and stage information."""
        lines = self.parser.parsed_data.get("line_parser", [])
        blocks = self.parser.parsed_data.get("block_parser", None)
        manlis = self.parser.parsed_data.get("manli_parser", None)

        self.planning["system"]["line_array"] = LineWriter(
            lines, blocks, manlis, options
        ).to_json_array()

    def process_battery(self, options):
        """Process battery/ESS data and append to existing arrays."""
        battery_parser = self.parser.parsed_data.get("battery_parser", None)
        ess_parser = self.parser.parsed_data.get("ess_parser", None)
        centrals = self.parser.parsed_data.get("central_parser", None)

        # Proceed if any storage source is available
        has_battery = centrals and any(
            c.get("type") == "bateria" for c in centrals.centrals
        )
        if battery_parser is None and ess_parser is None and not has_battery:
            return

        stages = self.parser.parsed_data.get("stage_parser", None)
        buses = self.parser.parsed_data.get("bus_parser", None)
        manbat = self.parser.parsed_data.get("manbat_parser", None)
        maness = self.parser.parsed_data.get("maness_parser", None)

        output_dir = Path(options["output_dir"]) if options else Path("results")

        writer = BatteryWriter(
            battery_parser=battery_parser,
            ess_parser=ess_parser,
            central_parser=centrals,
            bus_parser=buses,
            stage_parser=stages,
            manbat_parser=manbat,
            maness_parser=maness,
            options=options,
        )

        existing_gen = self.planning["system"].get("generator_array", [])
        existing_dem = self.planning["system"].get("demand_array", [])

        result = writer.process(existing_gen, existing_dem, output_dir)

        self.planning["system"]["battery_array"] = result["battery_array"]
        self.planning["system"]["generator_array"] = result["generator_array"]
        self.planning["system"]["demand_array"] = result["demand_array"]
        if writer._clamped_warnings:  # noqa: SLF001
            self.planning.setdefault("_clamped_battery_warnings", []).extend(
                writer._clamped_warnings  # noqa: SLF001
            )
        if "converter_array" in result:
            self.planning["system"]["converter_array"] = result["converter_array"]

        # DCMod=2 regulation reservoirs: append to existing reservoir_array
        reg_reservoirs = result.get("regulation_reservoirs", [])
        if reg_reservoirs:
            existing_rsv = self.planning["system"].get("reservoir_array", [])
            self.planning["system"]["reservoir_array"] = existing_rsv + reg_reservoirs

    @staticmethod
    def _load_alias_file(alias_file: Path | str | None) -> dict[str, str] | None:
        """Load a flat ``{old_name: new_name}`` alias map from JSON.

        Returns ``None`` when ``alias_file`` is ``None``.  Raises
        ``RuntimeError`` if the file is missing, unreadable, or does not
        contain a flat string→string mapping.
        """
        if alias_file is None:
            return None
        path = Path(alias_file)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"Cannot read alias file '{path}': {exc}") from exc
        if not isinstance(data, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in data.items()
        ):
            raise RuntimeError(
                f"Alias file '{path}' must be a flat JSON object of "
                "{string: string} pairs."
            )
        return data

    def process_boundary_cuts(self, options):
        """Write boundary-cut and hot-start-cut CSVs from parsed PLP planos data.

        If the PLP input contained plpplaem/plpplem files, the parsed boundary
        cuts are written to a CSV file in the output directory and the
        ``sddp_boundary_cuts_file`` option is set so that the SDDP solver
        loads them.  CLI options control mode, iteration filtering, and
        whether to export hot-start cuts for intermediate stages.
        """
        planos = self.parser.parsed_data.get("planos_parser")
        if planos is None:
            return

        # Honour --no-boundary-cuts
        if options.get("no_boundary_cuts", False):
            return

        output_dir = Path(options.get("output_dir", ""))
        sddp_opts = self.planning["options"].setdefault("sddp_options", {})
        name_alias = self._load_alias_file(options.get("alias_file"))

        # ── Boundary cuts (last stage) ─────────────────────────────────────
        if planos.cuts:
            csv_path = output_dir / "boundary_cuts.csv"
            write_boundary_cuts_csv(
                planos.cuts,
                planos.reservoir_names,
                csv_path,
                name_alias=name_alias,
            )
            self.planning["_boundary_cuts_count"] = len(planos.cuts)
            self.planning["_boundary_state_variables"] = len(planos.reservoir_names)
            # Path relative to where gtopt runs (same dir as the JSON).
            # When JSON is inside output_dir, input_directory is "." and
            # the file is at "./boundary_cuts.csv".
            # When JSON is outside, input_directory is the output_dir path,
            # so use "{input_directory}/boundary_cuts.csv".
            input_dir_val = self.planning["options"].get("input_directory", ".")
            if input_dir_val == ".":
                sddp_opts["boundary_cuts_file"] = "boundary_cuts.csv"
            else:
                sddp_opts["boundary_cuts_file"] = str(
                    Path(input_dir_val) / "boundary_cuts.csv"
                )

        # Wire mode and max-iterations options through to the JSON
        bc_mode = options.get("boundary_cuts_mode")
        if bc_mode is not None:
            sddp_opts["boundary_cuts_mode"] = bc_mode

        bc_max_iter = options.get("boundary_max_iterations")
        if bc_max_iter is not None:
            sddp_opts["boundary_max_iterations"] = bc_max_iter

        # ── Hot-start cuts (intermediate stages) ───────────────────────────
        # Always export hot-start cuts when non-boundary cuts exist, so they
        # are available in the gtopt input directory.  Loading is disabled by
        # default; pass --hot-start-cuts to enable named_cuts_file.
        non_boundary = [
            c for c in planos.all_cuts if c["stage"] != planos.boundary_stage
        ]
        if non_boundary:
            hs_path = output_dir / "hot_start_cuts.csv"
            # Build stage→phase mapping from the planning structure
            stage_to_phase = self._build_stage_to_phase_map()
            write_hot_start_cuts_csv(
                non_boundary,
                planos.reservoir_names,
                hs_path,
                stage_to_phase=stage_to_phase,
                name_alias=name_alias,
            )
            # Only wire the file into the JSON options if explicitly requested
            if options.get("hot_start_cuts", False):
                if input_dir_val == ".":
                    sddp_opts["named_cuts_file"] = "hot_start_cuts.csv"
                else:
                    sddp_opts["named_cuts_file"] = str(
                        Path(input_dir_val) / "hot_start_cuts.csv"
                    )

    def _build_stage_to_phase_map(self) -> dict[int, int] | None:
        """Build a mapping from PLP stage (1-based) to gtopt phase UID.

        Uses the stage_array already set in the planning JSON.
        Returns ``None`` if no mapping can be built (which makes
        ``write_hot_start_cuts_csv`` use identity mapping).
        """
        raw_stages: Any = self.planning.get("stage_array", [])
        stage_array: list[dict[str, Any]] = (
            raw_stages if isinstance(raw_stages, list) else []
        )
        if not stage_array:
            return None

        stage_to_phase: dict[int, int] = {}
        for stage in stage_array:
            stage_uid: int = stage.get("uid", 0)
            phase_uid: int = stage.get("phase_uid", 0)
            stage_to_phase[stage_uid] = phase_uid

        return stage_to_phase or None

    @staticmethod
    def _load_variable_scales_file(file_path: Path) -> list[dict]:
        """Load variable scales from a JSON file.

        The file must contain a JSON array of objects, each with keys:
        ``class_name``, ``variable``, ``uid``, ``scale``.

        Returns an empty list on any read/parse error (with a warning log).
        """
        logger = logging.getLogger(__name__)
        try:
            with open(file_path, encoding="utf-8") as fh:
                data = json.load(fh)
            if not isinstance(data, list):
                logger.warning(
                    "variable-scales-file %s: expected a JSON array, got %s",
                    file_path,
                    type(data).__name__,
                )
                return []
            required_keys = {"class_name", "variable", "uid", "scale"}
            result: list[dict] = []
            for entry in data:
                if not isinstance(entry, dict) or not required_keys <= entry.keys():
                    logger.warning(
                        "variable-scales-file %s: skipping invalid entry %r "
                        "(expected keys: %s)",
                        file_path,
                        entry,
                        ", ".join(sorted(required_keys)),
                    )
                    continue
                result.append(entry)
            return result
        except (OSError, json.JSONDecodeError) as exc:
            logger.warning(
                "variable-scales-file %s: failed to load: %s", file_path, exc
            )
            return []

    def process_variable_scales(self, options):
        """Build ``variable_scales`` entries in the options section.

        Generates VariableScale JSON entries for reservoir energy scaling
        and battery energy scaling, using the ``variable_scales`` mechanism
        in ``PlanningOptions`` rather than per-element fields.

        Scale priority (highest to lowest):
        1. Explicit ``--reservoir-energy-scale`` / ``--battery-energy-scale``.
        2. ``--auto-reservoir-energy-scale`` / ``--auto-battery-energy-scale``.
        3. ``--variable-scales-file`` entries (lowest priority).

        OFF by default — gtopt auto-scales reservoirs and batteries from emax.
        """
        if not options:
            return

        # Auto-scale is the default since per-element energy_scale fields have
        # been removed from the C++ structs. Use variable_scales exclusively.
        has_reservoir = "reservoir_energy_scale" in options or options.get(
            "auto_reservoir_energy_scale", False
        )
        has_battery = "battery_energy_scale" in options or options.get(
            "auto_battery_energy_scale", False
        )
        has_file = "variable_scales_file" in options

        if not has_reservoir and not has_battery and not has_file:
            return

        # --- Load file-based scales first (lowest priority) ---
        file_scales: list[dict] = []
        if has_file:
            file_path = options["variable_scales_file"]
            file_scales = self._load_variable_scales_file(Path(file_path))

        # Build a lookup of (class_name, variable, uid) → scale from the file
        # so we can skip file entries that are overridden by auto/explicit.
        file_scale_map: dict[tuple[str, str, int], float] = {}
        for entry in file_scales:
            key = (entry["class_name"], entry["variable"], entry["uid"])
            file_scale_map[key] = entry["scale"]

        # Track which (class_name, variable, uid) are set by auto/explicit
        computed_keys: set[tuple[str, str, int]] = set()

        scales: list[dict] = []

        # --- Reservoir energy scales ---
        if has_reservoir:
            explicit_reservoir: dict = options.get("reservoir_energy_scale", {})
            auto_reservoir = options.get("auto_reservoir_energy_scale", False)

            # Collect FEscala data from planos parser (plpplem1.dat)
            planos = self.parser.parsed_data.get("planos_parser")
            fescala_map: dict = {}
            if planos is not None:
                fescala_map = planos.reservoir_fescala

            # Collect central_parser energy_scale as fallback for auto mode
            central_parser = self.parser.parsed_data.get("central_parser")
            central_energy_scale: dict = {}
            if central_parser is not None:
                for central in central_parser.centrals:
                    if central.get("type") == "embalse" and "energy_scale" in central:
                        central_energy_scale[str(central["name"])] = central[
                            "energy_scale"
                        ]

            reservoirs = self.planning["system"].get("reservoir_array", [])
            for rsv in reservoirs:
                name = rsv["name"]
                uid = rsv["uid"]
                scale = None

                # Priority 1: explicit --reservoir-energy-scale
                if name in explicit_reservoir:
                    scale = explicit_reservoir[name]
                # Priority 2: auto-rsv-energy-scale
                elif auto_reservoir:
                    # Try FEscala from plpplem1.dat first
                    fescala = fescala_map.get(name)
                    if fescala is not None:
                        scale = 10.0 ** (fescala - 6)
                    else:
                        # Fallback: central_parser's energy_scale (Escala/1e6)
                        scale = central_energy_scale.get(name)

                if scale is not None and scale != 1.0:
                    scales.append(
                        {
                            "class_name": "Reservoir",
                            "variable": "energy",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Reservoir", "energy", uid))
                    # Scale flow (extraction) variables: energy is in GWh,
                    # flow is in hm³ ≈ energy/1000, so divide by 1000.
                    scales.append(
                        {
                            "class_name": "Reservoir",
                            "variable": "flow",
                            "uid": uid,
                            "scale": scale / 1000.0,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Reservoir", "flow", uid))

        # --- Battery energy scales ---
        if has_battery:
            explicit_energy: dict = options.get("battery_energy_scale", {})
            auto_energy = options.get("auto_battery_energy_scale", False)

            batteries = self.planning["system"].get("battery_array", [])
            for bat in batteries:
                name = bat["name"]
                uid = bat["uid"]
                scale = None

                # Priority 1: explicit --energy-scale
                if name in explicit_energy:
                    scale = explicit_energy[name]
                # Priority 2: auto-energy-scale → 0.01 for all PLP batteries
                elif auto_energy:
                    scale = 0.01

                if scale is not None and scale != 1.0:
                    scales.append(
                        {
                            "class_name": "Battery",
                            "variable": "energy",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Battery", "energy", uid))
                    # Scale flow (finp/fout) with the same factor so
                    # energy-balance coefficients stay O(1).
                    scales.append(
                        {
                            "class_name": "Battery",
                            "variable": "flow",
                            "uid": uid,
                            "scale": scale,
                            "name": name,
                        }
                    )
                    computed_keys.add(("Battery", "flow", uid))

        # --- Merge file-based scales (lowest priority) ---
        for entry in file_scales:
            key = (entry["class_name"], entry["variable"], entry["uid"])
            if key not in computed_keys and entry["scale"] != 1.0:
                scales.append(entry)

        if scales:
            self.planning["options"]["variable_scales"] = scales

    def to_json(self, options=None) -> Dict:
        """Convert parsed data to GTOPT JSON structure."""
        if options is None:
            options = {}

        progress = options.get("_progress")

        def _step(key: str) -> None:
            if progress is not None:
                progress.step(key)

        _step("options")
        self.process_options(options)
        _step("stages")
        self.process_stage_blocks(options)
        self.process_indhor(options)
        _step("scenarios")
        self.process_scenarios(options)
        self.process_apertures(options)
        _step("buses")
        self.process_buses()
        self.process_lines(options)
        _step("generators")
        self.classify_pasada_centrals(options)
        self.process_centrals(options)
        _step("demands")
        self.process_demands(options)
        _step("hydro")
        self.process_ror_spec(options)
        self.process_afluents(options)
        self.process_generator_profiles(options)
        self.process_junctions(options)
        self.process_flow_turbines(options)
        _step("water_rights")
        self.process_water_rights(options)
        _step("lng")
        self.process_lng(options)
        _step("pumped_storage")
        self.process_pumped_storage(options)
        _step("batteries")
        self.process_battery(options)
        _step("boundary")
        self.process_boundary_cuts(options)
        self.process_variable_scales(options)

        # Organize into planning structure
        name = options.get("name", "plp2gtopt") if options else "plp2gtopt"
        self.planning["system"]["name"] = name

        # Build version string with provenance info
        from plp2gtopt import __version__ as plp2gtopt_version  # noqa: PLC0415

        version = options.get("sys_version", "") if options else ""
        input_dir = options.get("input_dir", "")
        source = Path(input_dir).name if input_dir else ""
        parts = [f"plp2gtopt {plp2gtopt_version}"]
        if source:
            parts.append(f"from {source}")
        if version:
            parts.append(version)
        self.planning["system"]["version"] = ", ".join(parts)

        return self.planning

    def write(self, options=None):
        """Write JSON output to file."""
        if options is None:
            options = {}

        output_dir = Path(options["output_dir"]) if options else Path("results")
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = Path(options["output_file"]) if options else Path("gtopt.json")
        output_file.parent.mkdir(parents=True, exist_ok=True)

        planning = self.to_json(options)
        progress = options.get("_progress")
        if progress is not None:
            progress.step("write")
        with open(output_file, "w", encoding="utf-8") as f:
            json.dump(_strip_internal_keys(planning), f, indent=4)
