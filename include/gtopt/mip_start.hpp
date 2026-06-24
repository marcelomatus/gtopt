/**
 * @file      mip_start.hpp
 * @brief     Initial-MIP-solution (warm-start) generation & injection pipeline
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A generalized, solver-agnostic framework that, before the monolithic MIP
 * solve, runs two independently-toggleable stages:
 *
 *   A. **Relaxation analysis** — solve the LP relaxation (under its own solver
 *      options), check feasibility, optionally report the saturated / binding
 *      constraints, and apply the `on_infeasible` policy (stop / warn /
 *      feasopt-diagnose).
 *   B. **Integer injection** — when a `MipStartMethod` is configured, compute a
 *      starting integer solution from the relaxation via a pluggable
 *      `MipStartGenerator` and inject it as a backend MIP start.
 *
 * The pipeline operates IN PLACE on the live `LinearInterface` (relax → solve →
 * round → restore integrality), never cloning — see the design note in
 * `apply_mip_start`.  All start vectors are dense and in RAW LP column space.
 */

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/error.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class LinearInterface;

/// One commitment unit's status (u) columns in chronological order, with the
/// matching block durations and the unit's min-up/min-down times (hours).  This
/// is the domain input the min-up/down repair needs — built by the
/// `SystemLP::resolve` hook (one entry per (scenario, commitment) with a
/// run-length limit).  `status_cols[t]` is a RAW column index;
/// `durations[t]` is the block duration [h] of that period (parallel arrays).
struct CommitmentRunInfo
{
  double min_up_hours {0.0};
  double min_down_hours {0.0};
  std::vector<int> status_cols {};
  std::vector<double> durations {};
};

/// Outcome of the MIP-start pipeline (for logging / tests).
struct MipStartReport
{
  bool relaxation_solved {false};  ///< stage A solved the LP relaxation
  bool relaxation_feasible {false};  ///< the relaxation was feasible
  bool injected {false};  ///< a start was accepted by the backend
  std::optional<double> relax_obj {};  ///< LP-relaxation objective (if solved)
  std::string source {};  ///< generator name that produced the start
};

/// Context handed to a generator.  Precondition: `li` holds an OPTIMAL
/// LP-relaxation solution and its integer columns are currently relaxed to
/// continuous; `int_cols` is the snapshot of those columns' raw indices.
struct MipStartContext
{
  LinearInterface& li;
  const SolverOptions& relax_opts;
  std::span<const int> int_cols;
  const MipStartOptions& opts;
  std::span<const CommitmentRunInfo> commitments;
};

/// Pluggable strategy that turns the solved LP relaxation into a dense
/// raw-space starting column vector (size == `get_numcols()`).
class MipStartGenerator
{
public:
  MipStartGenerator() = default;
  virtual ~MipStartGenerator() = default;
  MipStartGenerator(const MipStartGenerator&) = delete;
  MipStartGenerator& operator=(const MipStartGenerator&) = delete;
  MipStartGenerator(MipStartGenerator&&) = delete;
  MipStartGenerator& operator=(MipStartGenerator&&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Produce the dense raw-space start vector, or `std::nullopt` to skip.
  [[nodiscard]] virtual std::optional<std::vector<double>> generate(
      MipStartContext& ctx) = 0;
};

/// Factory: build the generator for a method, or `nullptr` when the method is
/// `none` or not yet implemented (caller skips injection).
[[nodiscard]] std::unique_ptr<MipStartGenerator> make_mip_start_generator(
    MipStartMethod method);

/// Run the two-stage MIP-start pipeline on `li` immediately before its MIP
/// solve.  `base_opts` are the effective monolithic solver options (the
/// relaxation solve overlays `opts.relax_solver_options` on top).
///
/// @return A report on success (including the relaxation-only / no-op cases),
///         or an `Error` when the relaxation is infeasible and the policy is
///         `stop` or `feasopt` (the caller must NOT proceed to the MIP solve).
[[nodiscard]] std::expected<MipStartReport, Error> apply_mip_start(
    LinearInterface& li,
    const SolverOptions& base_opts,
    const MipStartOptions& opts,
    std::span<const CommitmentRunInfo> commitments = {});

}  // namespace gtopt
