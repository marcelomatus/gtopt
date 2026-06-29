/**
 * @file      linear_interface_solve.cpp
 * @brief     LinearInterface solve dispatch — initial_solve / resolve.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from `linear_interface.cpp`.  Holds the algorithm-fallback
 * helper (`next_fallback_algo`), the `initial_solve` and `resolve`
 * entry points, and the lazy-crossover / status helpers
 * (`ensure_duals`, `get_status`, `get_kappa`, `diagnose_row`).  All of
 * these share the timed-solve / fallback-cycle / log-guard scaffolding,
 * so they live together in their own translation unit.
 */

#include <algorithm>
#include <chrono>
#include <expected>
#include <format>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// Accumulate one solve's effort into the process-global SolveEffortTotals.
/// Generic across all backends: prefer the backend's native solver time +
/// ticks (`last_solve_effort`); fall back to the measured @p wall seconds
/// when the backend reports none, and to seconds when it reports no ticks.
void accumulate_solve_effort(const SolverBackend& backend, double wall)
{
  const auto eff = backend.last_solve_effort();
  const double seconds = eff.seconds > 0.0 ? eff.seconds : wall;
  const double ticks = eff.ticks > 0.0 ? eff.ticks : seconds;
  SolveEffortTotals::instance().add(seconds, ticks);
}

/// Fold @p scale_objective into the solver's reduced-cost optimality
/// tolerance (CPLEX ``EPOPT`` / HiGHS dual-feasibility tolerance).
///
/// The optimality tolerance is applied by the backend in **raw**
/// LP-objective space, but ``flatten()`` divides the objective by
/// ``scale_objective`` (linear_problem.cpp), so a raw reduced cost is
/// ``rc_raw = rc_phys / scale_objective``.  A *fixed* raw tolerance ``T``
/// therefore declares optimality at a **physical** reduced-cost floor of
/// ``T × scale_objective`` — i.e. the solver gets coarser (and the
/// returned basis / reduced costs scale-dependent) as ``scale_objective``
/// grows.  This is the one place where ``scale_objective`` leaks into an
/// algorithmic decision rather than a pure objective rescaling.
///
/// Keep the *physical* floor invariant by setting the raw tolerance to
/// ``phys_eps / scale_objective``, where ``phys_eps`` is the requested
/// physical tolerance (default ``1e-6`` — the value that is correct at
/// ``scale_objective == 1`` and matches the CPLEX default).  Clamped to
/// the documented ``EPOPT`` range ``[1e-9, 1e-1]``.
///
/// No-op when ``scale_objective == 1`` (raw == physical) so backend
/// defaults are preserved for the common case (and the new
/// ``scale_objective = 1`` plp2gtopt default).
void scale_optimality_tol_for_objective(SolverOptions& opts,
                                        double scale_objective) noexcept
{
  if (scale_objective == 1.0) {
    return;
  }
  constexpr double base_phys_eps = 1e-6;  // physical floor at scale_obj == 1
  constexpr double epopt_min = 1e-9;  // CPLEX EPOPT lower clamp
  constexpr double epopt_max = 1e-1;  // CPLEX EPOPT upper clamp
  const double phys_eps = opts.optimal_eps.value_or(base_phys_eps);
  opts.optimal_eps =
      std::clamp(phys_eps / scale_objective, epopt_min, epopt_max);
}
}  // namespace

