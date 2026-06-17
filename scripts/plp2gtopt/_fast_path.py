# SPDX-License-Identifier: BSD-3-Clause
"""Iterative SDDP fast-path defaults for the plp2gtopt pipeline.

Single home for the benchmarked PLP-faithful SDDP config so the two entry
points stay in lock-step:

* ``main.build_options``        — the CLI path
* ``GTOptWriter.process_options`` — the writer-direct path (a raw opts dict
  handed straight to ``convert_plp_case`` with no CLI)

The config (benchmarked to ~PLP parity on the CEN65 2-year case):

* ``model_options.lp_reduction``        — elide provably-zero LP columns (~-19% wall)
* ``sddp_options.aperture_solve_mode``  = ``warm`` (dual aperture warm-start)
* ``sddp_options.aperture_chunk_size``  = ``-1`` (all apertures/phase per chunk,
  one LP clone, warm-start reuse)
* forward/backward solver ``algorithm`` = ``dual`` (+ ``advanced_basis`` forward)
* ``sddp_options.low_memory_mode``       = ``off`` (the reference oracle —
  off never diverges, so the fast dual+warm config is correct there).
  ``--solver-invariant`` flips this to ``compress`` and swaps in the
  barrier/no-crossover forward config (memory savings WITH reproducibility).

The bundled ``cplex.prm`` is retuned to dual / no-presolve to match (see
``plp2gtopt.install_solver_param_files``).

``cut_sharing_mode`` is intentionally NOT set here: the two callers target
different dicts — the CLI threads it through the top-level ``opts`` dict,
the writer sets it directly on ``sddp_options`` — so each owns that one line.
"""

from __future__ import annotations

from typing import Any

# Planning methods that receive the iterative fast-path defaults.
# ``monolithic`` is excluded (no aperture / SDDP machinery).
FAST_PATH_METHODS: frozenset[str] = frozenset({"sddp", "cascade", "cascade-reduced"})


def apply_iterative_fast_path(
    model_opts: dict[str, Any],
    sddp_opts: dict[str, Any],
    *,
    src_model: dict[str, Any] | None = None,
    src_sddp: dict[str, Any] | None = None,
    invariant: bool = False,
) -> None:
    """Apply the iterative SDDP fast-path defaults in place.

    Every field is a *default*: an explicit value already present in the
    target dicts (or seeded from the optional ``src_*`` planning dicts on
    the writer-direct path) wins. ``src_model`` / ``src_sddp`` are the
    source planning's ``model_options`` / ``sddp_options`` (empty on the
    CLI path, where there is nothing to inherit from).

    ``invariant`` selects the forward/backward LP algorithm:

    * ``False`` (default) — **dual simplex + warm-start** (``advanced_basis``
      on the forward).  Fastest, but the optimal *basis* is non-unique under
      LP degeneracy, so ``low_memory_mode`` off vs compress can land on
      different (equal-cost) vertices → non-reproducible per-cell LPs.

    * ``True`` — **barrier without crossover** (``crossover=False``).  Barrier
      converges to the unique analytic-center point and the cut path consumes
      that solve's unique interior duals directly (``crossover=False`` is the
      single knob — ``LinearInterface::ensure_duals`` does no lazy crossover),
      so off ≡ compress (bit-identical trajectories) regardless of how the LP
      is presented to the solver.  ``presolve`` is left at its default (ON):
      cold barrier re-factorizes from scratch every solve, so presolve
      shrinking the LP is pure benefit (unlike the dual+warm default, where
      presolve is disabled to preserve the warm basis).  Slower per solve and
      yields a different (equally valid) solution than the simplex vertex.
      Pair with the matching ``cplex.prm`` retune
      (``install_solver_param_files(..., invariant=True)``).
    """
    src_model = src_model or {}
    src_sddp = src_sddp or {}

    if src_model.get("lp_reduction") is not None:
        model_opts["lp_reduction"] = src_model["lp_reduction"]
    else:
        model_opts.setdefault("lp_reduction", True)

    sddp_opts.setdefault(
        "aperture_solve_mode", src_sddp.get("aperture_solve_mode") or "warm"
    )
    if "aperture_chunk_size" not in sddp_opts:
        acs = src_sddp.get("aperture_chunk_size")
        sddp_opts["aperture_chunk_size"] = -1 if acs is None else acs

    # Memory mode default.  The fast dual+warm config is correct only under
    # low_memory_mode=off (the reference oracle — off never diverges).  Under
    # compress the same config diverges from off, so couple compress with the
    # invariant solver config: default => OFF (fast + correct), and
    # --solver-invariant => compress (memory savings WITH reproducibility,
    # paired with the barrier/no-crossover forward config below).  Overridable
    # by an explicit JSON low_memory_mode or by gtopt's --memory-saving (which,
    # when passed, wins; when omitted, this JSON value stands).
    sddp_opts.setdefault(
        "low_memory_mode",
        src_sddp.get("low_memory_mode") or ("compress" if invariant else "off"),
    )

    fwd = sddp_opts.get("forward_solver_options")
    if fwd is None:
        fwd = dict(src_sddp.get("forward_solver_options") or {})
    bwd = sddp_opts.get("backward_solver_options")
    if bwd is None:
        bwd = dict(src_sddp.get("backward_solver_options") or {})

    if invariant:
        # off==compress reproducibility, HYBRID:
        #   * FORWARD — barrier + crossover=False (+ presolve ON).  Barrier
        #     reaches the unique analytic-center point, so the forward trial
        #     reservoir trajectory is deterministic / presentation-independent
        #     (this is where the off vs compress divergence was seeded).
        #     crossover=False keeps the unique interior duals; presolve is ON
        #     because cold barrier re-factorizes every solve, so presolve
        #     shrinking the LP is pure speedup.
        #   * BACKWARD apertures — warm-start dual + presolve OFF (the fast
        #     default).  The aperture pass (aperture_solve_mode=warm) honors
        #     these, reusing the basis across a chunk (aperture_chunk_size=-1).
        #     Cheap; safe IFF the aperture duals are unique — empirically
        #     validated per case (the seed was the forward primal, not the
        #     cut duals).  Falls back to forward's barrier on both passes if a
        #     case turns out to need invariant backward duals too.
        fwd.setdefault("algorithm", "barrier")
        fwd.setdefault("crossover", False)
        fwd.setdefault("presolve", True)
        bwd.setdefault("algorithm", "dual")
        bwd.setdefault("advanced_basis", True)
        bwd.setdefault("presolve", False)
    else:
        fwd.setdefault("algorithm", "dual")
        fwd.setdefault("advanced_basis", True)
        bwd.setdefault("algorithm", "dual")

    sddp_opts["forward_solver_options"] = fwd
    sddp_opts["backward_solver_options"] = bwd
