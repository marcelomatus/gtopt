# cen2gtopt prototypes (`_legacy/`)

Standalone scripts that predate the polished `cen2gtopt` package.

| File | Purpose | Successor |
|------|---------|-----------|
| `cen_compare_24h.py` | 24-hour real-vs-reconstructed CMG diagnostic against full CEN inputs | `python -m cen2gtopt.marginal_units` (single-day) and `python -m cen2gtopt.marginal_period` (multi-day) |
| `cen_water_values.py` | Per-plant water-value inference from λ\_CEN and dispatch state | Re-implemented per-bus inside the marginal-unit picker (`identify_marginal`) |
| `scvic_download_helper.md` | Manual download instructions for SCVIC consolidado-mensual Excel reports (the SIP `/costo-combustible/v3/findAll` endpoint was retired 2024-12-31) | `cen2gtopt._scvic_loader` reads the downloaded Excel file directly |

Kept for historical reference and as one-off analytical tools.

These are **NOT** part of the installable wheel:

* The `_legacy/` directory has no `__init__.py`, so setuptools does not
  treat it as a sub-package and does not include it in the build.
* Each `*.py` file declares `# ruff: noqa`, `# pylint: skip-file`, and
  `# mypy: ignore-errors`, so CI lint/type checks skip them.

Run them directly with:

```bash
python scripts/cen2gtopt/_legacy/cen_compare_24h.py
python scripts/cen2gtopt/_legacy/cen_water_values.py
```

For new work, use the production pipeline:

```bash
python -m cen2gtopt.marginal_units              # single-day diagnostic
python -m cen2gtopt.marginal_period             # multi-day batch
python -m cen2gtopt.buses list                  # bus catalogue
python -m cen2gtopt.cache fetch --date YYYY-MM-DD
python -m cen2gtopt.pcp_solution lmp --date YYYY-MM-DD
```
