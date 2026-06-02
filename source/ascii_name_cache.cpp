/**
 * @file      ascii_name_cache.cpp
 * @brief     Implementation of the lazy ASCII-name cache (issue #508).
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The cache walks `SimulationLP`'s `AmplElementNameMap` (populated by
 * `register_ampl_element` in `system_lp.cpp`) under `std::call_once`,
 * which itself is gated on the first `lookup` call from
 * `LabelMaker::format_label`.  Runs that never invoke `write_lp`
 * therefore never trigger population.
 */

#include <gtopt/ampl_variable.hpp>
#include <gtopt/as_label.hpp>
#include <gtopt/ascii_name_cache.hpp>
#include <gtopt/simulation_lp.hpp>

namespace gtopt
{

namespace
{

/// Read-only accessor for `SimulationLP::m_ampl_element_names_` via the
/// existing `lookup_ampl_element_uid` infrastructure.  The map is
/// keyed by `AmplElementNameKey = pair<string_view, string_view>` where
/// the first member is the snake_case class name (as registered in
/// `register_ampl_element`).  `populate_()` reads the registry through
/// a friend-style helper so the cache stays decoupled from
/// `SimulationLP`'s internal layout.
[[nodiscard]] const AmplElementNameMap& ampl_names(const SimulationLP& sim)
{
  return sim.ampl_element_names();
}

}  // namespace

std::string_view AsciiNameCache::lookup(std::string_view class_name,
                                        Uid uid) const
{
  // Lazy populate on first probe — exactly once across the cache's
  // lifetime.  Subsequent calls are pure reads.
  std::call_once(m_once_, [&] { populate_(); });

  const auto class_it = m_by_class_.find(class_name);
  if (class_it == m_by_class_.end()) {
    return {};
  }
  const auto uid_it = class_it->second.find(uid);
  if (uid_it == class_it->second.end()) {
    return {};
  }
  return uid_it->second;
}

void AsciiNameCache::populate_() const
{
  const auto& names = ampl_names(m_sim_);

  // Pass 1: total byte budget.  ASCIIfication never enlarges (each
  // input byte produces exactly one output byte), so the input length
  // is the exact required arena capacity.
  std::size_t total_bytes = 0;
  for (const auto& [key, _] : names) {
    total_bytes += key.second.size();
  }
  m_arena_.reserve(total_bytes);

  // Pass 2: asciify each name in place and record the view.  Reserving
  // up front guarantees no reallocation can invalidate previously
  // recorded views.
  for (const auto& [key, element_uid] : names) {
    const auto& class_snake = key.first;
    const auto& element_name = key.second;
    if (element_name.empty()) {
      continue;
    }
    const auto off = m_arena_.size();
    asciify_into(m_arena_, element_name);
    const std::string_view view {m_arena_.data() + off, m_arena_.size() - off};
    m_by_class_[class_snake].emplace(element_uid, view);
  }

  m_populated_.store(true, std::memory_order_release);
}

}  // namespace gtopt
