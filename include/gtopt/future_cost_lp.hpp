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

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtopt/future_cost.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scene_lp.hpp>

namespace gtopt
{
class SystemLP;  // fwd — for the no-op update_lp (keeps the element resident)

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
  /// `FutureCost/{alpha|alpha_<s>, rebase}` from the streams installed by
  /// `set_alpha_output`.  `alpha + rebase` reconstructs the un-rebased FCF.
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// No-op `update_lp` — its sole purpose is to mark `FutureCostLP` as
  /// `HasUpdateLP` so the collection stays RESIDENT under
  /// `low_memory = compress` (disposable collections are dropped between SDDP
  /// iterations).  The α-output handles installed by `set_alpha_output` must
  /// survive to `write_out`, so the element must not be rebuilt from scratch.
  [[nodiscard]] int update_lp(SystemLP& /*system_lp*/,
                              const ScenarioLP& /*scenario*/,
                              const StageLP& /*stage*/) noexcept
  {
    return 0;
  }

  /// One output stream per α column: `name` (`"alpha"` for the single layout,
  /// `"alpha_<s>"` per source scene under multicut) + a terminal-block col map.
  struct AlphaStream
  {
    std::string name;
    STBIndexHolder<ColIndex> cols;
  };

  /// Install the α / `varphi_s` output streams + the per-scene rebase constant
  /// c̄ (placed at the cell's terminal block).  Called by the solve method
  /// AFTER α registration + boundary-cut load — copies handles/values only,
  /// read-only w.r.t. the LP.  `add_to_output` then emits them.
  void set_alpha_output(std::vector<AlphaStream> streams,
                        STBIndexHolder<double> rebase)
  {
    m_alpha_streams_ = std::move(streams);
    m_rebase_ = std::move(rebase);
  }

  /// Number of α output streams installed: 0 before solve / no α columns,
  /// 1 for the single-α layout, N under multicut.  Exposed for tests.
  [[nodiscard]] std::size_t alpha_stream_count() const noexcept
  {
    return m_alpha_streams_.size();
  }

private:
  std::vector<AlphaStream> m_alpha_streams_;
  STBIndexHolder<double> m_rebase_;
};

}  // namespace gtopt
