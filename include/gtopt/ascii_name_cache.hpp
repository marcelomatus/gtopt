/**
 * @file      ascii_name_cache.hpp
 * @brief     Lazy per-SimulationLP cache mapping (class, uid) -> asciified name
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the lazy ASCII-name cache described in
 * `docs/design/lp-extended-labels.md` §6.  The cache is empty at
 * construction; the first call to `lookup(class_name, uid)` triggers a
 * single `std::call_once`-guarded two-pass populate:
 *
 *   - Pass 1 sums the asciified-name byte budget (ASCIIfication never
 *     enlarges so `in.size() == out.size()`).
 *   - Pass 2 reserves the arena once and writes the asciified bytes via
 *     `asciify_into`; recorded `string_view`s remain valid for the
 *     lifetime of the owning `SimulationLP`.
 *
 * When `LabelMaker` operates under `LpLabelStyle::compact` (or under
 * `LpNamesLevel::none`) the cache is never consulted — the cache
 * pointer on `LabelMaker` stays `nullptr` and `lookup` is never called.
 * This keeps the hard requirement of "zero cost when `write_lp` is not
 * invoked" structural rather than a runtime toggle.
 *
 * Thread safety: `populate_()` is guarded by `std::call_once`.
 * Subsequent `lookup` calls are pure reads of an immutable structure —
 * safe under concurrent SDDP / cascade label emission across cells.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>

namespace gtopt
{

class SimulationLP;

/**
 * @class AsciiNameCache
 * @brief Lazy `(class_snake, uid) -> string_view` map populated on first probe.
 *
 * Owned by `SimulationLP` (one instance per simulation).  At
 * construction the cache holds only its empty containers and a
 * `std::once_flag` — ~64 bytes resident, zero heap allocation.  The
 * first call to `lookup` triggers `populate_()`, which walks
 * `SimulationLP`'s element-name registry (populated by
 * `register_ampl_element` in `system_lp.cpp`) and writes each
 * asciified name into a single contiguous arena.
 *
 * Lookup is a two-level `flat_map` probe (class first, then uid),
 * keyed by the snake_case class name (matching the existing
 * `AmplElementNameKey` convention).  A miss returns an empty
 * `string_view`; `LabelMaker::format_label` falls back to the
 * compact form for that single label.
 */
class AsciiNameCache
{
public:
  /// Bind the cache to its owning simulation.  Stores a reference only;
  /// no population occurs — every container default-constructs empty
  /// and the `once_flag` is unset.  Subsequent moves / copies are
  /// disabled because `m_sim_` references the owning `SimulationLP`.
  explicit AsciiNameCache(const SimulationLP& sim) noexcept
      : m_sim_(sim)
  {
  }

  AsciiNameCache(const AsciiNameCache&) = delete;
  AsciiNameCache(AsciiNameCache&&) = delete;
  AsciiNameCache& operator=(const AsciiNameCache&) = delete;
  AsciiNameCache& operator=(AsciiNameCache&&) = delete;
  ~AsciiNameCache() = default;

  /**
   * @brief Look up the asciified element name for `(class_name, uid)`.
   *
   * The first call across the cache's lifetime triggers
   * `populate_()` under `std::call_once`; every subsequent call is
   * a pure read.  Returns an empty `string_view` when the pair is
   * not registered or when the element's raw name is empty —
   * `LabelMaker` falls back to the compact form in that case.
   *
   * @param class_name  Snake-case class name (e.g. `"line"`,
   *                    `"generator"`).  Caller passes the same form
   *                    `LabelMaker` already produces via
   *                    `lowercase(...)`, but `populate_()` is
   *                    tolerant of PascalCase — it stores keys as
   *                    snake_case `string_view`s into the
   *                    SimulationLP's AMPL registry, so the lookup
   *                    side accepts whatever form the caller emits
   *                    as long as it matches that registry's
   *                    convention (snake_case in practice).
   * @param uid         Element UID, must match the value recorded
   *                    via `register_ampl_element`.
   * @return Asciified name as a `string_view` into the cache arena,
   *         or empty when no match.
   */
  [[nodiscard]] std::string_view lookup(std::string_view class_name,
                                        Uid uid) const;

  /**
   * @brief True once `populate_()` has run at least once.
   *
   * Diagnostic-only; the hot path uses `std::call_once` directly.
   */
  [[nodiscard]] bool is_populated() const noexcept
  {
    return m_populated_.load(std::memory_order_acquire);
  }

private:
  /// Single-shot population, guarded by `std::call_once` inside `lookup`.
  /// Walks `m_sim_.m_ampl_element_names_` (via the public
  /// `lookup_ampl_element_uid`-adjacent registry) in two passes:
  /// pass 1 totals the byte budget so pass 2 can write into a reserved
  /// arena without ever reallocating.
  void populate_() const;

  const SimulationLP& m_sim_;
  mutable std::once_flag m_once_;
  mutable std::atomic<bool> m_populated_ {false};
  mutable std::string m_arena_;
  // Two-level map: snake_case class name → (uid → arena view).  Keyed
  // by `string_view` into the AmplElementNameKey storage (owned by
  // `SimulationLP`).
  mutable flat_map<std::string_view, flat_map<Uid, std::string_view>>
      m_by_class_;
};

}  // namespace gtopt
