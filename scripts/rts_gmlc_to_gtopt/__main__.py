"""CLI entry point for the RTS-GMLC → gtopt converter.

Usage:

    python -m rts_gmlc_to_gtopt --day 1 -o rts_gmlc_day1.json
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

from rts_gmlc_to_gtopt._converter import convert, to_gtopt_json, write_json

GITHUB_RAW_BASE = (
    "https://raw.githubusercontent.com/GridMod/RTS-GMLC/master/RTS_Data/SourceData"
)
CACHE_FILES = ("gen.csv", "bus.csv", "branch.csv", "reserves.csv")


def populate_cache(cache_dir: Path) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    for name in CACHE_FILES:
        local = cache_dir / name
        if local.exists() and local.stat().st_size > 0:
            continue
        url = f"{GITHUB_RAW_BASE}/{name}"
        with urllib.request.urlopen(url) as resp, local.open("wb") as out:
            out.write(resp.read())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="rts_gmlc_to_gtopt",
        description=(
            "Convert the RTS-GMLC (GridMod 2019) test system to a gtopt JSON "
            "single-day dispatch fixture with per-generator multi-pollutant "
            "emission accounting."
        ),
    )
    parser.add_argument(
        "--day",
        type=int,
        default=1,
        help="Day index used to scale the synthetic 24-hour load shape.",
    )
    parser.add_argument(
        "--pollutants",
        type=str,
        default="co2",
        help="Comma-separated pollutant tags to track (co2,so2,nox,...).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("rts_gmlc.json"),
        help="Output gtopt JSON path.",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache" / "gtopt" / "rts_gmlc",
        help="Directory holding cached RTS-GMLC source CSVs.",
    )
    parser.add_argument(
        "--no-download",
        action="store_true",
        help="Skip the network round-trip; require the cache to be primed.",
    )
    args = parser.parse_args(argv)

    if not args.no_download:
        populate_cache(args.cache_dir)

    pollutants = tuple(p.strip() for p in args.pollutants.split(",") if p.strip())
    conversion = convert(args.cache_dir, day=args.day)
    payload = to_gtopt_json(conversion, include_pollutants=pollutants)
    write_json(args.output, payload)

    print(
        f"RTS-GMLC day {args.day}: {conversion.gen_count} gens / "
        f"{conversion.bus_count} buses / {conversion.line_count} lines / "
        f"{conversion.n_hours} h.  Peak load = "
        f"{max(conversion.total_load_mw):.0f} MW.  "
        f"pollutants = {pollutants}.  Written: {args.output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
