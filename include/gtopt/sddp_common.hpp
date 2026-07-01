/**
 * @file      sddp_common.hpp
 * @brief     Common types and forward declarations for the SDDP subsystem
 * @date      2026-03-21
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Lightweight header with SDDP-specific strong types, forward declarations,
 * and small structures shared by sddp_solver, sddp_aperture, sddp_cut_io,
 * and sddp_monitor.  This avoids circular includes: consumers that only need
 * the basic types include this file instead of sddp_method.hpp.
 */

#pragma once

#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>

namespace gtopt
{

// ─── Forward declarations ────────────────────────────────────────────────────

class ScenarioLP;
class SystemLP;
class PhaseLP;
struct PhaseStateInfo;

// ─── Uniform SDDP log prefix ────────────────────────────────────────────────
//
// Every SDDP info/warn log line uses:
//   "SDDP <Phase> [i<iteration> s<scene> p<phase>]: <message>"
//   "SDDP <Phase> [i<iteration> s<scene> p<phase> a<aperture>]: <message>"
//
// where <Phase> is Forward, Backward, Aperture, etc.
// The bracketed key is easy to parse in run_gtopt:
//   re.compile(r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\]")
//
// `sddp_log(...)` returns a lightweight `SDDPLogTag` aggregate (string_view
// + a few uids — no heap alloc).  When passed to `spdlog::info("{}: …", …)`
// or `std::format("{}: …", …)`, the `std::formatter<SDDPLogTag>`
// specialization at the bottom of this header materialises the
// `"SDDP Forward [i1 s1 p2]"` string ONLY when the formatter is actually
// invoked — i.e. AFTER spdlog's runtime level filter has decided to emit.
// Under `--quiet` or any sub-INFO level, no string is built.
//
// Implicit conversion to `std::string` is provided so legacy call sites
// like `li.set_log_tag(sddp_log(...))` keep working unchanged.

/// Lazy SDDP log-prefix tag.  Holds string_view + uids; materialises into
/// "SDDP Forward [i1 s1 p2]" form only when formatted.
struct SDDPLogTag
{
  std::string_view tag;
  IterationUid iteration_uid;
  std::optional<SceneUid> scene_uid;
  std::optional<PhaseUid> phase_uid;
  std::optional<Uid> aperture_uid;

