/**
 * @file      arrow_index_cache.hpp
 * @brief     Shared, scene/phase-invariant Arrow input-index cache
 * @date      Tue Jun 30 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Holds the (cname, fname, uid) -> (Stage, Block) Arrow row indices built by
 * the long-direct input reader.  These indices depend only on the input
 * tables, never on the (scene, phase) cell, so they are built once and shared
 * across every per-cell SystemLP build instead of being rebuilt (and the
 * parquet re-read) per InputContext — that per-cell rebuild was the
 * long-direct-input LP-build blow-up (see get_arrow_index).
 *
 * The cache is owned by SimulationLP behind a forward declaration (pimpl) so
 * simulation_lp.hpp need not include uid_traits.hpp, which would form the
 * include cycle simulation_lp.hpp -> uid_traits.hpp -> uididx_traits.hpp ->
 * simulation_lp.hpp.  Concurrent access from the parallel per-cell builds is
 * serialised by SimulationLP::array_index_mutex().
 */

#pragma once

#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <gtopt/uid_traits.hpp>

namespace gtopt
{

/// One sub-map per index arity used by the input readers.  Mirrors the tuple
/// that previously lived in InputContext, but shared across all cells.
struct ArrowIndexCache
{
  std::tuple<
      UidTraits::array_table_vector_uid_idx_t<ScenarioUid, StageUid, BlockUid>,
      UidTraits::array_table_vector_uid_idx_t<ScenarioUid, StageUid>,
      UidTraits::array_table_vector_uid_idx_t<StageUid, BlockUid>,
      UidTraits::array_table_vector_uid_idx_t<StageUid>>
      maps;

  /// Run-lived backing storage for the map keys.  The map key types
  /// (UidTraits::CFName / CFNameUid) hold std::string_view class/field names.
  /// When the cache was per-cell those views stayed valid for the cell's
  /// build; now that the cache is shared for the whole run, get_arrow_index
  /// MUST intern the incoming (cname, fname) here first so the persisted keys
  /// never dangle.  std::unordered_set is node-based, so the address of a
  /// stored string — and hence a string_view into it — is stable across
  /// later inserts.  Interning happens under SimulationLP::array_index_mutex()
  /// (held by make_array_index), so no extra synchronization is needed.
  std::unordered_set<std::string> interned_names;

  /// Return a stable, run-lived view of @p s, copying it into interned_names
  /// on first sight.  Use the result for every cache-key construction.
  [[nodiscard]] std::string_view intern(std::string_view s)
  {
    return *interned_names.emplace(s).first;
  }
};

}  // namespace gtopt
