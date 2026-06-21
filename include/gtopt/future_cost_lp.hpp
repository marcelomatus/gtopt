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

#include <string_view>

#include <gtopt/future_cost.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scene_lp.hpp>

namespace gtopt
{

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

  /// Saves α (`varphi_s`), the rebase constant c̄, and the un-rebased FCF
  /// approximation (α + c̄) to the solution.  Inert until step C.
  [[nodiscard]] bool add_to_output(OutputContext& out) const;
};

}  // namespace gtopt
