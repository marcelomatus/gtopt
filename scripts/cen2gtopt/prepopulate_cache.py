#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt.prepopulate_cache`` — pre-populate the cmg-real cache
for many CEN buses without touching cmg-online.

Strategy:

1. Read the **PLEXOS PCP solution XML** (``DBSEN_PRGDIARIO.xml`` /
   ``DBSEN_PRGDIARIO_PID.xml`` already in the local PCP archive
   cache) for the canonical list of CEN node names — the ~247 buses
   PLEXOS uses.
2. **Convert each PLEXOS name to candidate bar_transf strings** with
   a heuristic that handles the common SEN naming conventions
   (CamelCase → uppercase, underscore-pad to 14 chars, append the
   3-digit voltage suffix).  For irregular cases (D.ALMAGRO,
   L.ALMENDROS, A.MELIP) we ship a hand-curated override table.
3. **Fetch each bar_transf via the cached** ``fetch_by_name`` path
   with throttled parallelism (default 4 workers, 1-2 s between
   submissions).  Calls that 404 are silently dropped; successes
   land in
   ``~/.cache/gtopt/cen2gtopt/sip/costo_marginal_real__v4__findByDate/``
   and persist for subsequent runs.
4. **Update the bus catalogue** at
   ``~/.cache/gtopt/cen2gtopt/known_buses.parquet`` so downstream
   tools (``cen_buses list``, ``compute_marginal_units_for_day_all_buses``)
   see the expanded bus set.

Runs entirely against the cached extractor — no raw API calls.

Examples
--------

::

    # Pre-populate using the PLEXOS XML at the default path
    python -m cen2gtopt.prepopulate_cache --date 2026-04-22

    # With a smaller batch + longer delay (gentler on rate limits)
    python -m cen2gtopt.prepopulate_cache --date 2026-04-22 \\
        --max-workers 2 --delay-ms 2000

    # Limit to first 50 candidates (smoke test)
    python -m cen2gtopt.prepopulate_cache --date 2026-04-22 --limit 50
