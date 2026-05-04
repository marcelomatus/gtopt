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
  // 1-based UID → 0-based slot in the per-row cells vector.
  const auto phase_slot = static_cast<std::ptrdiff_t>(value_of(phase_uid)) - 1;
  const std::scoped_lock lock(m_mutex_);
  auto& row = m_rows_[key];
  if (phase_slot >= std::ssize(row)) {
    row.resize(static_cast<size_t>(phase_slot + 1), '.');
  }
  const char ch = static_cast<char>(state);
  auto& cell = row[static_cast<size_t>(phase_slot)];
  cell = std::max(cell, ch);
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
  for (const auto& [key, row] : m_rows_) {
    if (!first) {
      json += ",\n";
    }
    first = false;
    json += std::format(R"(      {{"i": {}, "s": {}, "cells": "{}"}})",
                        key.iteration_uid,
                        key.scene_uid,
                        row);
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
