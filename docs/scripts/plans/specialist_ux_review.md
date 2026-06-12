# UX Review: `gtopt_marginal_units` + `cen2gtopt` — specialist critique

> **Status**: pre-implementation review against master plan §5, §9.5, §4.6, §4.8.3, §6, §10, §13.
> **Reviewer**: UX critic agent (Claude Sonnet 4.6), 2026-05-05.
> **Source read**: `docs/scripts/gtopt_marginal_units_plan.md` (full).
> **Peer scripts read**: `scripts/gtopt_check_output/main.py`,
> `scripts/gtopt_compare/main.py`, `scripts/gtopt_results_summary/main.py`,
> `scripts/cen_demanda/main.py`, `scripts/plp2gtopt/_parsers.py`.

---

## 1. Scope

Reviewed surfaces:

- §5 — `gtopt_marginal_units` CLI flag list and exit codes.
- §9.5, §9.5.1, §9.5.2 — `cen2gtopt` CLI, on-the-spot semantics, exit codes.
- §4.6 — output Parquet schema (columns, units, types).
- §4.8.3 — required-by-mode table.
- §6 — edge-case error scenarios (13 pitfalls).
- §10 — intentionally out-of-scope items.
- §13 — open questions.
- Existing-script conventions for naming and style consistency.

CLI commands tried (read-only): none of these scripts exist yet; the review
is against the design plan.

---

## 2. Persona impact matrix

| # | Finding (short) | Heuristic | Severity | Freq | A | B | C | D | E |
|---|---|---|---|---|---|---|---|---|---|
| P0-1 | `--from/--to` collides with `cen_demanda`'s `--start/--end` | N4, S1 | major | frequent | - | X | X | X | - |
| P0-2 | `--input {gtopt,feed}` switch forces recall of a non-obvious enum before any work | N6, GPC4, S8 | major | frequent | X | X | X | - | - |
| P0-3 | Mode/input cross-constraint is invisible at help time | N5, GPC2 | major | frequent | X | X | X | - | - |
| P0-4 | `--api-key` env var `$CEN_API_KEY` clashes with `cen_demanda`'s `$CEN_USER_KEY` | N4, S1 | major | occasional | - | X | X | X | - |
| P1-1 | No canonical example in §5 — dual-script flow is buried in the body | N10, GPC5 | minor | frequent | - | X | X | - | - |
| P1-2 | `--output`/`--out` naming inconsistency across the spec | N4, S1 | minor | frequent | X | X | X | - | - |
| P1-3 | `cells_within(±0.5 USD/MWh)` is an invalid Parquet column name | N4, GPC6 | major | constant | X | X | - | X | - |
| P1-4 | `confidence` values underdocumented; `published` used in §4.8.2 but absent from §4.6 | N10, GPC2 | minor | frequent | X | X | - | - | - |
| P1-5 | Exit code 1 missing from both scripts (argparse uses it for bad args) | N9, S5 | minor | frequent | - | X | X | X | - |
| P1-6 | No progress feedback for long fetch + reconstruction runs | N1, S3 | minor | frequent | - | X | - | X | - |
| P1-7 | `--include {dispatch,demand,lmp,flows}+` syntax is non-standard argparse | N4, GPC6 | minor | occasional | - | X | X | X | - |
| P1-8 | `--zone-mode both` produces two overlapping attributions with no merge rule | GPC3, N9 | minor | occasional | X | - | - | - | - |
| P1-9 | Tolerance flags on `gtopt_marginal_units` have no short forms; power users will tire | N7, S2 | cosmetic | occasional | X | - | - | X | - |
| P2-1 | `--mode-help` not in the spec; four modes are too dense for one-liner help | N6, N10 | cosmetic | occasional | X | X | X | - | - |
| P2-2 | `reduced_cost` column name is overloaded (`MC - λ_g` is not the LP reduced cost) | N2, GPC6 | cosmetic | occasional | X | - | - | - | - |
| P2-3 | `--plp-compare` in §7.4 is undocumented in the §5 flag list | N10 | cosmetic | rare | X | - | - | - | - |
| P2-4 | `--cen-tz` appears in §6.11 but is absent from the §5/§9.5 flag lists | N10, N4 | minor | rare | - | X | - | - | - |

---

## 3. Findings — P0 (user-blocking)

---

### P0-1 — `--from`/`--to` conflicts with the existing `cen_demanda` convention `--start`/`--end`

