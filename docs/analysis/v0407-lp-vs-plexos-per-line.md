# v0407 LP-relax K=10 eps=0.1 vs PLEXOS — per-line comparison

Source: `/home/marce/tmp/gtopt_pcp_v0407_tangent_aux_K10/output_lprelax_K10_eps01/Line/*.parquet`  
PLEXOS ref: `~/.cache/gtopt/plexos_compare/RES20260407-acb3d1f3792b4c0d.pkl`


## System totals

| Quantity | gtopt | PLEXOS | Δ |
|---|---:|---:|---:|
| |signed flow energy| (MWh) | 4,774,638 | 5,002,325 | -4.6% |
| Reported losses (MWh) | 31,779 | 30,692 | +3.5% |
| Analytical losses (MWh) | 27,468 | 30,692 | -10.5% |

Lines matched: 312 / 315 PLEXOS, 312 gtopt.  Direction-reversed lines (sign(g_energy) ≠ sign(p_energy)): **93**


## Top 20 lines by |energy diff|  (signed, gtopt − PLEXOS, MWh)

| name | g_energy | p_energy | de | g_peak | p_peak | g_loss_ana |
| --- | --- | --- | --- | --- | --- | --- |
| LosChangos500->Jadresic500 | -69,023 | 112,915 | -181,938 | 837.4 | 1,120.1 | 377.4 |
| NvaPAzucar500->NvaMaitenc500 | -82,639 | 86,985 | -169,624 | 1,068.7 | 1,090.5 | 502.8 |
| LoAguirre500->Polpaico500 | -106,475 | 53,921 | -160,396 | 1,046.2 | 896.3 | 117.1 |
| Charrua220->Mulchen220 | -66,419 | 62,583 | -129,002 | 457.0 | 457.0 | 763.5 |
| Jadresic500->Jadresic220 | -52,129 | 61,824 | -113,953 | 650.4 | 681.1 | 52.4 |
| NvaMaitenc500->NvaCardones500 | -49,783 | 63,905 | -113,689 | 1,033.9 | 1,051.3 | 181.0 |
| NvaZaldivar220->MonteMina220 | -52,604 | 56,892 | -109,497 | 406.6 | 486.6 | 337.0 |
| Jadresic220->Jadresic220_II | -48,199 | 52,584 | -100,783 | 465.9 | 526.6 | 0.0 |
| EntreRios500->Charrua500 | -43,654 | 47,016 | -90,670 | 480.2 | 511.2 | 13.3 |
| NvaMaitenc500->Maitenc220 | -33,391 | 37,076 | -70,467 | 465.2 | 523.6 | 5.9 |
| Polpaico500->Polpaico220 | -24,277 | 34,238 | -58,514 | 579.6 | 517.2 | 17.6 |
| Colbun220->Ancoa220 | -31,406 | 23,789 | -55,195 | 456.2 | 233.1 | 4.4 |
| LosChangos500->LosChangos220 | -18,088 | 32,734 | -50,822 | 280.5 | 413.5 | 4.1 |
| Cardones220->CPinto220 | -20,389 | 30,342 | -50,730 | 420.0 | 485.3 | 233.7 |
| LosChangos220->Kapatur220 | -17,418 | 32,663 | -50,081 | 280.5 | 413.5 | 12.4 |
| Apoquindo110->ElSalto110 | -27,658 | 21,004 | -48,663 | 246.8 | 195.9 | 93.8 |
| Temuco220->Cautin220 | -23,786 | 24,369 | -48,155 | 207.3 | 211.8 | 19.3 |
| Nogales220->Ventanas220 | -20,263 | 25,177 | -45,439 | 353.9 | 337.2 | 92.0 |
| StaRosa110->AJahuel110 | -20,323 | 21,010 | -41,333 | 187.7 | 193.3 | 129.0 |
| Almendros220->AJahuel220 | -18,288 | 21,739 | -40,027 | 237.0 | 246.9 | 65.8 |