  /// Eager materialisation for legacy callers (`set_log_tag(sddp_log(…))`,
  /// `auto prefix = sddp_log(…)`).  Format-arg consumers go through
  /// `std::formatter<SDDPLogTag>` instead and never hit this conversion.
  ///
  /// Implicit conversion (no `explicit`) keeps existing call sites
  /// compiling without changes.  The formatter takes precedence over
  /// the implicit conversion in `std::format` overload resolution.
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  [[nodiscard]] operator std::string() const
  {
    // Each optional is dereferenced only under its own has_value() guard,
    // mirroring the std::formatter<SDDPLogTag> specialisation below.  The
    // sddp_log(...) factories keep the nesting invariant (aperture ⊂ phase ⊂
    // scene), but checking each level independently keeps the output
    // well-formed for any construction and is provably safe to the analyzer.
    std::string result = as_label<void>("SDDP ", tag, " [i", iteration_uid);
    if (scene_uid.has_value()) {
      result += as_label<void>(' ', 's', *scene_uid);
    }
    if (phase_uid.has_value()) {
      result += as_label<void>(' ', 'p', *phase_uid);
    }
    if (aperture_uid.has_value()) {
      result += as_label<void>(' ', 'a', *aperture_uid);
    }
    result += ']';
    return result;
  }
};

/// "SDDP Forward [i1 s1 p2]" — with phase tag and per-phase key.
/// Strict-typed UID-only signature: callers pass ``IterationUid`` /
/// ``SceneUid`` / ``PhaseUid`` (1-based, matching the printed
/// ``[iN sM pK]`` format).  Passing the matching ``*Index`` (0-based,
/// internal) is a compile error — the caller must convert via
/// ``gtopt::uid_of(...)`` at the call site, mirroring the
/// ``SPDLOG_*("...[i{}]…", gtopt::uid_of(iteration_index), ...)``
/// pattern used everywhere else in the SDDP code (see PR #459).
[[nodiscard]] inline SDDPLogTag sddp_log(std::string_view tag,
                                         IterationUid iteration_uid,
                                         SceneUid scene_uid,
                                         PhaseUid phase_uid) noexcept
{
  return SDDPLogTag {
      .tag = tag,
      .iteration_uid = iteration_uid,
      .scene_uid = scene_uid,
      .phase_uid = phase_uid,
      .aperture_uid = std::nullopt,
  };
}

/// "SDDP Aperture [i1 s1 p2 a5]" — with aperture uid appended.
[[nodiscard]] inline SDDPLogTag sddp_log(std::string_view tag,
                                         IterationUid iteration_uid,
                                         SceneUid scene_uid,
                                         PhaseUid phase_uid,
                                         Uid aperture_uid) noexcept
{
  return SDDPLogTag {
      .tag = tag,
      .iteration_uid = iteration_uid,
      .scene_uid = scene_uid,
      .phase_uid = phase_uid,
      .aperture_uid = aperture_uid,
  };
}

/// "SDDP Forward [i1 s1]" — scene-level (no phase).
[[nodiscard]] inline SDDPLogTag sddp_log(std::string_view tag,
                                         IterationUid iteration_uid,
                                         SceneUid scene_uid) noexcept
{
  return SDDPLogTag {
      .tag = tag,
      .iteration_uid = iteration_uid,
      .scene_uid = scene_uid,
      .phase_uid = std::nullopt,
      .aperture_uid = std::nullopt,
  };
}

/// "SDDP Init [i1]" — iteration-level only.
[[nodiscard]] inline SDDPLogTag sddp_log(std::string_view tag,
                                         IterationUid iteration_uid) noexcept
{
  return SDDPLogTag {
      .tag = tag,
      .iteration_uid = iteration_uid,
      .scene_uid = std::nullopt,
      .phase_uid = std::nullopt,
      .aperture_uid = std::nullopt,
  };
}

// ─── Phase grid recorder ────────────────────────────────────────────────────
//
// Lightweight accumulator for per-(iteration, scene, phase) activity state.
// Each cell is a single character:
//   '.' = idle, 'F' = forward, 'B' = backward, 'E' = elastic,
//   'A' = aperture, 'X' = infeasible
//
// Thread-safe: uses a mutex since forward/backward passes may run on
// different threads.  The grid is serialised into the status JSON so
// external tools can reconstruct the TUI phase grid without parsing logs.

/// Phase grid cell states (ordered by display priority).
enum class GridCell : char
{
  Idle = '.',
  Forward = 'F',
  Backward = 'B',
  Elastic = 'E',
  Aperture = 'A',
  Infeasible = 'X',
};

class PhaseGridRecorder
{
public:
  /// Record a phase activity.  Higher-priority states overwrite lower ones.
  ///
  /// All three identifiers are stored as UIDs — the recorder never
  /// performs arithmetic on them and never assumes a particular
  /// numeric layout.  Internally, per-row activity is held in a
  /// `std::map<PhaseUid, char>` keyed by `PhaseUid`; `to_json()` emits
  /// the inner map in sorted-UID order, so the resulting `"cells"`
  /// string positions correspond to the natural PhaseUid ordering
  /// (typically 1-based contiguous, but the recorder no longer
  /// depends on that — it only requires that `PhaseUid::operator<`
  /// gives the desired column order).
  void record(IterationUid iteration_uid,
              SceneUid scene_uid,
              PhaseUid phase_uid,
              GridCell state);

  /// Return a snapshot of the grid as a JSON fragment (no trailing comma).
  /// Format: "phase_grid": {"rows": [{"i":0,"s":0,"cells":"FF.FB..."}, ...]}
  /// Cells are emitted in PhaseUid-sorted order, with '.' filling any
  /// missing UID slot in `[first_uid, last_uid]` so the position of
  /// each character corresponds to its PhaseUid offset from the
  /// minimum UID seen for the row.
  [[nodiscard]] std::string to_json() const;

  /// True when at least one cell has been recorded.
  [[nodiscard]] bool empty() const;

private:
  struct Key
  {
    IterationUid iteration_uid {};
    SceneUid scene_uid {};
    auto operator<=>(const Key&) const = default;
  };

  /// Per-row activity, keyed by PhaseUid.  `std::map` (not `flat_map`)
  /// because the recorder is debug telemetry — neither write rate nor
  /// memory footprint warrants the cache-friendlier flat variant.
  using PhaseCells = std::map<PhaseUid, char>;