**What:** The plan names the date-range arguments `--from YYYY-MM-DD --to YYYY-MM-DD`
on `cen2gtopt`. The existing `scripts/cen_demanda/main.py` (lines 213–224) uses
`--start` / `--end` for the same concept against the same CEN SIPUB API. A user
who has previously used `cen_demanda` will type `--start 2026-04-01 --end 2026-04-07`
and get an argparse error with no helpful hint.

**Heuristic:** N4 (Consistency and standards), S1 (Strive for consistency).

**Severity x Frequency:** major x frequent.

**Where:** §9.5 flag list; contrast `scripts/cen_demanda/main.py:214,220`.

**Who:** Persona B (operator), C (first-time), D (power user).

**Reproducer:**
```bash
# cen_demanda user's muscle memory:
cen2gtopt --start 2026-04-01 --end 2026-04-07 --out feed.parquet
# → argparse error: unrecognized arguments: --start
```

**Proposed fix:** Rename `--from`/`--to` to `--start`/`--end` to match
`cen_demanda`. Alternatively, accept both forms (`--from`/`--start`,
`--to`/`--end`) with a deprecation notice for the non-canonical form.
Document the choice in §9.5 with an explicit cross-reference to `cen_demanda`.

**Effort:** S

---

### P0-2 — `--input {gtopt,feed}` forces users to memorise a non-obvious enum

**What:** The primary dispatch flag `--input {gtopt,feed}` requires the user to
know in advance whether they have a "gtopt output directory" or a "canonical
operation feed". Neither term is self-explanatory to a first-time user: a gtopt
output directory *is* an output feed; "feed" does not tell you its on-disk shape.
The flag name `--input` with a `kind` enum is also inconsistent with every other
gtopt script, which uses positional or directory-named arguments (e.g.
`gtopt_check_output CASE_DIR`, `gtopt_compare --gtopt-output DIR`).

More critically, if a user omits `--input` there is no fallback — the plan does
not specify auto-detection or a default, so a missing `--input` will produce an
argparse-level error with no user-actionable message.

**Heuristic:** N6 (Recognition rather than recall), GPC4 (Reduce working memory
load), S8 (Reduce short-term memory load).

**Severity x Frequency:** major x frequent.

**Where:** §5, flag `--input {gtopt,feed}`.

**Who:** Persona A (researcher), B (operator), C (first-time).

**Reproducer:**
```bash
# User has a gtopt output dir and does not read the docs:
gtopt-marginal-units --planning plan.json --output output/
# → argparse error: the following arguments are required: --input
# User has no idea what value to supply.
```

**Proposed fix — two-pronged:**

1. Rename the flag to `--input-kind {gtopt-dir,feed-parquet}` so the values
   describe what the user *has*, not an internal category. Consider accepting both
   spellings: `gtopt-dir|gtopt` and `feed-parquet|feed`.

2. Implement auto-detection as a fallback: if `--input-kind` is absent, sniff the
   `--planning` / `--output` combination (if both are present → `gtopt-dir`; if
   `--feed` is present → `feed-parquet`). Emit `info: auto-detected input kind:
   gtopt-dir` so the user knows what happened (N1, GPC2).

**Effort:** S (rename) + M (auto-detection)

---

### P0-3 — Mode/input cross-constraints are invisible at `--help` time

**What:** Not every `(--input, --mode)` pair is legal: `--input gtopt --mode real`
is an error (§4.8.3). The plan specifies the constraint table only in §4.8.3, deep
in the algorithm section. The §5 CLI synopsis shows `--input` and `--mode` as
independent flags. A user who picks `--input gtopt --mode real` will not know it
is invalid until they attempt a run and receive exit code 3 with message
"missing X for mode Y". That message tells them *what* is missing but not *why*
the combination is illegal or what to do instead.

**Heuristic:** N5 (Error prevention), GPC2 (Reduce uncertainty).

**Severity x Frequency:** major x frequent.

**Where:** §5 synopsis; §4.8.3 cross-constraint table.

**Who:** Persona A, B, C.

**Reproducer:**
```bash
gtopt-marginal-units --input gtopt --mode real --planning plan.json --output out/
# → exit 3 "missing lmp[bus_uid] for mode real"
# User confused: they have balance_dual in their gtopt output and think that IS lmp.
```

**Proposed fix:**

