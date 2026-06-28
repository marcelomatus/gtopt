# Guatemala — reference data from the operators (AMM / EOR)

Operator-published data downloaded to fill the gaps in the `sddp2gtopt`
model and to validate its LMPs.  Sources verified June 2026.

| file | source | what it gives |
|------|--------|---------------|
| `AMM_PSA20250101_spot_hourly.xls` | AMM `pdfs2/2025/spot/` | **Hourly spot price (POE) for all 2025**, USD/MWh — the LMP validation benchmark.  2025 mean **84.6**, median 79.6; **June 79.6** (closest season to the Jun-2026 PDD case).  ⇒ real Guatemala spot ≈ 80–95 $/MWh, matching PSR's `cmgbuscp` ~95. |
| `AMM_PDS20260628_week27.xlsx` | AMM weekly dispatch | Per-plant **CVG merit lists** ($/MWh) + hourly dispatch + POE for week 27 (28-Jun-2026). |
| `EOR_Mexico-SER_transfer_study_2019.pdf` | EOR | Mexico↔SER transfer-capacity study (wet season) — the interconnection MW limits. |
| `EOR_Informe_Planeamiento_Operativo_2026-2027-Actualizacion_enero-2026.pdf` | EOR | 2026-2027 regional operational-planning study (transfer limits, RTR). |
| `EOR_Informe_de_Identificacion_de_la_RTR-2026_vfinal.pdf` | EOR | Regional Transmission Network identification 2026 (tie lines / corridors). |

## Key number — the interconnection limit

The **Guatemala↔Mexico** tie (Tapachula 400 kV ↔ Los Brillantes) is rated
**~200 MW import / 70 MW export** (CFE firm export 120 MW, expandable to
200) — *not* the ~1,786 MW of $0-fuel import generators in the PSR `.dat`.
That over-capacity is what pins the converted LMPs near $0; capping the
Mexico import at the tie limit (`sddp2gtopt --import-limit 200`) is the fix.

## Live sources (browse for newer data)

- AMM planning index: <https://www.amm.org.gt/ultpub/ultimas_pub_plani.php>
- AMM results index: <https://www.amm.org.gt/ultpub/ultimas_pub_resul.php>
- AMM annual spot: `pdfs2/<YYYY>/spot/PSA<YYYY>0101.xls`
- EOR Máximas Transferencias: <https://www.enteoperador.org/mer/gestion-comercial/maximas-transferencias/>
- EOR MCTP study: <https://www.enteoperador.org/mer/gestion-tecnica-operativa/estudio-de-maximas-capacidades-de-transferencias-de-potencia/>

> Note: `amm.org.gt/pdfs2/*` and `enteoperador.org` reject non-browser
> user-agents — fetch with a real `User-Agent` header.
