# SCVIC Consolidado-Mensual download helper

The Coordinador Eléctrico Nacional discontinued the SIP REST endpoint
`/costo-combustible/v3/findAll` on **2024-12-31**. Per-unit declared
fuel costs (USD/fuel-unit, declared by each Coordinado under DS 130 /
Norma Técnica) now live in the **SCVIC** platform at
<https://costosvariables.coordinador.cl/> — but its REST API is gated
by NetIQ SSO (Coordinado-only).

The same data is published — at **monthly** granularity — as a public
Excel report on the Coordinador's web portal.

## Download steps

1. Open this URL in a browser (Cloudflare bot-check requires JS):

   <https://www.coordinador.cl/mercados/documentos/costos-variables-de-generacion-y-stock-de-combustible/costos-variables-de-generacion/consolidado-mensual-de-costos-variables-y-costos-de-partida-y-detencion/>

2. The page lists monthly Excel files (one per `YYYY-MM`). Click the
   one whose month contains the date you want to compare against
   (typically lags ~1 month — e.g. for 2026-04-22, the April 2026
   file usually appears in late May 2026).

3. Save the file to the repo, e.g.:

   ```
   tools/scvic_consolidado_2026_04.xlsx
   ```

4. Run the comparison demo with the file:

   ```
   python tools/cen_compare_24h_v2.py \
       --date 2026-04-22 \
       --scvic-excel tools/scvic_consolidado_2026_04.xlsx
   ```

   …or via env var:

   ```
   SCVIC_EXCEL=tools/scvic_consolidado_2026_04.xlsx \
       python tools/cen_compare_24h_v2.py
   ```

## What the parser tolerates

`scripts/cen2gtopt/_scvic_loader.py` auto-detects the table header
in any of the first 30 rows, then maps Spanish-accented column names
onto a canonical schema:

| canonical column            | matches header containing            |
|:----------------------------|:-------------------------------------|
| `central_name`              | `Central`, `Nombre Central`          |
| `configuracion`             | `Configuración`                      |
| `empresa`                   | `Empresa`, `Coordinado`              |
| `tipo_combustible`          | `Combustible`, `Combustible Princ…`  |
| `costo_combustible`         | `Costo Combustible`, `Precio Comb…`  |
| `costo_variable_total`      | `Costo Variable Total`, `CVT`        |
| `costo_variable_no_combust` | `Costo Variable No Combust`, `CVNC`  |
| `costo_partida`             | `Costo de Partida`, `Startup`        |
| `costo_detencion`           | `Costo de Detención`, `Shutdown`     |

Spanish decimals (`1.234,56` → `1234.56`) are coerced. Spanish
month names (`Abril 2026` → `2026-04-01`) are parsed. Missing dates
fall back to the `--date` argument's year-month.

## What it changes in the output

- The catalogue line `MC tiers $X-$Y/MWh — pmax-heuristic only`
  becomes `… — SCVIC declared CV` and reports the hit rate
  (e.g. `SCVIC hits: 85/1261 centrales used declared CV`).
- The remaining ~1100 centrales (renewables, hydro, BESS, PMGDs,
  small thermals not in SCVIC) keep the heuristic.
- Expected mean |Δλ| against CEN λ at the 2 reference buses
  drops from ~$37/MWh (heuristic only) toward ~$3-5/MWh once
  the catalogue is replaced with declared CV.

## If the parser fails

Run with `-v` and inspect:

```
python -c "
import pandas as pd
from cen2gtopt._scvic_loader import _detect_header_row
df = pd.read_excel('tools/scvic_consolidado_2026_04.xlsx', header=None)
print('detected header row:', _detect_header_row(df))
print('row content:')
print(df.iloc[_detect_header_row(df)].tolist())
"
```

If the header row isn't found, paste the first 15 rows into the
issue tracker so the column-detection rules in
`_HEADER_RULES` (in `_scvic_loader.py`) can be extended.
