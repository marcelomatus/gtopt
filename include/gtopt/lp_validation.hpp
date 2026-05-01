/**
 * @file      lp_validation.hpp
 * @brief     Build-time LP numerical validation: thresholds and accumulator
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides LpValidationOptions (thresholds for what counts as a "huge" /
 * "tiny" coefficient, bound, RHS, or objective) and LpValidationStats
 * (accumulator that emits one spdlog::warn() per offending kind, capped to
 * avoid million-line spam on truly broken LPs).
 *
 * The validation is opt-in via LpMatrixOptions::validation. When enabled,
 * every write into the LP (LinearInterface::add_col, add_row, add_rows,
 * set_coeff, set_*, set_obj_coeff, ...) is classified once at insertion
 * time. The cost is one branch + one comparison per coefficient when off,
 * one comparison + one count bump on hits.
 *
 * This complements (does not replace) the post-flatten stats sweep in
 * lp_stats.hpp: that path runs once at the end and computes global ratios;
 * this path catches the moment a 1e15 coefficient lands on a row, naming
 * the location.
 */

#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include <boost/container/small_vector.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

namespace spdlog
{
class logger;
}

namespace gtopt
{

/**
 * @struct LpValidationOptions
 * @brief Thresholds + on/off flag for incremental LP validation.
 *
 * Mirrors LpMatrixOptions in shape: optional fields with merge semantics
 * (first-value-wins). Defaults are chosen so that a "well-conditioned"
 * IEEE benchmark stays silent and a coefficient like 1e15 on a balance
 * row triggers a single line per kind.
 */
struct LpValidationOptions
{
  /// Master switch.  When unset, defaults to true at the consumer site
  /// — the validation always runs unless explicitly disabled.
  OptBool enable {};

  /// |coeff| above this triggers a "huge coefficient" warning.
  /// Default 1e10 — well above the 1e7 ratio that lp_stats already
  /// flags as "poor", so we only warn on values that will likely
  /// confuse the solver.
  OptReal coeff_warn_max {};

  /// |coeff| below this (but > 0) triggers a "tiny coefficient" warning.
  /// Default 1e-10 — matches lp_stats stats_eps.  These are the values
  /// that will round to zero in the LP file and silently disappear.
  OptReal coeff_warn_min {};

  /// Finite |bound| above this triggers a "huge bound" warning.
  /// Default 1e12 — solver infinity sentinels (DblMax, +/- backend
  /// infinity) are skipped at the call site, so this only flags
  /// truly large finite numbers that the user probably didn't mean
  /// to write.
  OptReal bound_warn_max {};

  /// Finite |RHS| above this triggers a "huge RHS" warning.
  /// Default 1e12 — same rationale as bound_warn_max.
  OptReal rhs_warn_max {};

  /// |obj coefficient| above this triggers a "huge objective" warning.
  /// Default 1e10.
  OptReal obj_warn_max {};

  /// Maximum number of warnings emitted per kind before falling silent.
  /// Default 50 — enough to give the user pointers to fix the problem,
  /// not so many that a broken LP fills the log.  After the cap is
  /// reached the count keeps incrementing (visible in the summary)
  /// but no more lines are printed.
  OptInt max_warnings_per_kind {};

  /// Merge optional fields from another LpValidationOptions.
  /// First-value-wins (mirrors LpMatrixOptions::merge).
  constexpr void merge(const LpValidationOptions& other) noexcept
  {
    merge_opt(enable, other.enable);
    merge_opt(coeff_warn_max, other.coeff_warn_max);
    merge_opt(coeff_warn_min, other.coeff_warn_min);
    merge_opt(bound_warn_max, other.bound_warn_max);
    merge_opt(rhs_warn_max, other.rhs_warn_max);
    merge_opt(obj_warn_max, other.obj_warn_max);
    merge_opt(max_warnings_per_kind, other.max_warnings_per_kind);
  }

  // ── Effective-value accessors ──
  // Inline so `note_*` paths stay in the hot path and the compiler
  // can constant-fold the defaults when the option is unset.

  [[nodiscard]] constexpr bool effective_enable() const noexcept
  {
    return enable.value_or(true);
  }

  [[nodiscard]] constexpr double effective_coeff_warn_max() const noexcept
  {
    return coeff_warn_max.value_or(1e10);
  }

  [[nodiscard]] constexpr double effective_coeff_warn_min() const noexcept
  {
    return coeff_warn_min.value_or(1e-10);
  }

  [[nodiscard]] constexpr double effective_bound_warn_max() const noexcept
  {
    return bound_warn_max.value_or(1e12);
  }

