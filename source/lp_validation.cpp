/**
 * @file      lp_validation.cpp
 * @brief     Implementation of build-time LP validation hooks
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cmath>
#include <format>
#include <string_view>

#include <gtopt/lp_validation.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

inline void push_offender(LpValidationStats::OffenderList& list,
                          std::string_view location,
                          double value)
{
  if (list.size() < LpValidationStats::k_offenders_capped) {
    list.push_back(LpValidationStats::Offender {
        .location = std::string {location},
        .value = value,
    });
  }
}

}  // namespace

void LpValidationStats::note_coeff(double v,
                                   std::string_view location,
                                   const LpValidationOptions& cfg)
{
  if (!cfg.effective_enable()) {
    return;
  }
  const double abs_v = std::abs(v);
  if (abs_v == 0.0) {
    return;
  }

  // Track running min/max for the summary verdict.
  if (abs_v > max_abs_coeff) {
    max_abs_coeff = abs_v;
  }
  if (abs_v < min_abs_coeff) {
    min_abs_coeff = abs_v;
  }

  const auto cap = static_cast<size_t>(cfg.effective_max_warnings_per_kind());

  if (abs_v > cfg.effective_coeff_warn_max()) {
    if (coeff_huge_count < cap) {
      spdlog::warn(
          "LP_VALIDATION huge coefficient |a|={:.3e} at {}", v, location);
    }
    push_offender(first_huge_coeffs, location, v);
    ++coeff_huge_count;
  } else if (abs_v < cfg.effective_coeff_warn_min()) {
    if (coeff_tiny_count < cap) {
      spdlog::warn(
          "LP_VALIDATION tiny coefficient |a|={:.3e} at {}", v, location);
    }
    push_offender(first_tiny_coeffs, location, v);
    ++coeff_tiny_count;
  }
}

void LpValidationStats::note_bound(double v,
                                   std::string_view location,
                                   const LpValidationOptions& cfg)
{
  if (!cfg.effective_enable()) {
    return;
  }
  if (!std::isfinite(v)) {
    return;
  }
  const double abs_v = std::abs(v);
  if (abs_v <= cfg.effective_bound_warn_max()) {
    return;
  }

  const auto cap = static_cast<size_t>(cfg.effective_max_warnings_per_kind());
  if (bound_huge_count < cap) {
    spdlog::warn("LP_VALIDATION huge bound |b|={:.3e} at {}", v, location);
  }
  push_offender(first_huge_bounds, location, v);
  ++bound_huge_count;
}

void LpValidationStats::note_rhs(double v,
                                 std::string_view location,
                                 const LpValidationOptions& cfg)
{
  if (!cfg.effective_enable()) {
    return;
  }
  if (!std::isfinite(v)) {
    return;
  }
  const double abs_v = std::abs(v);
  if (abs_v <= cfg.effective_rhs_warn_max()) {
    return;
  }

  const auto cap = static_cast<size_t>(cfg.effective_max_warnings_per_kind());
  if (rhs_huge_count < cap) {
    spdlog::warn("LP_VALIDATION huge RHS |r|={:.3e} at {}", v, location);
  }
  push_offender(first_huge_rhs, location, v);
  ++rhs_huge_count;
}

void LpValidationStats::note_obj(double v,
                                 std::string_view location,
                                 const LpValidationOptions& cfg)
{
  if (!cfg.effective_enable()) {
    return;
  }
  const double abs_v = std::abs(v);
  if (abs_v <= cfg.effective_obj_warn_max()) {
    return;
  }

  const auto cap = static_cast<size_t>(cfg.effective_max_warnings_per_kind());
  if (obj_huge_count < cap) {
    spdlog::warn("LP_VALIDATION huge objective |c|={:.3e} at {}", v, location);
  }
  push_offender(first_huge_obj, location, v);
  ++obj_huge_count;
}

void LpValidationStats::note_filtered(double original,
                                      double eps,
                                      std::string_view location,
                                      const LpValidationOptions& cfg)
{
  if (!cfg.effective_enable()) {
    return;
  }
  // Only flag when the dropped value is non-trivial.  Anything below
  // 1e-15 is rounding dust the user already implicitly accepts.
  if (std::abs(original) < 1e-15) {
    return;
  }

  const auto cap = static_cast<size_t>(cfg.effective_max_warnings_per_kind());
  if (coeff_filtered_count < cap) {
    spdlog::warn(
        "LP_VALIDATION coefficient {:.3e} filtered to 0 (eps={:.3e}) at {}",
        original,
        eps,
        location);
  }
  push_offender(first_filtered_coeffs, location, original);
  ++coeff_filtered_count;
}

void LpValidationStats::log_summary(spdlog::logger& lg) const
{
  if (clean()) {
    return;
  }
  lg.info(
      "LP_VALIDATION summary: coeff_huge={} coeff_tiny={} filtered={} "
      "bound_huge={} rhs_huge={} obj_huge={} |coeff|=[{:.3e},{:.3e}]",
      coeff_huge_count,
      coeff_tiny_count,
      coeff_filtered_count,
      bound_huge_count,
      rhs_huge_count,
      obj_huge_count,
      (min_abs_coeff == std::numeric_limits<double>::infinity())
          ? 0.0
          : min_abs_coeff,
      max_abs_coeff);
}

void LpValidationStats::log_summary() const
{
  if (auto logger = spdlog::default_logger(); logger) {
    log_summary(*logger);
  }
}

}  // namespace gtopt