In the `--help` text for `--mode`, after the four mode names, append a two-line
constraint summary, e.g.:

```
  simulated  requires --input gtopt (LP duals available)
  real        requires --input feed (uses feed lmp column directly)
  real-reconstruct  requires --input feed (reconstructs lmp from dispatch)
  compare     requires both --input gtopt/feed and --feed-against/--gtopt-against
```

When the user supplies an illegal `(--input, --mode)` pair, emit the clearer
message:

```
error: --mode real requires --input feed (gtopt LP output does not contain
       realised CEN operation data); use cen2gtopt to produce a feed first.
exit 3
```

**Effort:** S

---

### P0-4 — Auth env var `$CEN_API_KEY` conflicts with existing `$CEN_USER_KEY`

**What:** The `cen_demanda` script (which talks to the same SIPUB API) reads the
token from `$CEN_USER_KEY` and passes it as the `user_key` query parameter
(line 241: `default=os.environ.get("CEN_USER_KEY")`). The plan for `cen2gtopt`
(§9.5) introduces `--api-key $CEN_API_KEY` and a different env var name.
A user who already has `CEN_USER_KEY` set will expect `cen2gtopt` to pick it up
automatically, but it will not, and the script will silently fall back to CSV mode
with no indication that auth was attempted and failed.

**Heuristic:** N4 (Consistency and standards), S1 (Strive for consistency).

**Severity x Frequency:** major x occasional.

**Where:** §9.5 `--api-key $CEN_API_KEY`; `scripts/cen_demanda/main.py:241`.

**Who:** Persona B (operator), C (first-time), D (power user).

**Reproducer:**
```bash
export CEN_USER_KEY=mytoken   # already set from cen_demanda usage
cen2gtopt --from 2026-04-01 --to 2026-04-07 --out feed.parquet
# → silently uses CSV path; no warning that SIP auth failed or was skipped
```

**Proposed fix:** Accept `$CEN_USER_KEY` as the primary env-var name (matching
`cen_demanda`) and also accept `$CEN_API_KEY` as an alias. Document both. Or
standardise the project on `$CEN_API_KEY` and add `--user-key` as an alias on
`cen_demanda` with a deprecation warning. Whichever you choose, emit a log message
at the INFO level: `info: SIP auth via $CEN_USER_KEY` or `info: SIP key absent;
falling back to CSV exports (slower, no topology)`.

**Effort:** S

---

## 4. Findings — P1 (significant friction)

---

### P1-1 — The two-script "quick start" example is buried; it must be at the top

**What:** §5 shows the two-command flow only *after* 700 lines of algorithm.
The plan says "Convention follows `scripts/gtopt_check_output/` and
`scripts/gtopt_results_summary/`" — both of those scripts put copy-pasteable
examples in the `_DESCRIPTION` block that appears at the very top of `--help`.
The plan specifies no equivalent for either new script.

**Heuristic:** N10 (Help and documentation), GPC5 (Present new information with
meaningful context).

**Where:** §5 prose; compare `gtopt_check_output/main.py:21–31`.

**Who:** Persona B, C.

**Proposed fix:** The implementation plan for both scripts must require a
`_DESCRIPTION` block with at minimum one copy-pasteable two-command example as
the **first** thing the user sees from `--help`:

```
Examples:
  # Fetch one week of CEN real operation data:
  cen2gtopt --start 2026-04-01 --end 2026-04-07 --out feed.parquet

  # Identify marginal units from that feed:
  gtopt-marginal-units --input feed --feed feed.parquet \
      --mode real-reconstruct --report report.md

  # Identify marginal units from a gtopt simulation:
  gtopt-marginal-units --input gtopt --planning plan.json \
      --output output/ --mode simulated
```

**Effort:** S

---

### P1-2 — Mixed `--out` / `--output` convention across the spec and peer scripts

**What:** The §5 flag list uses `--out` for `gtopt_marginal_units`. The §9.5 flag
list uses `--out` for `cen2gtopt`. But every peer script that writes a file uses
`--output` (checked: `gtopt2pp/main.py:185`, `ts2gtopt/main.py:231`,
`pp2gtopt/main.py:189`, `gtopt_timeseries_export/main.py:29`). The `--output`
flag also aligns with `cen_demanda`'s `--output-dir`. Using `--out` breaks the
"same flag name for same concept" rule the toolchain has followed.

**Heuristic:** N4, S1.

