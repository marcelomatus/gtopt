"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
"""

import logging
import time
import zipfile
from pathlib import Path
from typing import Any

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.gtopt_writer import GTOptWriter

logger = logging.getLogger(__name__)


def _log_stats(planning: dict, elapsed: float) -> None:
    """Log conversion statistics similar to gtopt_main log_pre_solve_stats."""
    sys = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    logger.info("=== System statistics ===")
    logger.info("  System name     : %s", sys.get("name", "(unnamed)"))
    logger.info("=== System elements  ===")
    logger.info("  Buses           : %d", len(sys.get("bus_array", [])))
    logger.info("  Generators      : %d", len(sys.get("generator_array", [])))
    logger.info("  Generator profs : %d", len(sys.get("generator_profile_array", [])))
    logger.info("  Demands         : %d", len(sys.get("demand_array", [])))
    logger.info("  Lines           : %d", len(sys.get("line_array", [])))
    logger.info("  Batteries       : %d", len(sys.get("battery_array", [])))
    logger.info("  Converters      : %d", len(sys.get("converter_array", [])))
    logger.info("  Junctions       : %d", len(sys.get("junction_array", [])))
    logger.info("  Waterways       : %d", len(sys.get("waterway_array", [])))
    logger.info("  Reservoirs      : %d", len(sys.get("reservoir_array", [])))
    logger.info("  Turbines        : %d", len(sys.get("turbine_array", [])))
    logger.info("=== Simulation statistics ===")
    logger.info("  Blocks          : %d", len(sim.get("block_array", [])))
    logger.info("  Stages          : %d", len(sim.get("stage_array", [])))
    logger.info("  Scenarios       : %d", len(sim.get("scenario_array", [])))
    logger.info("=== Key options ===")
    logger.info("  use_single_bus  : %s", opts.get("use_single_bus", False))
    logger.info("  scale_objective : %s", opts.get("scale_objective", 1000))
    logger.info("  demand_fail_cost: %s", opts.get("demand_fail_cost", 0))
    logger.info("  input_directory : %s", opts.get("input_directory", "(default)"))
    logger.info("  annual_discount : %s", opts.get("annual_discount_rate", 0.0))
    logger.info("=== Conversion time ===")
    logger.info("  Elapsed         : %.3fs", elapsed)


def create_zip_output(output_file: Path, output_dir: Path, zip_path: Path) -> None:
    """Create a ZIP archive containing the JSON file and all data files.

    The archive layout mirrors what gtopt_guisrv / gtopt_websrv expect:

    - ``{case_name}.json``  at the archive root
    - ``{input_directory}/{subdir}/{file}.parquet`` for data files

    Args:
        output_file: Path to the main JSON configuration file.
        output_dir:  Directory containing Parquet/CSV data files.
        zip_path:    Destination ZIP file path.
    """
    case_name = output_file.stem
    input_dir_name = output_dir.name

    logger.info("Creating ZIP archive: %s", zip_path)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # Add main JSON config at archive root
        zf.write(output_file, arcname=f"{case_name}.json")

        # Add all data files under the input_directory prefix
        for data_file in sorted(output_dir.rglob("*")):
            if data_file.is_file():
                arcname = f"{input_dir_name}/{data_file.relative_to(output_dir)}"
                zf.write(data_file, arcname=arcname)

    logger.info(
        "ZIP archive written: %s (%d bytes)",
        zip_path,
        zip_path.stat().st_size,
    )


def convert_plp_case(options: dict[str, Any]) -> None:
    """Convert PLP input files to GTOPT format.

    Args:
        options: Conversion options dict with keys:
            input_dir, output_dir, output_file, last_stage, last_time,
            compression, hydrologies, probability_factors, discount_rate,
            management_factor, zip_output (optional, default False).

    Raises:
        RuntimeError: If any step of the conversion fails.
    """
    input_dir = Path(options.get("input_dir", "input"))
    if not input_dir.exists():
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. "
            f"Details: Input directory does not exist: '{input_dir}'"
        )

    try:
        t0 = time.monotonic()

        # Parse all files
        logger.info("Parsing PLP input files from: %s", input_dir)
        parser = PLPParser(options)
        parser.parse_all()

        # Convert to GTOPT format and write output
        logger.info("Writing GTOPT output to: %s", options["output_file"])
        writer = GTOptWriter(parser)
        writer.write(options)

        elapsed = time.monotonic() - t0

        # Log conversion statistics
        _log_stats(writer.planning, elapsed)

        output_file = Path(options["output_file"])
        logger.info("Conversion successful! Output written to %s", output_file)

        # Optionally create a ZIP archive
        if options.get("zip_output", False):
            output_dir = Path(options["output_dir"])
            zip_path = output_file.with_suffix(".zip")
            create_zip_output(output_file, output_dir, zip_path)
            print(f"ZIP archive created: {zip_path}")

    except RuntimeError:
        raise
    except FileNotFoundError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Required file not found: {e}"
        ) from e
    except ValueError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Invalid data format: {e}"
        ) from e
    except Exception as e:
        raise RuntimeError(f"PLP to GTOPT conversion failed. Details: {e}") from e