  mutable std::mutex m_mutex_;
  std::map<Key, PhaseCells> m_rows_;
};

// ─── Cost / bound formatting ────────────────────────────────────────────────
//
// Render a scalar with an SI-suffix so log lines stay readable at
// planning-cost magnitude.  Examples:
//   * 1.234       → ``"1.234"``
//   * 1234.5      → ``"1.23K"``
//   * 161127348.5 → ``"161.13M"``
//   * 1.234e+09   → ``"1.234G"``
//
// Picks 2 / 3 decimal places depending on magnitude so the printed
// string stays under 8 chars in typical use.  Negative values are
// prefixed with ``-``; absolute value drives the suffix.  Used by the
// per-scene "SDDP Forward […]: done, opex=…" line, the iteration-end
// UB / LB / α summary, and the convergence headline.
/// Lazy SI-suffix wrapper.  Holds a double; materialises into the
/// formatted string only when consumed by `{}` (which fires AFTER
/// spdlog's runtime level filter) or via the implicit
/// `operator std::string()` for non-format-arg callers.
///
/// Used at ~5 sites per (iter, scene, phase) of an SDDP run: forward
/// `opex=…`, backward `cut z=…→α≥…`, tgt re-solve `z=…`.  On
/// CEN-scale (16 scen × 51 phases × 30 iter) that's ~120 K
/// `format_si` evaluations per run; at INFO+ level filter every one
/// of those allocs is wasted.
struct FormatSI
{
  double value;

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  [[nodiscard]] operator std::string() const
  {
    // Uniform 6dp across all SI bands.  See the matching block in
    // the std::formatter specialisation below for rationale.
    const double a = std::abs(value);
    const char* sign = (value < 0.0) ? "-" : "";
    if (a >= 1e12) {
      return std::format("{}{:.6f}T", sign, a / 1e12);
    }
    if (a >= 1e9) {
      return std::format("{}{:.6f}G", sign, a / 1e9);
    }
    if (a >= 1e6) {
      return std::format("{}{:.6f}M", sign, a / 1e6);
    }
    if (a >= 1e3) {
      return std::format("{}{:.6f}K", sign, a / 1e3);
    }
    return std::format("{}{:.6f}", sign, a);
  }
};

[[nodiscard]] inline FormatSI format_si(double v) noexcept
{
  return FormatSI {v};
}

}  // namespace gtopt

// ─── std::formatter<SDDPLogTag> ─────────────────────────────────────────────
//
// The formatter is invoked by `std::format` / spdlog ONLY after the
// runtime level filter has decided to emit the line — i.e. under
// `--quiet` no formatter call is made and no string is built.  The
// `sddp_log(...)` factory at the top of this header returns this
// aggregate by value (no heap), so the only allocations on the
// log path happen here, lazily.
template<class CharT>
struct std::formatter<gtopt::SDDPLogTag, CharT>  // NOLINT(cert-dcl58-cpp)
    : std::formatter<std::basic_string_view<CharT>, CharT>
{
  template<class FormatContext>
  auto format(const gtopt::SDDPLogTag& t, FormatContext& ctx) const
  {
    auto out = ctx.out();
    out = std::format_to(out, "SDDP {} [i{}", t.tag, t.iteration_uid);
    if (t.scene_uid.has_value()) {
      out = std::format_to(out, " s{}", *t.scene_uid);
    }
    if (t.phase_uid.has_value()) {
      out = std::format_to(out, " p{}", *t.phase_uid);
    }
    if (t.aperture_uid.has_value()) {
      out = std::format_to(out, " a{}", *t.aperture_uid);
    }
    *out++ = ']';
    return out;
  }
};

// ─── std::formatter<FormatSI> ──────────────────────────────────────────────
//
// Same lazy-formatter pattern as SDDPLogTag.  Materialises the
// SI-suffix string only when spdlog's runtime level filter admits the
// line — at INFO+ filter / `--quiet` mode every per-(iter, scene,
// phase) `format_si` call site (~5/cell × ~24K cells on Juan-scale)
// stops paying the alloc cost.
template<class CharT>
struct std::formatter<gtopt::FormatSI, CharT>  // NOLINT(cert-dcl58-cpp)
    : std::formatter<std::basic_string_view<CharT>, CharT>
{
  template<class FormatContext>
  auto format(const gtopt::FormatSI& f, FormatContext& ctx) const
  {
    auto out = ctx.out();
    const double a = std::abs(f.value);
    const char* sign = (f.value < 0.0) ? "-" : "";
    // Uniform 6dp across all SI bands so a column of numbers
    // printed back-to-back lines up at the decimal point.  Kept in
    // sync with the implicit-conversion `operator std::string()`
    // above (the cold path used by `std::string(format_si(v))`)
    // so the hot and cold paths produce byte-identical output.
    if (a >= 1e12) {
      return std::format_to(out, "{}{:.6f}T", sign, a / 1e12);
    }
    if (a >= 1e9) {
      return std::format_to(out, "{}{:.6f}G", sign, a / 1e9);
    }
    if (a >= 1e6) {
      return std::format_to(out, "{}{:.6f}M", sign, a / 1e6);
    }
    if (a >= 1e3) {
      return std::format_to(out, "{}{:.6f}K", sign, a / 1e3);
    }
    return std::format_to(out, "{}{:.6f}", sign, a);
  }
};