**Where:** §5 `--out path/to/marginal_units.parquet`; §9.5 `--out path/to/...`.

**Who:** All CLI users (A, B, C, D).

**Proposed fix:** Rename `--out` to `--output` in both CLIs. If brevity is
important, add `-o` as the short form (which is unoccupied in the peer scripts).

**Effort:** S

---

### P1-3 — `cells_within(±0.5 USD/MWh)` is an invalid Parquet column name

**What:** §4.7 R5 (audit columns) lists two summary columns:

> `cells_within(±0.5 USD/MWh)` and `cells_within(±2 USD/MWh)`

Column names with parentheses, `±`, and `/` are not valid Python identifiers and
are rejected by most Parquet / pandas workflows unless quoted everywhere. Any
downstream notebook doing `df.cells_within(±0.5 USD/MWh)` will fail with a
`SyntaxError`. Even `df["cells_within(±0.5 USD/MWh)"]` is legal Python but will
silently break pyarrow schema inference, dask, and connectorX integrations.

**Heuristic:** N4 (Consistency and standards), GPC6 (Use names that are
conceptually close to the function).

**Severity x Frequency:** major x constant (every audit run).

**Where:** §4.7 R5 prose.

**Who:** Persona A (researcher, notebooks), D (power user).

**Proposed fix:** Rename to standard snake_case identifiers with the threshold
encoded in the name:

| Old name | Proposed name |
|---|---|
| `cells_within(±0.5 USD/MWh)` | `cells_within_0p5_usd_mwh` |
| `cells_within(±2 USD/MWh)` | `cells_within_2p0_usd_mwh` |
| `cells_with_topology_mismatch` | already valid, keep as-is |
| `cells_with_must_run_override` | already valid, keep as-is |

Or, use a tidy summary DataFrame with a `threshold_usd_mwh` row-key column and a
`count` value column so thresholds are data, not column names.

**Effort:** S

---

### P1-4 — `confidence` column has four values in the text but only three in §4.6

**What:** §4.6 defines `confidence: str` as one of `"lp_dual" | "merit_order" |
"fallback"`. §4.8.2 mentions a fourth value: `"published"` (when the LMP is read
directly from the feed without reconstruction). §4.7 R4 mentions `confidence` is
set to `"merit_order"` uniformly in that mode. Neither section reconciles the
full enumeration. A researcher building a notebook pivot on `confidence` will
discover the `"published"` value only at runtime.

**Heuristic:** N10, GPC2.

**Where:** §4.6 output schema; §4.8.2 table.

**Who:** Persona A, B.

**Proposed fix:** Add a dedicated "Confidence levels" subsection inside §4.6 (or
immediately after it) that exhaustively enumerates all possible `confidence`
values:

| Value | Meaning | Produced by |
|---|---|---|
| `lp_dual` | LP reduced costs were available (simulated mode) | `--input gtopt --mode simulated` |
| `merit_order` | Only dispatch + declared MC + topology (no duals) | `--mode real` / `--mode real-reconstruct` |
| `published` | LMP read directly from feed; no reconstruction | `--mode real` when feed contains `lmp` column |
| `fallback` | Unattributed cell — LP-degeneracy escape hatch | any mode |

**Effort:** S

---

### P1-5 — Exit code 1 missing from both exit-code tables

**What:** Argparse uses exit code 1 internally for `--help` (exit 0) and for
invalid arguments (exit 2). In practice argparse exits with `2` for bad arguments,
but many callers also rely on distinguishing code 1 (check/lint failure) from
code 2 (configuration/input error). The plan defines:

- `gtopt_marginal_units`: codes 0, 2, 3 (§5).
- `cen2gtopt`: codes 0, 2, 3, 4 (§9.5.2).

Neither table documents what exit code 1 means (or explicitly states it is
unused). `gtopt_check_output` exits with `1` when findings are critical (line 265).
`gtopt_compare` exits with `1` for numeric mismatch (line 1530). If
`gtopt_marginal_units` also emits `1` for "at least one classification is
suspect", downstream `&&`-chained scripts will behave inconsistently depending on
which script they are using.

**Heuristic:** N9, S5.

**Where:** §5 exit codes; §9.5.2 exit codes.

**Who:** Persona B (operator batch scripts), D (power user).

