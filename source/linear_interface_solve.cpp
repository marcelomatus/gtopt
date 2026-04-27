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

std::expected<int, Error> LinearInterface::initial_solve(
    const SolverOptions& solver_options)
{
  using Clock = std::chrono::steady_clock;

  ++m_solver_stats_.initial_solve_calls;
  m_solver_stats_.total_ncols += get_numcols();
  m_solver_stats_.total_nrows += get_numrows();

  try {
    // Start from backend-optimal defaults, overlay user settings on top.
    auto effective = m_backend_->optimal_options();
    effective.overlay(solver_options);
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
      m_solver_stats_.total_solve_time_s +=
          std::chrono::duration<double>(Clock::now() - t0).count();
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
      m_solver_stats_.total_solve_time_s +=
          std::chrono::duration<double>(Clock::now() - t0).count();
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

}  // namespace gtopt
