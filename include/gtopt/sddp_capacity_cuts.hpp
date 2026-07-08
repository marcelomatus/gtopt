/**
 * @file      sddp_capacity_cuts.hpp
 * @brief     Capacity-space projection of stored SDDP Benders cuts —
 *            the cut-extraction API for the OptGen-style investment
 *            master loop (deliverable 3 of the SDDiP campaign).
 * @date      2026-07-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * After an SDDP solve, the phase-0 optimality cuts bound the expected
 * cost-to-go as a function of EVERY phase-0 outgoing state variable —
 * reservoir energies AND the capacity accounting states (`capainst` /
 * `capacost`, registered by `CapacityObjectBase::add_to_lp` via
 * `add_state_col`).  An investment master MIP needs those same cuts
 * restricted to the capacity coordinates:
 *
 *   θ ≥ rhs − Σ_j coeff_j · K_j
 *
 * (the stored row convention is `α + Σ_j coeff_j·x_j ≥ rhs`, so the
 * master-facing support is `rhs − Σ coeff·K`).  `extract_capacity_cuts`
 * performs that projection, resolving every stored coefficient against
 * the (scene, phase) state-variable registry and keeping exactly the
 * `capainst` / `capacost` coordinates.
 *
 * **Projection caveat** (design doc §7.3,
 * `docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md`):
 * dropping a non-capacity coordinate (e.g. the reservoir `efin`) is
 * exact only when that coordinate is pinned at the same value across
 * master iterations (it is: phase-0 incoming reservoir state is the
 * `eini` data).  `dropped_state_coefficients` reports how many
 * coordinates were projected away so callers can tell an exact
 * pure-expansion projection (0 dropped) from the mixed-state case.
 */

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/sddp_cut_store.hpp>

namespace gtopt
{

class SimulationLP;

/// One capacity-space coefficient of an extracted cut, identified by
/// the state-variable registry key of its `capainst` / `capacost`
/// column.  The `string_view`s reference the registry's stable storage
/// (valid for the lifetime of the solver run).
struct CapacityCutCoefficient
{
  std::string_view class_name {};  ///< Element class (e.g. "Generator")
  std::string_view col_name {};  ///< "capainst" or "capacost"
  Uid uid {unknown_uid};  ///< Element UID
  ScenarioUid scenario_uid {unknown_uid_of<Scenario>()};
  StageUid stage_uid {unknown_uid_of<Stage>()};
  /// Stored cut-row coefficient (the row is `α + coeff·x ≥ rhs`, i.e.
  /// `coeff = −∂V/∂x`); the master-facing support slope is `−coeff`.
  double coeff {};
};

/// A stored optimality cut projected onto capacity coordinates.
struct CapacityCut
{
  SceneUid scene_uid {};  ///< Scene that generated the cut
  PhaseUid phase_uid {};  ///< Phase whose α the cut bounds
  IterationIndex iteration {};  ///< SDDP iteration that emitted it
  double rhs {};  ///< Cut row RHS (physical): θ(K) ≥ rhs − Σ coeff·K
  std::vector<CapacityCutCoefficient> coefficients {};
  /// Physical state coordinates (non-α, non-capacity — e.g. reservoir
  /// `efin`) that the projection dropped.  0 ⇒ the projection is exact
  /// in capacity space.
  std::size_t dropped_state_coefficients {};
};

/// Project the stored optimality cuts installed on @p phase_index onto
/// capacity state-variable coordinates (`capainst` / `capacost`).
///
/// Coefficients are resolved by column index against the originating
/// (scene, phase) state-variable registry — the same resolution the
/// oracle harness uses — so the caller must pass the live
/// `SimulationLP` of the run that produced @p cuts.  α coefficients
/// (class "Sddp", every `varphi_s` under multicut) are skipped;
/// feasibility cuts are ignored (they carry no cost-to-go bound).
///
/// @param sim          The live simulation whose registries resolve the
///                     stored column indices.
/// @param cuts         Stored cuts, e.g. `SDDPMethod::stored_cuts()`.
/// @param phase_index  Phase whose α the extracted cuts bound
///                     (default: phase 0 — the investment-master FCF).
/// @return One `CapacityCut` per matching stored optimality cut, in
///         input order (cuts whose coefficients contain a column that
///         no registry entry resolves are skipped with a warning —
///         they belong to a different LP universe).
[[nodiscard]] auto extract_capacity_cuts(const SimulationLP& sim,
                                         std::span<const StoredCut> cuts,
                                         PhaseIndex phase_index = PhaseIndex {
                                             0}) -> std::vector<CapacityCut>;

}  // namespace gtopt