**Proposed fix:** Add a row for exit code 1 to both tables. Decide whether 1 is
"partial result + suspect classifications" (separate from 2 = "degenerate cells"),
or explicitly state it is unused/reserved with a note that argparse does not
produce it for these scripts.

**Effort:** S

---

### P1-6 — No progress feedback for long network-fetch and reconstruction runs

**What:** A 90-day CEN fetch + per-hour reconstruction over a national grid
(hundreds of generators, thousands of buses) can take many minutes. The `cen2gtopt`
spec mentions an on-disk cache but does not specify any progress indication. The
`gtopt_marginal_units` spec mentions `-v|-q` but does not specify a progress bar.
Contrast with `run_gtopt` (which uses `rich.Live` at 4 Hz) and `plp2gtopt`
(which has a `_progress.py` module with a Braille spinner, referenced in
`_tui.py:94`).

**Heuristic:** N1 (Visibility of system status), S3 (Offer informative feedback).

**Where:** §9.5 (no progress mention); §5 (`-v|-q` only).

**Who:** Persona B, D.

**Proposed fix:**

For `cen2gtopt`: emit per-day progress on stderr using Python's built-in `tqdm`
(already in `requirements.txt`) or Rich's `Progress`, showing `[fetching
2026-04-01 … 2026-04-07]  day 3/7`. Exit early on Ctrl-C with a clean message
and write the `.partial` file (which the spec already mentions for exit code 4 —
leverage the same mechanism for user-initiated stops).

For `gtopt_marginal_units`: emit per-cell-batch progress when the total cell count
exceeds a threshold (e.g. 1000 cells). Use the same Rich/tqdm pattern.

Both scripts should respect `--quiet` to suppress the bar.

**Effort:** M

---

### P1-7 — `--include {dispatch,demand,lmp,flows}+` is non-standard argparse syntax

**What:** The spec writes `--include {dispatch,demand,lmp,flows}+` which implies
a multi-valued flag accepting a `+`-separated or space-separated list. Standard
argparse handles this with `nargs='+'` and `choices=[...]`, but the user-visible
syntax on the command line is then `--include dispatch lmp flows` (space-separated),
not `--include dispatch,lmp,flows` (comma-joined). The spec does not state which
form is used, and neither form is shown in an example.

Additionally, the default `dispatch,demand` is not shown in the `--help` text
skeleton in §9.5, so first-time users cannot see what they will get by default.

**Heuristic:** N4, GPC6.

**Where:** §9.5 `--include` flag.

**Who:** B, C, D.

**Proposed fix:** In the implementation spec, lock down the syntax to
`--include dispatch --include lmp` (repeatable flag, argparse `action='append'`)
or `--include dispatch,lmp` (comma-joined string, manually split). Add an example
to the `--help` epilog. Document the default in the metavar: `--include DATA`
with help text `"One or more of: dispatch, demand, lmp, flows
(default: dispatch demand). Repeat flag or comma-join: --include lmp,flows."`.

**Effort:** S

---

### P1-8 — `--zone-mode both` produces two independent attributions with no merge rule documented

**What:** §6 edge case 9 defines `--zone-mode both` as writing "two independent
attributions side by side, useful when the user wants to know what the price
*would* be without congestion." The output schema in §4.6 has no `zone_mode`
discriminator column. How the user is supposed to distinguish the two sets of rows
in the single `marginal_units.parquet` output file is not specified. Will rows be
duplicated? Will there be a `zone_mode` column? A suffix in the `zone_id`?

**Heuristic:** GPC3 (Fuse data to reduce search), N9.

**Where:** §6 edge case 9; §4.6 output schema.

**Who:** Persona A (researcher doing congestion studies).

**Proposed fix:** Add a `zone_mode: str` column to the §4.6 output schema with
values `"congestion"` and `"physical"`. When `--zone-mode both` is used, every
row appears twice — once with each `zone_mode` value. Document this explicitly in
§4.6 with a note: "When `--zone-mode both` is used, the output contains two rows
per (cell, generator) — one for each zone partitioning method."

**Effort:** S

---

## 5. Findings — P2 (polish)

---

### P2-1 — Four-mode density: `--mode-help` long-form discoverability

**What:** Four modes (`simulated`, `real`, `real-reconstruct`, `compare`) with
input-coupling constraints is too much to absorb from a one-line choices list in
`--help`. Peer tools in the optimization space (GenX, Calliope) use a separate
`--list-modes` or `--mode-help` flag for dense option sets. This is a P2 because
the plan does document modes fully in §4.8.3, but that document is not the
`--help` text.

