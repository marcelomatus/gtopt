/**
 * @file      sddp_cut_io_internal.hpp
 * @brief     Internal helpers shared by sddp_cut_io split TUs.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Not part of the public gtopt API — these helpers are shared between
 * the four sibling translation units that implement the cut
 * persistence pipeline:
 *   * source/sddp_cut_io.cpp        — UID maps, format dispatchers
 *   * source/sddp_cut_csv.cpp       — CSV save / load
 *   * source/sddp_cut_json.cpp      — JSON save / load
 *   * source/sddp_boundary_cuts.cpp — boundary (future-cost) cuts
 *   * source/sddp_named_cuts.cpp    — named hot-start cuts
 *
 * `is_final_state_col` and `extract_iteration_from_name` live in the
 * public header `<gtopt/sddp_cut_io.hpp>` (they have unit tests).
 *
 * External callers should include `<gtopt/sddp_cut_io.hpp>` instead.
 * This header lives in `include/gtopt/` only because the build system
 * does not search `source/` for headers; it is namespaced under
 * `gtopt::detail` to mark its internal nature.
 */

#pragma once

#include <ostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <gtopt/as_label.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/uid.hpp>
#include <gtopt/utils.hpp>

namespace gtopt::detail
{

/// Compact reverse map from ColIndex to state variable identity.
struct ColKeyInfo
{
  std::string_view class_name;
  std::string_view col_name;
  Uid uid {unknown_uid};
};

using ColKeyMap = std::unordered_map<ColIndex, ColKeyInfo>;

/// Build a ColIndex → (class_name, col_name, uid) reverse map from the
/// state variables registered for a given (scene, phase).
[[nodiscard]] inline auto build_col_key_map(const SimulationLP& sim,
                                            SceneIndex si,
                                            PhaseIndex pi) -> ColKeyMap
{
  ColKeyMap map;
  map_reserve(map, sim.state_variables(si, pi).size());
  for (const auto& [key, svar] : sim.state_variables(si, pi)) {
    map.try_emplace(svar.col(),
                    ColKeyInfo {
                        .class_name = key.class_name,
                        .col_name = key.col_name,
                        .uid = key.uid,
                    });
  }
  return map;
}

/// Write a single cut's coefficients to an output stream.
///
/// Format: class:var:uid=coeff (structured key, portable without LP names).
/// Every column referenced in a cut must be a registered state variable —
/// including alpha, which is registered by
/// `SDDPMethod::initialize_alpha_variables`.
///
/// StoredCut already holds physical-space values: post-migration cuts are
/// built by `build_benders_cut_physical`, which emits physical coefficients
/// and a physical RHS; the SparseRow is captured in `SDDPCutManager::store_cut`
/// *before* `add_row` applies per-row equilibration, so `cut.coefficients`
/// is what we need to serialise verbatim.  No `* scale_obj / col_scale`
/// conversion is applied here — the file value IS the physical value.
inline void write_cut_coefficients(std::ostream& ofs,
                                   const StoredCut& cut,
                                   const LinearInterface& /*li*/,
                                   const ColKeyMap& col_keys)
{
  for (const auto& [col, coeff] : cut.coefficients) {
    const auto it = col_keys.find(col);
    if (it == col_keys.end()) {
      throw std::runtime_error(std::format(
          "SDDP write_cut_coefficients: cut '{}' references col {} that "
          "is not a registered state variable",
          cut.name,
          col));
    }
    const auto& [cls, var, uid] = it->second;
    ofs << "," << as_label<':'>(cls, var, uid) << "=" << coeff;
  }
}

/// Write cut coefficients without variable scaling (fallback when phase
/// UID is not found).  Values are physical.
inline void write_cut_coefficients_unscaled(std::ostream& ofs,
                                            const StoredCut& cut,
                                            double /*scale_obj*/)
{
  for (const auto& [col, coeff] : cut.coefficients) {
    ofs << "," << col << ":" << coeff;
  }
}

}  // namespace gtopt::detail