"""

from __future__ import annotations

import argparse
import logging
import re
import sys
import time
from pathlib import Path


from cen2gtopt._bus_catalogue import (
    DEFAULT_CATALOGUE_PATH,
    discover_buses_from_cache,
    load_catalogue,
    merge_catalogues,
    save_catalogue,
)
from cen2gtopt._cached_extractor import fetch_by_name
from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt.marginal_units import SIP_KEY

_LOG = logging.getLogger("cen2gtopt.prepopulate_cache")


#: Hand-curated PLEXOS-name → bar_transf overrides for the irregular
#: abbreviations CEN uses in cmg-real but not in PLEXOS object names.
PLEXOS_TO_BAR_TRANSF: dict[str, str] = {
    "AMelipilla220": "A.MELIP_______220",
    "DAlmagro110": "D.ALMAGRO_____110",
    "DAlmagro220": "D.ALMAGRO_____220",
    "Almendros110": "L.ALMENDROS___110",
    "Almendros220": "L.ALMENDROS___220",
    "ElSalto110": "EL_SALTO______110",
    "ElSalto220": "EL_SALTO______220",
    "ElMaiten066": "ELMAITEN______066",
    "PAzucar220": "P.AZUCAR______220",
    "PAzucar500": "P.AZUCAR______500",
    "PAltoCmpc110": "P.ALTOCMPC____110",
    "AJahuel220": "A.JAHUEL______220",
    "AJahuel500": "A.JAHUEL______500",
    "AJahuel110": "A.JAHUEL______110",
    "AJahuel154": "A.JAHUEL______154",
    "ASanta110": "A.SANTA_______110",
    "ASanta220": "A.SANTA_______220",
    "CNavia110": "C.NAVIA_______110",
    "CNavia220": "C.NAVIA_______220",
    "PCortes154": "P.CORTES______154",
    "PMontt220": "P.MONTT_______220",
    "S-AA100": "S-AA__________100",
    "TO_Enlace220": "TO_ENLACE_____220",
    "Pid-Pid110": "PID-PID_______110",
    "TAP_OFF_LLANOS220": "TAP_OFF_LLANOS220",
}

#: PLEXOS XML namespace.
NS = "{http://tempuri.org/MasterDataSet.xsd}"
CLASS_NODE = 22


def _heuristic_bar_transf(plexos_name: str) -> str | None:
    """Convert a PLEXOS node name to a candidate bar_transf.

    Strips the trailing 3-digit voltage suffix, uppercases the name
    part, pads with underscores to 14 chars, then appends the
    voltage.  17 chars total.

    Returns ``None`` when the trailing voltage suffix is absent.
    """
    m = re.match(r"(.+?)(\d{3})$", plexos_name)
    if not m:
        return None
    name, voltage = m.group(1), m.group(2)
    name_upper = name.upper()
    if len(name_upper) > 14:
        return None  # name too long — likely needs hand-curation
    name_padded = (name_upper + "_" * 14)[:14]
    return f"{name_padded}{voltage}"


def extract_plexos_node_names(xml_path: Path) -> list[str]:
    """Return all unique Node-class object names from a PLEXOS XML."""
    # pylint: disable=import-outside-toplevel
    import xml.etree.ElementTree as ET

    tree = ET.parse(xml_path)
    root = tree.getroot()
    names: set[str] = set()
    for obj in root.findall(f"{NS}t_object"):
        cid = obj.findtext(f"{NS}class_id")
        if cid is None or int(cid) != CLASS_NODE:
            continue
        name = obj.findtext(f"{NS}name")
        if name:
            names.add(name)
    return sorted(names)


def candidate_bar_transfs(plexos_names: list[str]) -> list[str]:
    """Map every PLEXOS node name to a bar_transf candidate.
    Hand-curated overrides win over the heuristic."""
    out: list[str] = []
    for n in plexos_names:
        if n in PLEXOS_TO_BAR_TRANSF:
            out.append(PLEXOS_TO_BAR_TRANSF[n])
            continue
        bt = _heuristic_bar_transf(n)
        if bt is not None:
            out.append(bt)
    return sorted(set(out))


def populate_one(client: CenApiClient, *, date: str, bar_transf: str) -> int:
    """Fetch one (date, bar_transf) via the cached extractor.
    Returns the number of rows fetched (or already cached)."""
    try:
        df = fetch_by_name(
            client,
            "cmg_real",
            start=date,
            extra_params={"bar_transf": bar_transf},
        )
    except Exception:  # noqa: BLE001  # pylint: disable=broad-except
        return 0
    return len(df)


def find_plexos_xml(extract_root: Path | None = None) -> Path | None:
    """Locate the most-recent PLEXOS XML in the local PCP archive."""
    extract_root = extract_root or (
        Path.home() / ".cache" / "gtopt" / "cen2gtopt" / "pcp_archive" / "_unpacked"
    )
    if not extract_root.exists():
        return None
    candidates = sorted(
        list(extract_root.glob("PCP_*/DBSEN_PRGDIARIO*.xml"))
        + list(extract_root.glob("PID_*/Modelos/DBSEN_PRGDIARIO*.xml"))
        + list(extract_root.glob("PID_*/*/Modelos/DBSEN_PRGDIARIO*.xml"))
    )
    return candidates[-1] if candidates else None


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="cen2gtopt.prepopulate_cache",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--date", required=True, help="YYYY-MM-DD")
    p.add_argument(
        "--xml",
        type=Path,
        default=None,
        help="Path to DBSEN_PRGDIARIO XML (auto-discovered if omitted)",
    )
    p.add_argument(
        "--max-workers",
        type=int,
        default=4,
        help="Parallel cache-populate threads (default 4 — "
        "gentle on CEN's rate limiter)",
    )
    p.add_argument(
        "--delay-ms",
        type=int,
        default=500,
        help="Delay between submissions (ms; default 500).",
    )
    p.add_argument(
        "--limit", type=int, default=None, help="Cap candidates (smoke testing)"
    )
    p.add_argument(
        "--bar-transfs", help="Comma-separated explicit list of bar_transfs to populate"
    )
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO if args.verbose else logging.WARNING,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    # Resolve the bar_transf candidate list
    if args.bar_transfs:
        candidates = sorted(
            {bt.strip() for bt in args.bar_transfs.split(",") if bt.strip()}
        )
        print(f"using {len(candidates)} explicit bar_transfs")
    else:
        xml = args.xml or find_plexos_xml()
        if xml is None or not xml.exists():
            print(
                "ERROR: no PLEXOS XML found.  Pass --xml or run "
                "`python -m cen2gtopt.pcp_archive download "
                "--name PLEXOS<date>.zip` first."
            )
            return 2
        print(f"reading PLEXOS nodes from {xml}")
        nodes = extract_plexos_node_names(xml)
        print(f"  {len(nodes)} PLEXOS Node objects")
        candidates = candidate_bar_transfs(nodes)
        print(f"  {len(candidates)} bar_transf candidates after heuristic + overrides")

    if args.limit:
        candidates = candidates[: args.limit]
        print(f"  capped to first {args.limit}")

    # Filter out already-cached buses (skip work)
    existing = {r["bar_transf"] for _, r in discover_buses_from_cache().iterrows()}
    todo = [bt for bt in candidates if bt not in existing]
    print(f"  already cached: {len(candidates) - len(todo)}; to fetch: {len(todo)}")

    if not todo:
        print("nothing to do — cache already populated.")
        return 0

    # Sequential populate with adaptive throttle.  Per-bus fetches
    # against the same gateway used to be parallel via
    # ``ThreadPoolExecutor``; in practice that triggered the 3scale
    # rate-limiter and lost buses silently on 429 storms.  A
    # sequential pass is slower but deterministic and observable;
    # the inter-request delay backs off on 429 / 5xx and decays back
    # toward ``--delay-ms`` after sustained successes.
    cfg = CenApiConfig(user_keys={"sip": SIP_KEY}, verify_tls=False)
    n_ok = n_empty = 0
    rows_total = 0
    failed: list[str] = []
    base_delay = max(50.0, float(args.delay_ms))
    max_delay = max(base_delay * 16, 4000.0)
    delay_ms = base_delay
    consecutive_ok = 0
    with CenApiClient(cfg) as client:
        for n_done, bt in enumerate(todo, 1):
            try:
                n_rows = populate_one(client, date=args.date, bar_transf=bt)
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
                _LOG.warning("%s: %s", bt, exc)
                n_rows = 0
                failed.append(bt)
                consecutive_ok = 0
                delay_ms = min(max_delay, delay_ms * 2.0)
            else:
                if n_rows > 0:
                    n_ok += 1
                    rows_total += n_rows
                    consecutive_ok += 1
                    if consecutive_ok >= 5 and delay_ms > base_delay:
                        delay_ms = max(base_delay, delay_ms * 0.7)
                else:
                    n_empty += 1
                    consecutive_ok = 0
            if n_done % 25 == 0 or n_done == len(todo):
                print(
                    f"  [{n_done}/{len(todo)}]  ok={n_ok} empty={n_empty} "
                    f"failed={len(failed)} total_rows={rows_total:,} "
                    f"delay={delay_ms:.0f}ms"
                )
            time.sleep(delay_ms / 1000.0)
        if failed:
            print(f"  retry pass: {len(failed)} bus(es) at ceiling delay")
            for bt in failed:
                try:
                    n_rows = populate_one(client, date=args.date, bar_transf=bt)
                    if n_rows > 0:
                        n_ok += 1
                        rows_total += n_rows
                except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
                    _LOG.warning("retry %s: %s", bt, exc)
                time.sleep(max_delay / 1000.0)

    # Refresh the catalogue from the now-richer cmg-real cache
    print()
    new_cat = discover_buses_from_cache()
    existing_cat = load_catalogue()
    merged = merge_catalogues(existing_cat, new_cat)
    save_catalogue(merged, DEFAULT_CATALOGUE_PATH)
    print(
        f"catalogue updated: {len(existing_cat)} → {len(merged)} buses "
        f"→ {DEFAULT_CATALOGUE_PATH}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