## Top 20 lines by |peak diff|  (MW)

| name | g_peak | p_peak | dp | g_energy | p_energy | g_loss_ana |
| --- | --- | --- | --- | --- | --- | --- |
| LosChangos500->Kimal500_II | 490.0 | 853.6 | -363.6 | 46,732 | 72,076 | 203.5 |
| CNavia220_Aux_D->Polpaico220 | 15.5 | 361.9 | -346.4 | -1,539 | 31,659 | 0.5 |
| CNavia220->CNavia220_Aux_D | 15.5 | 361.9 | -346.4 | -1,539 | 31,659 | 0.0 |
| Colbun220->PNegro220 | 539.7 | 214.2 | +325.5 | 30,489 | 22,723 | 442.8 |
| Ancoa500->Ancoa220 | 547.7 | 256.5 | +291.2 | 28,671 | 10,677 | 11.5 |
| PNegro220->Candela220 | 585.7 | 294.9 | +290.8 | 40,864 | 33,395 | 453.1 |
| LosChangos500->Jadresic500 | 837.4 | 1,120.1 | -282.7 | -69,023 | 112,915 | 377.4 |
| Candela220->AJahuel220 | 408.0 | 131.0 | +277.0 | 11,287 | 6,346 | 96.8 |
| Illapa220->DAlmagro220 | 157.8 | 396.2 | -238.3 | 7,498 | 27,746 | 50.3 |
| Jadresic500->Cumbres500 | 575.7 | 812.5 | -236.8 | 31,014 | 47,289 | 82.3 |
| Illapa220->Cumbres500 | 345.9 | 580.3 | -234.4 | 10,375 | 36,772 | 59.7 |
| Colbun220->Ancoa220 | 456.2 | 233.1 | +223.1 | -31,406 | 23,789 | 4.4 |
| NvaCardones500->Cardones220 | 365.6 | 562.8 | -197.2 | -1,446 | 37,315 | 3.3 |
| Ralco220->LosNotros220 | 29.5 | 214.4 | -184.9 | 4,963 | 6,962 | 1.8 |
| Andes220->Oeste220 | 428.0 | 248.1 | +179.9 | 25,204 | 14,796 | 467.7 |
| Laberinto220->Oeste220 | 384.6 | 234.6 | +150.1 | -22,270 | 13,353 | 828.8 |
| LoAguirre500->Polpaico500 | 1,046.2 | 896.3 | +149.8 | -106,475 | 53,921 | 117.1 |
| Quintero220->SanLuis220 | 8.1 | 143.1 | -135.0 | 483 | 3,492 | 0.1 |
| LosChangos500->LosChangos220 | 280.5 | 413.5 | -133.0 | -18,088 | 32,734 | 4.1 |
| LosChangos220->Kapatur220 | 280.5 | 413.5 | -132.9 | -17,418 | 32,663 | 12.4 |

## Top 20 lines by gtopt analytical loss MWh

