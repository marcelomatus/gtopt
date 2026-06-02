"""CLI entry point for the NREL-118 → gtopt converter.

Usage:

    python -m nrel118_to_gtopt --week 2 -o nrel118_week2.json

If the NREL-118 source CSVs are absent from ``~/.cache/gtopt/nrel118/``
the converter downloads them from the NREL-Sienna
``PowerSystemsTestData`` GitHub repo (one-time, ~150 KB).
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

from nrel118_to_gtopt._converter import convert, to_gtopt_json, write_json

GITHUB_RAW_BASE = (
    "https://raw.githubusercontent.com/NREL-Sienna/PowerSystemsTestData/master/118-Bus"
)
CACHE_FILES = (
    "Generators.csv",
    "Buses.csv",
    "Lines.csv",
    "Load/DA/LoadR1DA.csv",
    "Load/DA/LoadR2DA.csv",
    "Load/DA/LoadR3DA.csv",
)


def _local_name_for(remote: str) -> str:
    """Map GitHub-relative paths to the flat cache file layout."""

    return remote.rsplit("/", maxsplit=1)[-1]


def populate_cache(cache_dir: Path) -> None:
    """Download missing CSVs into ``cache_dir`` (idempotent)."""

    cache_dir.mkdir(parents=True, exist_ok=True)
    for remote in CACHE_FILES:
        local = cache_dir / _local_name_for(remote)
        if local.exists() and local.stat().st_size > 0:
            continue
        url = f"{GITHUB_RAW_BASE}/{remote}"
        with urllib.request.urlopen(url) as resp, local.open("wb") as out:
            out.write(resp.read())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="nrel118_to_gtopt",
        description=(
            "Convert the NREL-118 (Pena 2017) test system to a gtopt JSON "
            "single-week dispatch fixture with IPCC AR6 emission factors."
        ),
    )
    parser.add_argument(
        "--week",
        type=int,
        default=2,
        help="ISO-week index (1-based) to slice from the annual profiles "
        "(default 2 = winter peak).",
    )
    parser.add_argument(
        "--renewables-share",
        type=float,
        default=0.0,
        help="If > 0, derate thermals by (1−share) and add an aggregate "
        "renewable generator at peak_load × share (e.g. 0.33 for the "
        "33%% wind+solar penetration variant).",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("nrel118.json"),
        help="Output gtopt JSON path.",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path.home() / ".cache" / "gtopt" / "nrel118",
        help="Directory holding cached NREL-118 source CSVs.",
    )
    parser.add_argument(
        "--no-download",
        action="store_true",
        help="Skip the network round-trip; require the cache to be primed.",
    )
    args = parser.parse_args(argv)

    if not args.no_download:
        populate_cache(args.cache_dir)

    conversion = convert(args.cache_dir, week=args.week)
    payload = to_gtopt_json(conversion, renewables_share=args.renewables_share)
    write_json(args.output, payload)

    print(
        f"NREL-118 week {args.week}: {conversion.gen_count} gens / "
        f"{conversion.bus_count} buses / {conversion.line_count} lines / "
        f"{conversion.n_hours} h.  Peak load = "
        f"{max(conversion.total_load_mw):.0f} MW.  "
        f"renewables_share = {args.renewables_share}.  "
        f"Written: {args.output}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
