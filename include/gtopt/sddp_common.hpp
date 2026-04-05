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
#include <string>

#include <gtopt/basic_types.hpp>
#include <gtopt/iteration.hpp>

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
//   "SDDP <Phase> [i<iter> s<scene> p<phase>]: <message>"
//   "SDDP <Phase> [i<iter> s<scene> p<phase> a<aperture>]: <message>"
//
// where <Phase> is Forward, Backward, Aperture, etc.
// The bracketed key is easy to parse in run_gtopt:
//   re.compile(r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\]")
//
// Parameters accept any formattable type (int, SceneUid, PhaseUid, etc.).

/// "SDDP Forward [i0 s1 p2]" — with phase tag and per-phase key.
template<typename S, typename P>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iter,
                                          S scene,
                                          P phase)
{
  return std::format("SDDP {} [i{} s{} p{}]", tag, iter, scene, phase);
}

/// "SDDP Aperture [i0 s1 p2 a5]" — with aperture uid appended.
template<typename S, typename P, typename A>
[[nodiscard]] inline std::string sddp_log(
    std::string_view tag, IterationIndex iter, S scene, P phase, A aperture)
{
  return std::format(
      "SDDP {} [i{} s{} p{} a{}]", tag, iter, scene, phase, aperture);
}

/// "SDDP Forward [i0 s1]" — scene-level (no phase).
template<typename S>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iter,
                                          S scene)
{
  return std::format("SDDP {} [i{} s{}]", tag, iter, scene);
}

/// "SDDP Init [i0]" — iteration-level only.
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iter)
{
  return std::format("SDDP {} [i{}]", tag, iter);
}

}  // namespace gtopt