**Heuristic:** N6, N10.

**Where:** §5 `--mode` flag.

**Who:** A, B, C.

**Proposed fix:** In the implementation, add a `--mode-help` flag that prints a
full mode reference (the §4.8.3 table rendered as plain text) to stdout and exits
0. Reference it in the `--mode` help string: "Use --mode-help for a full
mode/input compatibility table."

**Effort:** S

---

### P2-2 — `reduced_cost` column name is semantically wrong for `MC - λ_g`

**What:** The §4.6 output schema defines `reduced_cost: float` as `MC − λ_g` (the
gap between a generator's marginal cost and the bus LMP). In LP duality theory
the reduced cost of a primal variable `g` is `c_g − λ_b^T A_col` — the
sensitivity of the objective to a unit increase in `g`. `MC − λ_g` is closely
related but not identical: for an equality-priced unit `MC = λ_b` both are zero,
but for a bounded unit the LP reduced cost absorbs the bound multipliers, which
`MC − λ_g` does not. Using `reduced_cost` here will confuse Persona A, who knows
what LP reduced costs are.

**Heuristic:** N2 (Match between system and real world), GPC6.

**Where:** §4.6 output schema column `reduced_cost`.

**Who:** Persona A (researcher with LP background).

**Proposed fix:** Rename to `mc_minus_lmp: float` (explicit arithmetic) or
`price_gap_usd_mwh: float` (domain-oriented). Add a note in the schema doc:
"This is `MC_g − λ_bus(g)`, not the LP reduced cost (which also includes bound
multipliers)."

**Effort:** S (rename in schema spec before any code is written — zero migration
cost at this stage).

---

### P2-3 — `--plp-compare` (§7.4) is absent from the §5 public flag list

**What:** §7.4 describes a `--plp-compare path/to/plp/cmgnud.csv` flag as a
debugging aid. It appears nowhere in the §5 CLI synopsis. If a developer
implements it (as the spec implies they should), there will be an undocumented
public flag, which violates the "every flag in docs" completeness rule.

**Heuristic:** N10 (Help and documentation).

**Where:** §7.4 vs §5.

**Who:** Persona A (researcher doing PLP vs gtopt cross-checks).

**Proposed fix:** Either add `--plp-compare` to the §5 flag list with a note that
it is a debugging aid, or move §7.4 to §10 (out-of-scope for v1) to prevent
accidental implementation without documentation.

**Effort:** S

---

### P2-4 — `--cen-tz` override appears in §6.11 but is absent from `cen2gtopt` §9.5

**What:** §6 edge case 11 (time-zone alignment) says "A `--cen-tz` override lets
the user pin a different zone (e.g. SING legacy data)." This flag is not in the
§9.5 `cen2gtopt` CLI synopsis, nor is it in the `gtopt_marginal_units` §5
synopsis. The related `--source-tz` flag *is* in §9.5. It is unclear whether
`--cen-tz` and `--source-tz` are the same flag with two different names, or two
different flags.

**Heuristic:** N10, N4.

**Where:** §6.11 (`--cen-tz`); §9.5 (`--source-tz`).

**Who:** Persona B (operator handling historical SING data).

**Proposed fix:** Decide on one canonical name (`--source-tz` is already in §9.5,
so prefer that) and remove the `--cen-tz` mention from §6.11. Add a sentence in
§9.5: "Use `--source-tz` to override the default `Chile/Continental` locale (e.g.
`--source-tz America/Santiago` for historical SING data)."

**Effort:** S

---

## 6. Positives

- **Phase split at schema boundary** (§1.5) is an excellent UX architecture
  decision: it means a researcher can run `gtopt_marginal_units` on pure gtopt
  data with no network dependency and no CEN knowledge. The dual-script approach
  avoids the "do everything in one god-command" anti-pattern seen in tools like
  PLEXOS's monolithic importer.

- **Explicit `__unattributed__` and `__demand_fail__` sentinel rows** (§4.5) are
  a good UX decision: they mean downstream consumers never silently lose a cell
  — every output row is meaningful. This matches the project's existing
  `feedback_user_constraint_strict` pattern.

- **The `manifest.json` audit trail** (§3.3.3) gives Persona B a traceable chain
  from a `marginal_units.parquet` artifact back to the CEN fetch that produced it.
  This is exactly what a CEN-style audit workflow needs.