std::expected<int, Error> LinearInterface::initial_solve(
    const SolverOptions& solver_options)
{
  using Clock = std::chrono::steady_clock;

  // Reconstruct the model from the compressed snapshot before solving.  Under
  // LowMemoryMode::compress the LP is built straight into the snapshot and the
  // backend is released WITHOUT ever loading the matrix, so the live backend is
  // empty (0 rows / 0 cols).  resolve() and fix_integers_and_resolve() already
  // do this; initial_solve() must too — otherwise the empty model reaches the
  // backend (CLP/CBC null-deref in ClpPresolve → SIGSEGV; HiGHS/MindOpt error;
  // CPLEX silently "solves" nothing).  No-op when not released (compress off).
  ensure_backend();

  ++m_solver_stats_.initial_solve_calls;
  m_solver_stats_.total_ncols += get_numcols();
  m_solver_stats_.total_nrows += get_numrows();

  try {
    // Start from backend-optimal defaults, overlay user settings on top.
    auto effective = m_backend_->optimal_options();
    effective.overlay(solver_options);
    // Keep the physical reduced-cost optimality floor invariant to
    // scale_objective (the EPOPT scale-leak — see helper docstring).
    scale_optimality_tol_for_objective(effective, m_scale_objective_);
    m_last_solver_options_ = effective;

    m_backend_->apply_options(effective);

    const auto log_mode = effective.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(effective.log_level, 1)
        : effective.log_level;

    auto timed_solve = [&]
    {
      const auto t0 = Clock::now();
      if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
        const LogFileGuard log_guard(*this, m_log_file_, log_level);
        m_backend_->initial_solve();
      } else {
        const HandlerGuard guard(*this, log_level);
        m_backend_->initial_solve();
      }
      // See `resolve()` for the rationale.
      m_cache_.mark_solution_fresh(/*v=*/true);
      // Eagerly snapshot the just-solved primal/dual/reduced-cost
      // vectors into the LI cache so all downstream readers go through
      // the LI cache (single source of truth).  Plugin backends no
      // longer maintain a parallel solution cache.
      populate_solution_cache_post_solve();
      const double wall =
          std::chrono::duration<double>(Clock::now() - t0).count();
      m_solver_stats_.total_solve_time_s += wall;
      accumulate_solve_effort(*m_backend_, wall);
    };

    timed_solve();

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        spdlog::warn("{}: initial_solve non-optimal with {}, fallback to {}",
                     m_log_tag_.empty() ? get_prob_name() : m_log_tag_,
                     current_algo,
                     next_algo);

        fallback_opts.algorithm = next_algo;
        fallback_opts.presolve = false;
        // Reset aggressive scaling on fallback — aggressive scaling can
        // cause numerical difficulties (high kappa) that prevent the
        // primary algorithm from converging; the fallback algorithm may
        // succeed with the solver's default scaling strategy.
        if (fallback_opts.scaling == SolverScaling::aggressive) {
          fallback_opts.scaling = SolverScaling::automatic;
        }
        m_backend_->apply_options(fallback_opts);

        ++m_solver_stats_.fallback_solves;
        timed_solve();

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
      // Classify infeasibility for the end-of-run stats report.  Query
      // the backend directly (not get_status()) so primal/dual are
      // distinguishable — get_status() collapses them into status 2.
      ++m_solver_stats_.infeasible_count;
      if (m_backend_->is_proven_primal_infeasible()) {
        ++m_solver_stats_.primal_infeasible;
      } else if (m_backend_->is_proven_dual_infeasible()) {
        ++m_solver_stats_.dual_infeasible;
      }
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}{}",
              get_prob_name(),
              get_status(),
              effective.max_fallbacks > 0 ? " (after algorithm fallback cycle)"
                                          : ""),
          .status = get_status(),
      });
    }

    if (const auto k = m_backend_->get_kappa(); k.has_value()) {
      m_solver_stats_.max_kappa = std::max(m_solver_stats_.max_kappa, *k);
    }

    const auto status = get_status();
    cache_and_release();
    return status;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message =
            std::format("Unexpected error in initial_solve: {}", e.what()),
    });
  }
}

