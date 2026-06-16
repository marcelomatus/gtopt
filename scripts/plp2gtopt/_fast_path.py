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
) -> None:
    """Apply the iterative SDDP fast-path defaults in place.

    Every field is a *default*: an explicit value already present in the
    target dicts (or seeded from the optional ``src_*`` planning dicts on
    the writer-direct path) wins. ``src_model`` / ``src_sddp`` are the
    source planning's ``model_options`` / ``sddp_options`` (empty on the
    CLI path, where there is nothing to inherit from).
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

    fwd = sddp_opts.get("forward_solver_options")
    if fwd is None:
        fwd = dict(src_sddp.get("forward_solver_options") or {})
    fwd.setdefault("algorithm", "dual")
    fwd.setdefault("advanced_basis", True)
    sddp_opts["forward_solver_options"] = fwd

    bwd = sddp_opts.get("backward_solver_options")
    if bwd is None:
        bwd = dict(src_sddp.get("backward_solver_options") or {})
    bwd.setdefault("algorithm", "dual")
    sddp_opts["backward_solver_options"] = bwd
