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

/// "SDDP Forward [i0 s1 p2]" — with phase tag and per-phase key.
template<typename S, typename P>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index,
                                          S scene,
                                          P phase)
{
  return as_label<void>("SDDP ",
                        tag,
                        " [i",
                        iteration_index,
                        ' ',
                        's',
                        scene,
                        ' ',
                        'p',
                        phase,
                        ']');
}

/// "SDDP Aperture [i0 s1 p2 a5]" — with aperture uid appended.
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
                        iteration_index,
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

/// "SDDP Forward [i0 s1]" — scene-level (no phase).
template<typename S>
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index,
                                          S scene)
{
  return as_label<void>(
      "SDDP ", tag, " [i", iteration_index, ' ', 's', scene, ']');
}

/// "SDDP Init [i0]" — iteration-level only.
[[nodiscard]] inline std::string sddp_log(std::string_view tag,
                                          IterationIndex iteration_index)
{
  return as_label<void>("SDDP ", tag, " [i", iteration_index, ']');
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
  void record(IterationIndex iteration_index,
              SceneUid scene_uid,
              PhaseIndex phase_index,
              GridCell state)
  {
    const auto key = Key {
        .iteration_index = iteration_index,
        .scene_uid = scene_uid,
    };
    const auto phase_col = static_cast<std::size_t>(phase_index);
    const std::scoped_lock lock(m_mutex_);
    auto& row = m_rows_[key];
    if (phase_col >= row.size()) {
      row.resize(phase_col + 1, '.');
    }
    const char ch = static_cast<char>(state);
    auto& cell = row[phase_col];
    cell = std::max(cell, ch);
  }

  /// Return a snapshot of the grid as a JSON fragment (no trailing comma).
  /// Format: "phase_grid": {"rows": [{"i":0,"s":0,"cells":"FF.FB..."}, ...]}
  [[nodiscard]] std::string to_json() const
  {
    const std::scoped_lock lock(m_mutex_);
    std::string json;
    json += R"(  "phase_grid": {)"
            "\n";
    json += R"(    "rows": [)"
            "\n";
    bool first = true;
    for (const auto& [key, row] : m_rows_) {
      if (!first) {
        json += ",\n";
      }
      first = false;
      json += std::format(R"(      {{"i": {}, "s": {}, "cells": "{}"}})",
                          key.iteration_index,
                          key.scene_uid,
                          row);
    }
    json += "\n    ]\n";
    json += "  }\n";
    return json;
  }

  /// True when at least one cell has been recorded.
  [[nodiscard]] bool empty() const
  {
    const std::scoped_lock lock(m_mutex_);
    return m_rows_.empty();
  }

private:
  struct Key
  {
    IterationIndex iteration_index {};
    SceneUid scene_uid {};
    auto operator<=>(const Key&) const = default;
  };

  mutable std::mutex m_mutex_;
  std::map<Key, std::string> m_rows_;
};

}  // namespace gtopt
