/**
 * @file      log_format.hpp
 * @brief     Centralised INFO-log number formatting helpers.
 * @date      2026-05-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One source of truth for how monetary magnitudes, durations,
 * percentages and counters render across every INFO-level log line.
 * Replaces ad-hoc `{:.4f}` / `{:.2f}%` / mixed-suffix usage that
 * historically made `tail -f` output hard to scan.
 *
 * Each helper returns a small aggregate that has a `std::formatter`
 * specialisation, so the string is materialised lazily inside
 * `std::format` / spdlog only when the level filter admits the line.
 */

#pragma once

#include <cmath>
#include <format>
#include <string_view>

#include <gtopt/sddp_common.hpp>  // FormatSI (re-exported as log::money)

namespace gtopt::log
{

/// Money / large magnitudes — `format_si` with K/M/G/T suffix.
/// Pin used at SDDP UB/LB/obj sites so 2.187G is the universal form.
[[nodiscard]] inline auto money(double v) noexcept
{
  return format_si(v);
}

/// Signed percentage with four decimals, always rendered as
/// `+N.NNNN%` or `-N.NNNN%`.  Pass the fractional gap (0.1066 →
/// "+10.6600%") so callers don't need to multiply by 100 at every
/// emit site.  Four decimals (vs the prior two) make sub-0.01 %
/// Δgap values distinguishable from rounded zero — needed by the
/// cascade convergence tables where ``stationary_tol`` reaches the
/// 0.01 %–0.025 % band on tight levels (L0 warmup, L1 uninodal).
/// At two decimals every Δgap under 0.005 % rounded to "+0.00 %"
/// and the table couldn't tell stationarity-converged iters from
/// proxy-zero ones.
struct FormatPct
{
  double frac;
};

[[nodiscard]] inline FormatPct pct(double frac) noexcept
{
  return FormatPct {frac};
}

/// Wall-clock duration in seconds, rendered with an automatically
/// chosen unit and a stable column width:
///   <1   s  → ` 423ms`
///   <60  s  → ` 47.3s`
///   <3600 s → `  9.4m`
///   else    → `  2.5h`
struct FormatDur
{
  double seconds;
};

[[nodiscard]] inline FormatDur dur(double seconds) noexcept
{
  return FormatDur {seconds};
}

}  // namespace gtopt::log

// ─── std::formatter<FormatPct> ─────────────────────────────────────────────

template<class CharT>
struct std::formatter<gtopt::log::FormatPct, CharT>  // NOLINT(cert-dcl58-cpp)
    : std::formatter<std::basic_string_view<CharT>, CharT>
{
  template<class FormatContext>
  auto format(const gtopt::log::FormatPct& f, FormatContext& ctx) const
  {
    return std::format_to(ctx.out(), "{:+8.4f}%", 100.0 * f.frac);
  }
};

// ─── std::formatter<FormatDur> ─────────────────────────────────────────────

template<class CharT>
struct std::formatter<gtopt::log::FormatDur, CharT>  // NOLINT(cert-dcl58-cpp)
    : std::formatter<std::basic_string_view<CharT>, CharT>
{
  template<class FormatContext>
  auto format(const gtopt::log::FormatDur& f, FormatContext& ctx) const
  {
    if (!std::isfinite(f.seconds) || f.seconds < 0.0) {
      return std::format_to(ctx.out(), "   ---");
    }
    if (f.seconds < 1.0) {
      return std::format_to(ctx.out(), "{:>4.0f}ms", f.seconds * 1000.0);
    }
    if (f.seconds < 60.0) {
      return std::format_to(ctx.out(), "{:>5.1f}s", f.seconds);
    }
    if (f.seconds < 3600.0) {
      return std::format_to(ctx.out(), "{:>5.1f}m", f.seconds / 60.0);
    }
    return std::format_to(ctx.out(), "{:>5.1f}h", f.seconds / 3600.0);
  }
};
