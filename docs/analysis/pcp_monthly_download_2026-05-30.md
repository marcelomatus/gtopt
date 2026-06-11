# CEN PCP Monthly Download + plexos2gtopt Validation (2026-05-30)

12 monthly samples of the Coordinador Electrico Nacional **PLEXOS PCP**
weekly bundle were downloaded from the CEN portal and fed through
`python -m plexos2gtopt` (no LP solve) to validate end-to-end conversion.

## 1. Target window

- **Calendar window**: 2025-06-01 → 2026-05-30 (the 12 months before
  today).
- **Actual coverage**: the CEN PCP archive only goes back to
  **2025-10-01**, so the available window is 8 months (2025-10 …
  2026-05). Listing call:

  ```bash
  python -m cen2gtopt.pcp_archive list \
    --category PCP --pattern '^PLEXOS[0-9]{8}\.zip$' \
    --since 2025-06-01 --until 2026-05-30 --limit 10000
  ```

  → **221 PLEXOS bundles** spread over the 8 available months
  (no daily gaps beyond CEN's standard weekend-publication cadence).

- **Sampling plan**: 2 dates from each of the 4 oldest available
  months, 1 date from each of the 4 newest months (12 total). Picks
  biased to **Sundays** to land on the PCP weekly anchor. The two
  bundles already cached locally (`PLEXOS20260407.zip`,
  `PLEXOS20260422.zip`) were excluded.

## 2. Chosen dates, sizes, conversion status

All 12 downloads succeeded and all 12 conversions exited cleanly
(`rc=0`, `[PASS]`).

| #  | Date       | DOW | Bytes      | MB    | Conv s | Status |
|----|------------|-----|-----------:|------:|-------:|--------|
|  1 | 2025-10-05 | Sun | 39,001,431 | 37.20 |    10  | PASS   |
|  2 | 2025-10-19 | Sun | 29,173,190 | 27.82 |    12  | PASS   |
|  3 | 2025-11-09 | Sun | 32,469,873 | 30.97 |    12  | PASS   |
|  4 | 2025-11-23 | Sun | 32,201,291 | 30.71 |    13  | PASS   |
|  5 | 2025-12-07 | Sun | 32,753,619 | 31.24 |    11  | PASS   |
|  6 | 2025-12-21 | Sun | 32,601,091 | 31.09 |    12  | PASS   |
|  7 | 2026-01-04 | Sun | 32,969,243 | 31.44 |    13  | PASS   |
|  8 | 2026-01-18 | Sun | 32,866,418 | 31.35 |    11  | PASS   |
|  9 | 2026-02-15 | Sun | 33,053,042 | 31.52 |    11  | PASS   |
| 10 | 2026-03-15 | Sun | 33,209,222 | 31.67 |    11  | PASS   |
| 11 | 2026-04-12 | Sun | 28,559,077 | 27.24 |    10  | PASS   |
| 12 | 2026-05-17 | Sun | 29,125,461 | 27.78 |    11  | PASS   |

- **Total bytes downloaded**: 388,083,958 bytes (≈ **370.0 MB**)
- **Download wall time**: **56 s** (sequential)
- **Conversion wall time**: **138 s** (sequential, 12 × ~11 s)
- **Aggregate wall time**: **≈ 194 s** (3 min 14 s)

## 3. Per-case JSON sizes (post-conversion)

| Date       | nodes | gens | lines | demands | batteries |
|------------|------:|-----:|------:|--------:|----------:|
| 2025-10-05 |   237 | 1695 |   305 |     127 |        23 |
| 2025-10-19 |   239 | 1703 |   306 |     127 |        25 |
| 2025-11-09 |   239 | 1707 |   307 |     127 |        25 |
| 2025-11-23 |   241 | 1709 |   310 |     127 |        25 |
| 2025-12-07 |   241 | 1709 |   309 |     127 |        26 |
| 2025-12-21 |   241 | 1709 |   309 |     127 |        26 |
| 2026-01-04 |   243 | 1717 |   311 |     127 |        29 |
| 2026-01-18 |   243 | 1719 |   311 |     127 |        29 |
| 2026-02-15 |   246 | 1723 |   315 |     127 |        32 |
| 2026-03-15 |   246 | 1724 |   315 |     127 |        33 |
| 2026-04-12 |   247 | 1729 |   316 |     127 |        36 |
| 2026-05-17 |   251 | 1748 |   321 |     139 |        41 |

The CEN system shows steady growth across the 8 months: **+14 nodes /
+53 generators / +16 lines / +12 demands / +18 batteries** from
2025-10-05 to 2026-05-17 — consistent with the well-known
battery-storage build-out.

## 4. Warnings (identical on every conversion)

Each run emitted exactly one WARNING, always the same benign one:

```
horizon-mode=plexos: no PLEXOS solution .accdb and no --block-layout
found; falling back to 168 uniform hourly blocks (7 days × 24,
inferred from Horizon name). Pass --plexos-solution-accdb /
--block-layout to reproduce PLEXOS's variable-duration grouping.
```

This is the expected fallback when `--plexos-solution-accdb` isn't
passed: the converter uses a uniform 168-hour grid instead of CEN's
variable-duration timeslice grouping. For pure conversion validation
(no LP comparison) this is harmless. No ERROR-level messages, no
unknown JSON fields, no missing-collection failures occurred on any
of the 12 cases.

## 5. Reproducibility

```bash
# 1) Listing (12-month window)
python -m cen2gtopt.pcp_archive list \
  --category PCP --pattern '^PLEXOS[0-9]{8}\.zip$' \
  --since 2025-06-01 --until 2026-05-30 --limit 10000 \
  > ~/tmp/pcp_listing.txt

# 2) Download (sequential, each ~3-6 s)
for D in 20251005 20251019 20251109 20251123 20251207 20251221 \
         20260104 20260118 20260215 20260315 20260412 20260517; do
  python -m cen2gtopt.pcp_archive download --name "PLEXOS${D}.zip"
done

# 3) Convert (sequential, each ~11 s)
CACHE=~/.cache/gtopt/cen2gtopt/pcp_archive/PCP
for D in 20251005 20251019 20251109 20251123 20251207 20251221 \
         20260104 20260118 20260215 20260315 20260412 20260517; do
  python -m plexos2gtopt \
    -i "$CACHE/PLEXOS${D}.zip" \
    -o ~/tmp/gtopt_pcp_${D} \
    --horizon-mode plexos --no-check \
    > ~/tmp/conv_${D}.log 2>&1
done
```

Cache directory:
`~/.cache/gtopt/cen2gtopt/pcp_archive/PCP/PLEXOS<YYYYMMDD>.zip`.

Per-conversion logs: `~/tmp/conv_<YYYYMMDD>.log`.

Converted JSON trees: `~/tmp/gtopt_pcp_<YYYYMMDD>/PLEXOS<YYYYMMDD>.json`.

## 6. Bottom line

- **12 / 12 downloads succeeded**.
- **12 / 12 conversions PASSED** (`rc=0`, no errors, only the benign
  `accdb`-missing block-layout fallback warning).
- `plexos2gtopt` is robust over an 8-month, 12-sample slice of the
  CEN PCP archive — no XML-parse failures, no unknown JSON fields,
  no missing-collection errors on any sample.
