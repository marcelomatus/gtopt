# Support — PLP Reference Cases

PLP (*Programación de Largo Plazo*) cases used for validation and
benchmarking of `plp2gtopt` conversions.  PLP is the long-term
hydro-thermal scheduling tool widely used in Latin America; its
input data is stored as a collection of fixed-format `.dat` text
files describing the power-system topology, generator parameters,
demand curves, hydrology, and maintenance schedules.

See [`docs/scripts/plp2gtopt.md`](../../docs/scripts/plp2gtopt.md)
for the converter's full input ledger and end-to-end pipeline.

## Layout

```
support/plp/
├── README.md                          ← this file
├── 2_years/                  (360 M)  ← 2-year planning case (with Parquet outputs)
│   ├── plpblo.dat.xz
│   ├── plpcnfce.dat.xz
│   ├── plpmance.dat.xz
│   ├── plplajam.dat.xz       ← Laja irrigation config
│   ├── plpmaulen.dat.xz      ← Maule irrigation config
│   └── ...
├── 5_years/                  (108 M)  ← 5-year planning case
│   ├── plpaflce.dat.xz
│   ├── plpblo.dat.xz
│   ├── ...
└── long_term/                ( 60 M)  ← Long-term planning case (irrigation + seepage)
    ├── plpaflce.dat.1.xz             ← Split (compressed > 10 MB single-part)
    ├── plpaflce.dat.2.xz
    ├── plpblo.dat.xz
    ├── plpfilemb.dat.xz             ← Seepage curves (ELTORO, CIPRESES, COLBUN)
    ├── plplajam.dat.xz              ← Laja agreement parameters
    ├── plpmaulen.dat.xz             ← Maule agreement parameters
    └── ...
```

## File-format conventions

- **Codec**: every `.dat`, `.csv`, `.prn`, `.png` is xz-compressed
  with `xz -6` (multi-threaded via `-T0`).  `plp2gtopt` reads `.xz`
  files transparently — no manual decompression is needed.
- **Split files**: any file whose compressed size exceeds 10 MB is
  split into numbered parts (`plpaflce.dat.1.xz`, `plpaflce.dat.2.xz`,
  …) before compression.  `plp2gtopt` decompresses parts in parallel
  (4 threads) and concatenates them transparently.
- **Typical compression ratio**: 10×–100× for PLP `.dat` files.

## Adding a new PLP case

```bash
# 1. Create the case directory
mkdir support/plp/my_new_case

# 2. Copy raw .dat / .csv files in
cp /path/to/plp/case/*.dat support/plp/my_new_case/
cp /path/to/plp/case/*.csv support/plp/my_new_case/

# 3. Compress with the project's xz helper (handles splitting)
scripts/plp_compress_case.sh support/plp/my_new_case

# 4. Verify with plp2gtopt
plp2gtopt --input-dir support/plp/my_new_case \
          --output-dir ~/tmp/my_new_case_out

# 5. Commit
git add support/plp/my_new_case/
git commit -m "feat: add my_new_case PLP test case"
```

The helper script:
- compresses each file with `xz -6 -T0`,
- automatically splits files that compress to more than 10 MB into
  numbered parts,
- removes the original uncompressed files.

To change the split threshold:
```bash
scripts/plp_compress_case.sh support/plp/my_new_case --split-mb 8
```

To inspect a compressed case without converting:
```bash
scripts/plp_compress_case.sh --decompress support/plp/long_term
```
(undoes step 3 — only useful for debugging the compressed archive
itself; the standard workflow keeps everything compressed).

## Canonical reference cases

- `2_years/` — the canonical test case for irrigation-agreement
  comparisons (Laja + Maule), the BAT_ALICANTO reproducer, and
  per-element output sizing audits.
- `5_years/` — mid-horizon validation (capacity expansion +
  hydro-thermal coordination).
- `long_term/` — full-horizon case with irrigation + seepage
  (ELTORO, CIPRESES, COLBUN) and split `plpaflce.dat.*.xz`.

`plp_implementation.md`, `right_junctions_analysis.md`,
`gtopt_vs_plp_comparison.md`, and `seepage_and_colchones_analysis.md`
(under `docs/analysis/irrigation_agreements/`) all reference these
cases.

## See also

- [`../plexos/README.md`](../plexos/README.md) — sibling PLEXOS PCP
  daily bundles.
- [`docs/scripts/plp2gtopt.md`](../../docs/scripts/plp2gtopt.md) —
  converter reference.
- [`scripts/plp2gtopt/`](../../scripts/plp2gtopt/) — converter source.
- [`scripts/plp_compress_case.sh`](../../scripts/plp_compress_case.sh)
  — xz + split helper.