- **Tolerance overrides** (`--tol-price`, `--tol-flow`, `--tol-mu`, `--eps`) are
  all present at the CLI level. A researcher doing sensitivity studies can
  reproduce Fig. 3 from Hogan/Litvinov just by varying `--tol-price`. This is
  a first-class research affordance.

- **Exit-code 4 + `.partial` file** for `cen2gtopt` network errors is a good
  DX choice: it lets a batch script distinguish "CEN was unreachable" from "my
  date range was wrong" without parsing stderr.

- **`degenerate=True` + `reason` string** in the output schema (§4.5, §4.6) give
  Persona A a machine-readable audit trail for LP-degeneracy cases rather than
  hiding them behind a silent fallback. This is substantially better than PLP's
  `cmgnal` output, which does not distinguish degenerate from clean cells.

---

## 7. Opportunities

- **Auto-detection of `--input-kind`** from the presence/absence of `--feed` vs
  `--planning` + `--output` would eliminate the most common first-time user error
  (P0-2).

- **`--dry-run` flag** on `cen2gtopt` that prints the endpoints that *would* be
  fetched, the cache state, and the expected output size, then exits 0 — useful
  for audit trails and CI smoke tests.

- **`--check-feed` flag** on `gtopt_marginal_units` that validates the canonical
  feed schema (calls `verify_feed` from `_canonical_feed`) without running the
  classification — a fast pre-flight check for operators.

- **Machine-readable summary mode** (`--json-summary`): a structured JSON line per
  zone with `{zone_id, lmp, marginal_unit, confidence, degenerate}` would let the
  guiservice Phase-3 panel consume output without parsing the full Parquet.

- **A `--explain UNIT_NAME` flag** that runs the classifier for a single named
  unit across all cells and prints why it was or was not marginal in each — a
  "debugging lens" that is far more useful than reading raw Parquet rows.

---

## 8. Option/doc consistency summary

| Option | In §5/§9.5 | In §6/§7 | Mismatch |
|---|---|---|---|
| `--from` / `--to` | §9.5 date range | n/a | Conflicts with `cen_demanda`'s `--start`/`--end` |
| `--api-key` | §9.5 | n/a | Env var `$CEN_API_KEY` conflicts with `cen_demanda`'s `$CEN_USER_KEY` |
| `--out` | §5, §9.5 | n/a | All peer scripts use `--output`; inconsistent |
| `--cen-tz` | absent from §9.5 | §6.11 | Conflicts with `--source-tz` in §9.5; same flag? |
| `--plp-compare` | absent from §5 | §7.4 | Undocumented public flag |
| `confidence=published` | absent from §4.6 | §4.8.2 | Fourth enum value missing from schema |
| `cells_within(±0.5 USD/MWh)` | §4.7 R5 | n/a | Invalid identifier; must be renamed |
| `reduced_cost` | §4.6 | n/a | Semantic mismatch with LP reduced cost definition |
| Exit code 1 | absent | n/a | Not defined; peers use code 1 for quality failures |

---

## 9. Comparative benchmarks

`gtopt_marginal_units` represents a meaningfully stronger UX than comparable open
tools in the power-systems space. PyPSA's `network.lopf()` produces LMPs but
provides no built-in marginal-unit attribution; users must write their own
post-processing. GenX and Temoa both produce dual variables but offer no
congestion-zone partition nor a per-cell attribution artifact. Calliope's output
is Parquet but unschematized. PLEXOS ships a monolithic Windows-centric GUI that
conflates fetch, solve, and attribution into one non-auditable step. The canonical
feed schema + manifest approach in this plan is architecturally superior to all of
them for auditability. The main gap versus, say, CAISO's OASIS API tooling is the
absence of real-time streaming (explicitly deferred to §10) and the single-script
CLI paradigm (no notebook-native widget). The `degenerate` flag and the confidence
taxonomy are features none of the peer tools expose at all — that is a genuine
differentiator for Persona A.

---

## 10. Verdict

`UX VERDICT: NEEDS WORK`

Four P0 issues (conflicting date-range flag names, non-discoverable `--input`
enum, invisible mode/input cross-constraints, and a duplicate auth env-var) must
be resolved before the CLI contracts are finalised — they are cheapest to fix now,
before any code exists, and would cause silent user errors if left in.