  [[nodiscard]] constexpr double effective_rhs_warn_max() const noexcept
  {
    return rhs_warn_max.value_or(1e12);
  }

  [[nodiscard]] constexpr double effective_obj_warn_max() const noexcept
  {
    return obj_warn_max.value_or(1e10);
  }

  [[nodiscard]] constexpr int effective_max_warnings_per_kind() const noexcept
  {
    return max_warnings_per_kind.value_or(50);
  }
};

/**
 * @struct LpValidationStats
 * @brief Per-kind counters + bounded list of offenders + max/min trackers.
 *
 * Each note_* method is the central hook called from the LinearInterface
 * write path. They:
 *   - bump the per-kind count,
 *   - if under the warning cap and the value is out of band, emit a
 *     single spdlog::warn line with the location (column / row name,
 *     index, or both),
 *   - record the value in `first_huge_*` if room remains.
 *
 * The struct is cheap to construct and lives on LinearInterface.  When
 * the LP completes, log_summary() emits one info line summarising the
 * counts (or stays silent when nothing tripped).
 */
struct LpValidationStats
{
  /// Maximum number of offending values stored verbatim per kind.
  /// Beyond this only counts are kept.
  static constexpr size_t k_offenders_capped = 16;

  struct Offender
  {
    std::string location;
    double value {0.0};
  };

  using OffenderList =
      boost::container::small_vector<Offender, k_offenders_capped>;

  /// Per-kind counts (always incremented, even past the warn cap).
  size_t coeff_huge_count {};
  size_t coeff_tiny_count {};
  size_t coeff_filtered_count {};
  size_t bound_huge_count {};
  size_t rhs_huge_count {};
  size_t obj_huge_count {};

  /// First k_offenders_capped offenders for each kind (for the summary).
  OffenderList first_huge_coeffs {};
  OffenderList first_tiny_coeffs {};
  OffenderList first_filtered_coeffs {};
  OffenderList first_huge_bounds {};
  OffenderList first_huge_rhs {};
  OffenderList first_huge_obj {};

  /// Min/max |coefficient| seen across all note_coeff calls (for the
  /// summary verdict).  These are always tracked when validation is on,
  /// regardless of the warn thresholds.
  double max_abs_coeff {0.0};
  double min_abs_coeff {std::numeric_limits<double>::infinity()};

  // ── Hooks ─────────────────────────────────────────────────────────
  // Each hook is no-op when validation is disabled.  The first-band
  // check is a single comparison; the warning emission is gated on
  // the per-kind cap.

  void note_coeff(double v,
                  std::string_view location,
                  const LpValidationOptions& cfg);
  void note_bound(double v,
                  std::string_view location,
                  const LpValidationOptions& cfg);
  void note_rhs(double v,
                std::string_view location,
                const LpValidationOptions& cfg);
  void note_obj(double v,
                std::string_view location,
                const LpValidationOptions& cfg);

  /// `original` was filtered out by the matrix `eps` even though it was
  /// non-zero.  Bookkeeping: if the dropped value is large enough to be
  /// physically meaningful, a warning is emitted; otherwise only the
  /// count is bumped.
  void note_filtered(double original,
                     double eps,
                     std::string_view location,
                     const LpValidationOptions& cfg);

  /// Emit one spdlog::info summary line covering all six counters.
  /// Stays silent when every count is zero (clean LP).
  void log_summary(spdlog::logger& lg) const;

  /// Convenience wrapper using spdlog's default logger.
  void log_summary() const;

  /// Total number of warnings emitted (across all kinds).  Used by
  /// the post-equilibration verdict to decide whether to escalate
  /// from info to warn.
  [[nodiscard]] constexpr size_t total_count() const noexcept
  {
    return coeff_huge_count + coeff_tiny_count + coeff_filtered_count
        + bound_huge_count + rhs_huge_count + obj_huge_count;
  }

  /// Returns true when no validation event has been recorded.
  [[nodiscard]] constexpr bool clean() const noexcept
  {
    return total_count() == 0;
  }

  /// Reset all counters (used between phases / scenes).
  constexpr void reset() noexcept
  {
    coeff_huge_count = 0;
    coeff_tiny_count = 0;
    coeff_filtered_count = 0;
    bound_huge_count = 0;
    rhs_huge_count = 0;
    obj_huge_count = 0;
    first_huge_coeffs.clear();
    first_tiny_coeffs.clear();
    first_filtered_coeffs.clear();
    first_huge_bounds.clear();
    first_huge_rhs.clear();
    first_huge_obj.clear();
    max_abs_coeff = 0.0;
    min_abs_coeff = std::numeric_limits<double>::infinity();
  }
};

}  // namespace gtopt
