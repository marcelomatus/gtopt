/**
 * @file      future_cost_lp.hpp
 * @brief     LP representation of a FutureCost (FCF / cost-to-go) element
 * @date      Sun Jun 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Planning-level element: it owns the end-of-horizon cost-to-go α
 * (`varphi_s`) column(s) and the boundary cuts that linearise it.  Unlike the
 * operational elements it has NO per-(scenario, stage) `add_to_lp`; it
 * participates only in the GLOBAL planning pass (`add_to_global_lp`, run once
 * per (scene, phase) cell after the operational build) and the output pass.
 *
 * See docs/design/future_cost_and_user_model.md (piece 2).
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <gtopt/future_cost.hpp>
#include <gtopt/object_lp.hpp>

namespace gtopt
{
class SystemContext;
class SceneLP;
class PhaseLP;
class LinearProblem;
class OutputContext;
class PlanningLP;

using FutureCostLPSId = ObjectSingleId<class FutureCostLP>;

class FutureCostLP : public ObjectLP<FutureCost>
{
public:
  /// Output stream names (no magic strings — see feedback_no_magic_strings).
  static constexpr std::string_view AlphaName {"alpha"};
  static constexpr std::string_view RebaseName {"rebase"};
  static constexpr std::string_view ApproxFcfName {"approx_fcf"};

  explicit FutureCostLP(const FutureCost& pfc, const InputContext& /*ic*/)
      : ObjectLP<FutureCost>(pfc)
  {
  }

  [[nodiscard]] constexpr auto&& future_cost(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Planning-level GLOBAL scope: registers the α / `varphi_s` columns, loads
  /// the boundary cuts, applies the `mean_shift` rebase (anchored at the
  /// reservoirs' `efin` targets), and stores the α col handles + per-scene c̄.
  /// Runs once per (scene, phase) cell after the operational `add_to_lp` loop
  /// (see `system_lp.cpp::add_to_planning_lp`).
  ///
  /// Piece-2 step A: inert plumbing.  The α / boundary-cut / rebase logic
  /// migrates here from `sddp_method_alpha.cpp` + `sddp_boundary_cuts.cpp` in
  /// step D, guarded by the mean_shift bound-consistency regression test.
  [[nodiscard]] bool add_to_global_lp(const SystemContext& sc,
                                      const SceneLP& scene,
                                      const PhaseLP& phase,
                                      LinearProblem& lp);

  /// Emits the α / `varphi_s` solution(s) + the per-scene rebase constant c̄ to
  /// `FutureCost/{alpha|alpha_<s>, rebase}`.  SELF-FINDS its data at write
  /// time: the α columns are read from the persistent `SimulationLP`
  /// state-variable registry (via `alpha_cols_on_cell`) and the rebase
  /// constant from `SimulationLP::alpha_offset`, both reached through
  /// `OutputContext::system_context()`.  Because these registries persist
  /// across per-cell LP rebuilds, the output works under ALL `low_memory`
  /// modes — no per-cell resident stash is needed.  `alpha + rebase`
  /// reconstructs the un-rebased FCF.
  [[nodiscard]] bool add_to_output(OutputContext& out) const;
};

/// Consolidated boundary-cut / FCF configuration sourced from a `FutureCost`
/// element (piece 5 step 2b).  Each field is an `std::optional`: SET only when
/// the element explicitly authors it, so the SDDP/monolithic method can
/// OVERRIDE the corresponding `SDDPOptions` field iff present (element wins)
/// and otherwise leave `m_options_` untouched (backward-compatible — zero
/// numerical change when there is no FutureCost element or it leaves a field
/// unset).
struct SDDPBoundaryConfig
{
  std::optional<std::string> cuts_file {};
  std::optional<double> scale_alpha {};
  std::optional<bool> mean_shift {};
  std::optional<BoundaryCutSharingMode> sharing {};
  std::optional<BoundaryCutsMode> mode {};
};

/// Build the consolidated boundary config from a `FutureCost` element — the
/// single read-site mapping from the element's authored fields onto the
/// resolved `SDDPOptions` boundary fields.  Unset element fields stay `nullopt`
/// so the caller leaves the option untouched.
[[nodiscard]] SDDPBoundaryConfig boundary_config(const FutureCost& fc);

/// Pointer to the first ACTIVE `FutureCost` element in @p planning_lp, or
/// `nullptr` when there is none.  Reads the element straight from the System
/// INPUT data (`future_cost_array`) of the representative `(scene 0, phase 0)`
/// cell — NOT the LP collection, which the SDDP default `low_memory = compress`
/// drops for planning-only elements.  Used by the SDDP method to source the
/// user-α uid + the consolidated boundary config (steps 2a/2b).
[[nodiscard]] const FutureCost* active_future_cost(
    const PlanningLP& planning_lp);

/// Whether the active `FutureCost` element in @p planning_lp has
/// `use_user_alpha = true` (piece 5 step 2a) — the user-overridable FCF flag
/// that suppresses the built-in α (`register_alpha_variables` runs with
/// `register_as_state_variable = false`).  Returns `false` when there is no
/// active FutureCost element (the legacy boundary-cut / no-FCF paths).
[[nodiscard]] bool has_active_use_user_alpha(const PlanningLP& planning_lp);

/// The active user-α column's uid when a FutureCost element has
/// use_user_alpha=true AND a user_alpha_uid set; std::nullopt otherwise.
/// Single source of truth for the "user α is the FCF recourse column"
/// guard used across the SDDP backward/forward/alpha passes.
[[nodiscard]] std::optional<Uid> active_user_alpha_uid(
    const PlanningLP& planning_lp) noexcept;

}  // namespace gtopt
