/**
 * @file      sddp_common.cpp
 * @brief     Out-of-line implementations for SDDP common helpers
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Hosts non-template, non-inline-friendly bodies that would otherwise
 * inflate every translation unit including `sddp_common.hpp`
 * (`PhaseGridRecorder` carries a `std::mutex` + `std::map` + JSON
 * formatting; keeping the bodies in the header would force every
 * caller to pull in `<format>`, `<map>`, `<mutex>` indirectly).
 */

#include <algorithm>
#include <format>

#include <gtopt/sddp_common.hpp>

namespace gtopt
{

void PhaseGridRecorder::record(IterationUid iteration_uid,
                               SceneUid scene_uid,
                               PhaseUid phase_uid,
                               GridCell state)
{
  const auto key = Key {
      .iteration_uid = iteration_uid,
      .scene_uid = scene_uid,
  };
  const char ch = static_cast<char>(state);
  const std::scoped_lock lock(m_mutex_);
  auto& cells = m_rows_[key];
  // No arithmetic on the UID — the inner map is keyed on PhaseUid
  // directly.  Higher-priority states win on subsequent writes
  // (`std::max` on the underlying char which is ordered by GridCell
  // priority).
  auto [it, inserted] = cells.try_emplace(phase_uid, ch);
  if (!inserted) {
    it->second = std::max(it->second, ch);
  }
}

std::string PhaseGridRecorder::to_json() const
{
  const std::scoped_lock lock(m_mutex_);
  std::string json;
  json += R"(  "phase_grid": {)"
          "\n";
  json += R"(    "rows": [)"
          "\n";
  bool first = true;
  for (const auto& [key, cells] : m_rows_) {
    if (!first) {
      json += ",\n";
    }
    first = false;
    // Emit `cells` as a UID-keyed JSON object — one entry per recorded
    // PhaseUid, no UID arithmetic anywhere.  The inner `std::map<
    // PhaseUid, char>` already iterates in sorted-UID order, so the
    // resulting JSON object preserves a stable column ordering for
    // any downstream consumer that wants to render the row positionally.
    // The TUI consumer (`scripts/run_gtopt/_tui.py::load_from_status`)
    // is updated in lock-step to read this object form.  Treating the
    // cells as a map (rather than a packed positional string) is what
    // a `PhaseUid` is meant for: an opaque identifier you look up,
    // never index-arithmetic into.
    json += std::format(R"(      {{"i": {}, "s": {}, "cells": {{)",
                        key.iteration_uid,
                        key.scene_uid);
    bool first_cell = true;
    for (const auto& [uid, ch] : cells) {
      if (!first_cell) {
        json += ", ";
      }
      first_cell = false;
      json += std::format(R"("{}": "{}")", value_of(uid), std::string(1, ch));
    }
    json += "}}";
  }
  json += "\n    ]\n";
  json += "  }\n";
  return json;
}

bool PhaseGridRecorder::empty() const
{
  const std::scoped_lock lock(m_mutex_);
  return m_rows_.empty();
}

}  // namespace gtopt
