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
#include <string>

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
// Parameters accept any formattable type (int, SceneUid, PhaseUid, etc.).

/// "SDDP Forward [i1 s1 p2]" — with phase tag and per-phase key.
/// `iteration_index` is formatted as the matching `IterationUid` (1-based)
/// so the iN segment uses the same UID convention as the sN/pN segments
/// — keeping all three log keys consistent.
template<typename S, typename P>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index,
                                          S scene,
                                          P phase)
{
  return as_label<void>("SDDP ",
                        tag,
                        " [i",
                        uid_of(iteration_index),
                        ' ',
                        's',
                        scene,
                        ' ',
                        'p',
                        phase,
                        ']');
}

/// "SDDP Aperture [i1 s1 p2 a5]" — with aperture uid appended.
template<typename S, typename P, typename A>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index,
                                          S scene,
                                          P phase,
                                          A aperture)
{
  return as_label<void>("SDDP ",
                        tag,
                        " [i",
                        uid_of(iteration_index),
                        ' ',
                        's',
                        scene,
                        ' ',
                        'p',
                        phase,
                        ' ',
                        'a',
                        aperture,
                        ']');
}

/// "SDDP Forward [i1 s1]" — scene-level (no phase).
template<typename S>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index,
                                          S scene)
{
  return as_label<void>(
      "SDDP ", tag, " [i", uid_of(iteration_index), ' ', 's', scene, ']');
}

/// "SDDP Init [i1]" — iteration-level only.
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index)
{
  return as_label<void>("SDDP ", tag, " [i", uid_of(iteration_index), ']');
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
  /// All three identifiers are 1-based UIDs (the same convention used in
  /// the `[iN sM pK]` log prefixes and the JSON output below).  Callers
  /// convert from internal `IterationIndex` / `SceneIndex` / `PhaseIndex`
  /// at the call site via `gtopt::uid_of(...)` / `sim.uid_of(...)`.
  void record(IterationUid iteration_uid,
              SceneUid scene_uid,
              PhaseUid phase_uid,
              GridCell state);

  /// Return a snapshot of the grid as a JSON fragment (no trailing comma).
  /// Format: "phase_grid": {"rows": [{"i":0,"s":0,"cells":"FF.FB..."}, ...]}
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

  mutable std::mutex m_mutex_;
  std::map<Key, std::string> m_rows_;
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
[[nodiscard]] inline auto format_si(double v) -> std::string
{
  const double a = std::abs(v);
  const char* sign = (v < 0.0) ? "-" : "";
  if (a >= 1e12) {
    return std::format("{}{:.3f}T", sign, a / 1e12);
  }
  if (a >= 1e9) {
    return std::format("{}{:.3f}G", sign, a / 1e9);
  }
  if (a >= 1e6) {
    return std::format("{}{:.2f}M", sign, a / 1e6);
  }
  if (a >= 1e3) {
    return std::format("{}{:.2f}K", sign, a / 1e3);
  }
  return std::format("{}{:.4f}", sign, a);
}

}  // namespace gtopt