| name | g_loss_ana | g_loss_rep | g_energy | p_energy | g_peak | p_peak |
| --- | --- | --- | --- | --- | --- | --- |
| NvaPAzucar500->Polpaico500_I | 1,959.8 | 1,937.7 | 81,116 | 82,062 | 1,000.0 | 1,000.0 |
| Laberinto220->Oeste220 | 828.8 | 973.0 | -22,270 | 13,353 | 384.6 | 234.6 |
| Encuentro220->Colla220 | 770.4 | 766.5 | 25,867 | 27,393 | 212.1 | 224.2 |
| Charrua220->Mulchen220 | 763.5 | 760.3 | -66,419 | 62,583 | 457.0 | 457.0 |
| Jadresic220_II->MonteMina220 | 623.0 | 2,767.6 | 46,424 | 36,743 | 512.4 | 406.5 |
| Miraje220->TOEnlace220 | 559.4 | 647.5 | 17,110 | 16,055 | 148.3 | 149.1 |
| Mulchen220->Cautin220 | 559.0 | 664.2 | 32,185 | 37,423 | 411.5 | 436.8 |
| GasAta220->Esmeralda220 | 540.9 | 536.3 | 26,006 | 28,596 | 197.0 | 283.5 |
| Capricornio110->LaNegra110 | 537.7 | 532.3 | 12,768 | 24,340 | 76.0 | 205.5 |
| Cipreses154->Itahue154 | 520.9 | 515.5 | 18,080 | 30,062 | 153.1 | 192.9 |
| NvaPAzucar500->NvaMaitenc500 | 502.8 | 535.0 | -82,639 | 86,985 | 1,068.7 | 1,090.5 |
| Andes220->Oeste220 | 467.7 | 517.6 | 25,204 | 14,796 | 428.0 | 248.1 |
| Cochrane220->Encuentro220 | 465.6 | 432.8 | 30,320 | 27,517 | 491.6 | 489.6 |
| PNegro220->Candela220 | 453.1 | 440.4 | 40,864 | 33,395 | 585.7 | 294.9 |
| Polpaico220->ElSalto110 | 445.9 | 439.0 | 62,903 | 47,757 | 528.8 | 405.9 |
| Colbun220->PNegro220 | 442.8 | 432.2 | 30,489 | 22,723 | 539.7 | 214.2 |
| Quillota220->Polpaico220 | 424.1 | 417.9 | 67,714 | 44,811 | 665.4 | 554.2 |
| Quillota220->Mauro220 | 396.7 | 379.1 | 18,491 | 14,719 | 234.9 | 216.2 |
| Nogales220->Polpaico220 | 394.6 | 366.3 | 40,605 | 27,638 | 440.2 | 360.9 |
| LosChangos500->Jadresic500 | 377.4 | 402.6 | -69,023 | 112,915 | 837.4 | 1,120.1 |

## Top 15 direction-reversed lines (sign disagreement)

| name | g_energy | p_energy | g_peak | p_peak | g_loss_ana |
| --- | --- | --- | --- | --- | --- |
| LosChangos500->Jadresic500 | -69,023 | 112,915 | 837.4 | 1,120.1 | 377.4 |
| NvaPAzucar500->NvaMaitenc500 | -82,639 | 86,985 | 1,068.7 | 1,090.5 | 502.8 |
| LoAguirre500->Polpaico500 | -106,475 | 53,921 | 1,046.2 | 896.3 | 117.1 |
| Charrua220->Mulchen220 | -66,419 | 62,583 | 457.0 | 457.0 | 763.5 |
| Jadresic500->Jadresic220 | -52,129 | 61,824 | 650.4 | 681.1 | 52.4 |
| NvaMaitenc500->NvaCardones500 | -49,783 | 63,905 | 1,033.9 | 1,051.3 | 181.0 |
| NvaZaldivar220->MonteMina220 | -52,604 | 56,892 | 406.6 | 486.6 | 337.0 |
| Jadresic220->Jadresic220_II | -48,199 | 52,584 | 465.9 | 526.6 | 0.0 |
| EntreRios500->Charrua500 | -43,654 | 47,016 | 480.2 | 511.2 | 13.3 |
| NvaMaitenc500->Maitenc220 | -33,391 | 37,076 | 465.2 | 523.6 | 5.9 |
| Polpaico500->Polpaico220 | -24,277 | 34,238 | 579.6 | 517.2 | 17.6 |
| Colbun220->Ancoa220 | -31,406 | 23,789 | 456.2 | 233.1 | 4.4 |
| LosChangos500->LosChangos220 | -18,088 | 32,734 | 280.5 | 413.5 | 4.1 |
| Cardones220->CPinto220 | -20,389 | 30,342 | 420.0 | 485.3 | 233.7 |
| LosChangos220->Kapatur220 | -17,418 | 32,663 | 280.5 | 413.5 | 12.4 |