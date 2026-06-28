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

#include <gtopt/domain_rules.hpp>
#include <gtopt/error.hpp>
#include <gtopt/monolithic_options.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class LinearInterface;
struct FlatLinearProblem;
struct MipStartContext;

namespace detail
{
/// Round `v` to the nearest integer biased by `threshold` (a relaxed binary at
/// `v` rounds up iff `v >= threshold`), then clamp to `[lb, ub]`.  The generic,
/// domain-agnostic rounding step (stage 1) — any solver does this.
[[nodiscard]] double round_with_threshold(double v,
                                          double threshold,
                                          double lb,
                                          double ub) noexcept;

/// Stage 1 + 2 of the MIP-start pipeline: round the relaxation's integer
/// columns (`round_with_threshold`) and apply the default electric-system
/// commitment-repair pipeline (`make_default_domain_rules`) to the result.
/// Continuous columns are kept at their relaxation value.  Returns the dense
/// raw candidate start (empty iff the relaxation has no primal).  Shared by
/// every base generator (lp_round / relax_fix / file) so they all get the same
/// round + electric-rules front-end.
[[nodiscard]] std::vector<double> rounded_start_with_rules(
    MipStartContext& ctx, std::string_view generator_name);
}  // namespace detail

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
  /// Per storage-injection unit (reservoir-fed hydro / battery discharge) the
  /// `PeakInjectionRule` reads — status columns + per-block peak flags.  Empty
  /// unless `mip_start.peak_injection` is enabled (the `SystemLP::resolve` hook
  /// fills it).
  std::span<const PeakInjectionInfo> injections;
  /// The flattened LP (CSC matrix + bounds + objective + integer markers),
  /// supplied for generators that build a *second* solver model in process
  /// (e.g. `scip_repair`).  Column index j here equals raw LP column j, so the
  /// produced start maps 1:1 onto the dense start vector.  `nullptr` for
  /// generators that operate purely on `li` (lp_round / relax_fix / file).
  const FlatLinearProblem* flat_lp {nullptr};
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

/// Optional SCIP repair STAGE (stage 3 of the pipeline) — composable with any
/// base method and any active solver via `mip_start.scip_repair=true`.  Build a
/// SCIP backend from `ctx.flat_lp`, hand it the `candidate` commitment through
/// `set_mip_start(repair)` (→ the SCIP plugin's completesol/repair), and return
/// the candidate with SCIP's repaired integer columns overlaid.  Returns
/// `std::nullopt` (caller keeps the pre-SCIP candidate) when the SCIP plugin or
/// the flat LP is absent, or SCIP finds nothing feasible.  Defined in
/// `mip_start_scip.cpp` — the core links no SCIP headers.
[[nodiscard]] std::optional<std::vector<double>> scip_repair_candidate(
    MipStartContext& ctx, std::vector<double> candidate);

/// Run the two-stage MIP-start pipeline on `li` immediately before its MIP
/// solve.  `base_opts` are the effective monolithic solver options (the
/// relaxation solve overlays `opts.relax_solver_options` on top).
///
/// @return A report on success (including the relaxation-only / no-op cases),
///         or an `Error` when the relaxation is infeasible and the policy is
///         `stop` or `feasopt` (the caller must NOT proceed to the MIP solve).
/// `flat_lp` (optional) is the flattened LP that the active backend was loaded
/// from — required only by `scip_repair`, which builds a second backend from
/// it; `nullptr` for every other method.
[[nodiscard]] std::expected<MipStartReport, Error> apply_mip_start(
    LinearInterface& li,
    const SolverOptions& base_opts,
    const MipStartOptions& opts,
    std::span<const CommitmentRunInfo> commitments = {},
    std::span<const PeakInjectionInfo> injections = {},
    const FlatLinearProblem* flat_lp = nullptr);

/// Dump the live MIP solution's integer-column RAW values to `path`, one
/// `<index> <value>` line per integer column, preceded by an `ncols` header
/// for a structural sanity check on replay.  Call AFTER a successful MIP solve
/// (integrality intact, before any dual-recovery relaxation).  A later run
/// replays the dump as a cross-solver MIP start via `MipStartMethod::file`
/// (`FileMipStart`): both runs build the identical deterministic flat LP, so
/// raw column indices line up 1:1.
///
/// @return Empty on success, or a `FileIOError` if the file cannot be written.
[[nodiscard]] std::expected<void, Error> dump_integer_solution(
    const LinearInterface& li, const std::string& path);

}  // namespace gtopt
