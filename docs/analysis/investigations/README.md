# Investigation reports — audits, fix plans, proposals

Time-stamped notes from concrete debugging / refactoring work that
shaped the current code.  Distinct from the polished evergreen analysis
docs in the parent `docs/analysis/` (benchmark results, naming
conventions, design proposals): these are working documents that
record **what was wrong**, **why we fixed it**, and **what trade-offs
were considered** during a specific investigation.

Newer notes belong here when:

* the work spanned a few days and produced multiple cross-referenced
  fixes / commits;
* the rationale is non-obvious and useful to future contributors
  (e.g. "why is `m_replaying_` flipped by an RAII guard");
* the doc cites concrete bug reproductions, log excerpts, or commit
  hashes that don't fit in inline source comments.

Single-sitting analyses or polished design proposals belong directly
under `docs/analysis/` instead.

## Index

### `sddp/` — Stochastic Dual Dynamic Programming

| File | Date | Topic |
|------|------|-------|
| [sddp_clone_mutex_audit_2026-04-30.md](sddp/sddp_clone_mutex_audit_2026-04-30.md) | 2026-04-30 | CPLEX `clone()` global-mutex audit; manual aperture-clone route via `load_flat` |
| [sddp_cut_sharing_fix_plan_2026-04-30.md](sddp/sddp_cut_sharing_fix_plan_2026-04-30.md) | 2026-04-30 | Cut-sharing correctness — heterogeneous-scene unsafety of `accumulate`/`expected`/`max` |
| [sddp_cut_store_split_plan_2026-04-30.md](sddp/sddp_cut_store_split_plan_2026-04-30.md) | 2026-04-30 | `SDDPCutStore` split into per-scene + global stores |
| [sddp_dispatch_concurrency_proposal_2026-04-30.md](sddp/sddp_dispatch_concurrency_proposal_2026-04-30.md) | 2026-04-30 | Async forward-pass dispatch, `cell_task_headroom`, work-pool tuning |

### `linear_interface/` — `LinearInterface` lifecycle and LP scaling

| File | Date | Topic |
|------|------|-------|
| [lp_scale_audit_2026-04-26.md](linear_interface/lp_scale_audit_2026-04-26.md) | 2026-04-26 | LP coefficient-scale audit on juan/iplp; root cause of LB compounding |
| [lp_audit_fix_plan_2026-04-29.md](linear_interface/lp_audit_fix_plan_2026-04-29.md) | 2026-04-29 | Follow-up fix plan from the scale audit (P0/P1/P2 items) |
| [linear_interface_lifecycle_plan_2026-04-30.md](linear_interface/linear_interface_lifecycle_plan_2026-04-30.md) | 2026-04-30 | Bulk-`add_rows` / label-meta failure mode; release/reconstruct contract |

### `scene_infeasibility/` — SDDP forward-pass infeasibility recovery

| File | Date | Topic |
|------|------|-------|
| [scene_infeasibility_rollback_plan_2026-04-30.md](scene_infeasibility/scene_infeasibility_rollback_plan_2026-04-30.md) | 2026-04-30 | `forward_infeas_rollback` design + stall-stop guard |

## Conventions

* **File name format:** `<topic>_<YYYY-MM-DD>.md`.  The date is the
  *start* of the investigation, not the merge date — the doc evolves
  along with follow-up commits while the date stays put.
* **First section of every doc:** a one-line *Status* (`draft` /
  `landed` / `superseded`) and a *Companion to:* line pointing at
  related docs in this tree, plus the parent symptom (commit hash,
  log excerpt, or bug ID).
* **Cross-references** in source comments use the full path
  `docs/analysis/investigations/<sub>/<file>.md` — keeps `git mv`
  later painless because every reference is searchable as a single
  literal string.