std::expected<int, Error> LinearInterface::resolve(
    const SolverOptions& solver_options)
{
  using Clock = std::chrono::steady_clock;

  ensure_backend();

  ++m_solver_stats_.resolve_calls;
  m_solver_stats_.total_ncols += get_numcols();
  m_solver_stats_.total_nrows += get_numrows();

  try {
    // Start from backend-optimal defaults, overlay user settings on top.
    auto effective = m_backend_->optimal_options();
    effective.overlay(solver_options);
    // Keep the physical reduced-cost optimality floor invariant to
    // scale_objective (the EPOPT scale-leak — see helper docstring).
    scale_optimality_tol_for_objective(effective, m_scale_objective_);
    m_last_solver_options_ = effective;

    m_backend_->apply_options(effective);

    const auto log_mode = effective.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(effective.log_level, 1)
        : effective.log_level;

    auto timed_solve = [&]
    {
      const auto t0 = Clock::now();
      if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
        const LogFileGuard log_guard(*this, m_log_file_, log_level);
        m_backend_->resolve();
      } else {
        const HandlerGuard guard(*this, log_level);
        m_backend_->resolve();
      }
      // Live backend's `is_proven_optimal()` / `col_solution()` now
      // reflect the just-completed solve.  Set BEFORE the
      // `is_optimal()` check below drives the fallback cycle.
      m_cache_.mark_solution_fresh(/*v=*/true);
      // Eagerly snapshot the just-solved primal/dual/reduced-cost
      // vectors into the LI cache so all downstream readers go through
      // the LI cache (single source of truth).  Plugin backends no
      // longer maintain a parallel solution cache.
      populate_solution_cache_post_solve();
      const double wall =
          std::chrono::duration<double>(Clock::now() - t0).count();
      m_solver_stats_.total_solve_time_s += wall;
      accumulate_solve_effort(*m_backend_, wall);
    };

    timed_solve();

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        // Demoted to DEBUG: these fallback-cycle lines were printing 2–3
        // consecutive warn lines per infeasible LP (barrier→dual→primal).
        // Final outcome is covered by the single "elastic ok" / "installed
        // fcut" INFO line in sddp_forward_pass.cpp, so the fallback-step
        // chatter is noise in the main log.  Still available at debug
        // level for a deep post-mortem.
        SPDLOG_DEBUG("{}: resolve non-optimal with {}, fallback to {}",
                     m_log_tag_.empty() ? get_prob_name() : m_log_tag_,
                     current_algo,
                     next_algo);

        fallback_opts.algorithm = next_algo;
        fallback_opts.presolve = false;
        // Reset aggressive scaling on fallback — aggressive scaling can
        // cause numerical difficulties (high kappa) that prevent the
        // primary algorithm from converging; the fallback algorithm may
        // succeed with the solver's default scaling strategy.
        if (fallback_opts.scaling == SolverScaling::aggressive) {
          fallback_opts.scaling = SolverScaling::automatic;
        }
        m_backend_->apply_options(fallback_opts);

        ++m_solver_stats_.fallback_solves;
        timed_solve();

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
      // Classify infeasibility for the end-of-run stats report.
      ++m_solver_stats_.infeasible_count;
      if (m_backend_->is_proven_primal_infeasible()) {
        ++m_solver_stats_.primal_infeasible;
      } else if (m_backend_->is_proven_dual_infeasible()) {
        ++m_solver_stats_.dual_infeasible;
      }
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}{}",
              get_prob_name(),
              get_status(),
              effective.max_fallbacks > 0 ? " (after algorithm fallback cycle)"
                                          : ""),
          .status = get_status(),
      });
    }

    if (const auto k = m_backend_->get_kappa(); k.has_value()) {
      m_solver_stats_.max_kappa = std::max(m_solver_stats_.max_kappa, *k);
    }

    const auto status = get_status();
    cache_and_release();
    return status;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message = std::format("Unexpected error in resolve: {}", e.what()),
    });
  }
}

std::expected<LinearInterface::FixIntegersOutcome, Error>
LinearInterface::fix_integers_and_resolve(const SolverOptions& opts)
{
  using Clock = std::chrono::steady_clock;

  ensure_backend();

  // Build the effective options the backend's internal re-solve runs
  // under: backend-optimal defaults, user settings overlaid on top, then
  // the scale-objective optimality-tolerance correction — exactly what
  // `resolve()` applies before a solve (this TU owns the helper).  The
  // common backend method re-applies these via `apply_options(opts)`
  // internally; the CPLEX override then overrides only the LP method to
  // warm dual simplex for the throwaway fixed-LP pass.
  auto effective = m_backend_->optimal_options();
  effective.overlay(opts);
  scale_optimality_tol_for_objective(effective, m_scale_objective_);
  m_last_solver_options_ = effective;

  // Delegate the fix + re-solve to the one common backend virtual.  Each
  // plugin implements it with its native mechanism (CPLEX: single-call
  // `CPXPROB_FIXEDMILP` warm dual pass; everyone else: the portable
  // pin-bounds -> relax -> resolve default).  The fix happens on the LIVE
  // backend right after the MIP solve — no compress/reconstruct between
  // fix and read — so this is intentionally NOT routed through the LI
  // replay log; the fixed LP is a throwaway whose sole product is the
  // committed-solution duals.
  const auto t0 = Clock::now();
  const int fixed = m_backend_->fix_mip_and_resolve_duals(effective);
  m_solver_stats_.total_solve_time_s +=
      std::chrono::duration<double>(Clock::now() - t0).count();

  if (fixed <= 0) {
    // fixed < 0: no incumbent / fixed-LP solve failed — preserve the
    //   historical early-return contract so the cell does not fail (the
    //   caller already warns and keeps the MIP primal).
    // fixed == 0: pure LP — caller's existing duals are already valid.
    return FixIntegersOutcome {.fixed_columns = 0, .status = std::nullopt};
  }

  // The fixed LP was solved on the live backend; refresh the LI cache the
  // same way `resolve()` does post-solve so downstream dual / reduced-cost
  // readers see the committed-solution values, not stale MIP data.
  ++m_solver_stats_.resolve_calls;
  m_cache_.mark_solution_fresh(/*v=*/true);
  populate_solution_cache_post_solve();

  return FixIntegersOutcome {
      .fixed_columns = Index {fixed},
      .status = 0,
  };
}

}  // namespace gtopt
