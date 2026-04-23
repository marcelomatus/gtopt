/**
 * @file      state_variable.hpp
 * @brief     State variables and dependencies for linear programming problems
 * @date      Mon Jun 23 11:56:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * This module defines the StateVariable class which represents variables in a
 * linear programming problem that may have dependencies across different scenes
 * and phases of the optimization.
 *
 * Key Features:
 * - Tracks optimization values across problem phases
 * - Manages dependent variables that update automatically
 * - Thread-safe for concurrent access
 * - Supports both single-phase and multi-scenario problems
 *
 * @note All public methods are thread-safe unless otherwise noted
 */

#pragma once

#include <cassert>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{
struct LPKey
{
  SceneIndex scene_index {unknown_index};
  PhaseIndex phase_index {unknown_index};

  [[nodiscard]] constexpr auto operator<=>(const LPKey&) const noexcept =
      default;
};

struct LPVariable
{
  constexpr explicit LPVariable(LPKey lp_key, ColIndex col) noexcept
      : m_lp_key_ {lp_key}
      , m_col_ {col}
  {
  }

  [[nodiscard]]
  constexpr auto col() const noexcept -> ColIndex
  {
    return m_col_;
  }

  [[nodiscard]]
  constexpr auto lp_key() const noexcept -> const LPKey&
  {
    return m_lp_key_;
  }

  [[nodiscard]]
  constexpr auto scene_index() const noexcept -> SceneIndex
  {
    return m_lp_key_.scene_index;
  }

  [[nodiscard]]
  constexpr auto phase_index() const noexcept -> PhaseIndex
  {
    return m_lp_key_.phase_index;
  }

private:
  LPKey m_lp_key_;
  ColIndex m_col_ {unknown_index};
};

class StateVariable : public LPVariable
{
public:
  using LPKey = gtopt::LPKey;

  struct Key
  {
    ScenarioUid scenario_uid = unknown_uid_of<Scenario>();
    StageUid stage_uid = unknown_uid_of<Stage>();
    Uid uid {unknown_uid};
    std::string_view col_name;
    std::string_view class_name;
    LPKey lp_key;

    constexpr auto operator<=>(const Key&) const noexcept = default;
  };

  [[nodiscard]] static auto key(std::string_view class_name,
                                Uid uid,
                                std::string_view col_name,
                                PhaseIndex phase_index,
                                StageUid stage_uid,
                                SceneIndex scene_index,
                                ScenarioUid scenario_uid) -> Key
  {
    // Every state variable must carry a valid element uid so that
    // cross-phase cuts (fcut/scut/bcut/ecut/aper_cut) serialise with
    // parseable names.  `unknown_uid = -1` slips into LP labels as
    // `-1_…`, whose embedded `-` char is rejected by CoinLpIO's name
    // validator (see master #426 / a8a0e452, PR #429) and causes CBC
    // to strip every col/row label from the written LP file.  Catch
    // the root cause here — throw in both debug and release builds
    // so an unknown uid is never silently embedded into LP labels.
    // scene/scenario/stage uids must also be concrete; silent defaults
    // to `unknown_*` produce the same `-1_…` pattern in labels.
    if (uid == unknown_uid) {
      throw std::invalid_argument(
          "StateVariable::key: uid must not be unknown_uid");
    }
    if (scenario_uid == unknown_uid_of<Scenario>()) {
      throw std::invalid_argument(
          "StateVariable::key: scenario_uid must not be unknown");
    }
    if (stage_uid == unknown_uid_of<Stage>()) {
      throw std::invalid_argument(
          "StateVariable::key: stage_uid must not be unknown");
    }
    return {
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
        .uid = uid,
        .col_name = col_name,
        .class_name = class_name,
        .lp_key = {.scene_index = scene_index, .phase_index = phase_index},
    };
  }

  template<typename ScenarioLP, typename StageLP>
  [[nodiscard]]
  static constexpr auto key(const ScenarioLP& scenario,
                            const StageLP& stage,
                            std::string_view class_name,
                            Uid element_uid,
                            std::string_view col_name) noexcept -> Key
  {
    return key(class_name,
               element_uid,
               col_name,
               stage.phase_index(),
               stage.uid(),
               scenario.scene_index(),
               scenario.uid());
  }

  template<typename StageLP>
  [[nodiscard]]
  static constexpr auto key(const StageLP& stage,
                            std::string_view class_name,
                            Uid element_uid,
                            std::string_view col_name) noexcept -> Key
  {
    return key(
        class_name, element_uid, col_name, stage.phase_index(), stage.uid());
  }

  constexpr explicit StateVariable(LPKey lp_key,
                                   ColIndex col,
                                   double scost,
                                   double var_scale,
                                   LpContext context)
      : LPVariable(lp_key, col)
      , m_scost_(scost)
      , m_var_scale_(var_scale)
      , m_context_(std::move(context))
  {
  }

  /// State cost for elastic penalty [$/physical_unit].
  /// When > 0, overrides the global elastic_penalty for this variable.
  [[nodiscard]] constexpr auto scost() const noexcept { return m_scost_; }

  /// Physical-to-LP scale: physical = LP × var_scale.
  [[nodiscard]] constexpr auto var_scale() const noexcept
  {
    return m_var_scale_;
  }

  /// LP hierarchy context (scenario, stage, block, ...).
  [[nodiscard]] constexpr const auto& context() const noexcept
  {
    return m_context_;
  }

  using DependentVariable = LPVariable;

  [[nodiscard]] constexpr auto dependent_variables() const noexcept
      -> std::span<const DependentVariable>
  {
    return m_dependent_variables_;
  }

  constexpr auto add_dependent_variable(LPKey lp_key, ColIndex col)
      -> const DependentVariable&
  {
    return m_dependent_variables_.emplace_back(lp_key, col);
  }

  template<typename ScenarioLP, typename StageLP>
  constexpr auto add_dependent_variable(const ScenarioLP& scenario,
                                        const StageLP& stage,
                                        ColIndex col)
      -> const DependentVariable&
  {
    return add_dependent_variable(
        LPKey {
            .scene_index = scenario.scene_index(),
            .phase_index = stage.phase_index(),
        },
        col);
  }

  // ── Runtime SDDP values (mutable — written during solve passes) ────────
  //
  // These carry the per-state-variable post-solve values consumed by
  // next-phase trial propagation and backward-pass cut construction.
  //
  //   - col_sol       : always set after a forward solve (free — just a
  //                     primal read).  Used to propagate trial values to
  //                     the next phase's dependent column.
  //   - reduced_cost  : always set after a forward solve (free — just a
  //                     reduced-cost read on the dependent column).
  //
  // Cut construction uses reduced costs only.  See docs/methods/sddp.md
  // for why the row-dual (PLP-style) formulation was removed.
  //
  // `mutable` allows mutation via `const StateVariable*` pointers stored
  // on `StateVarLink`, keeping the const-correctness of the registry map
  // iteration at link-build time.

  /// Forward-pass primal solution of this state variable's source column,
  /// in **raw LP** space.  Multiply by `var_scale()` to obtain the
  /// physical value, or use `col_sol_physical()` below.
  [[nodiscard]] constexpr auto col_sol() const noexcept { return m_col_sol_; }
  constexpr void set_col_sol(double v) const noexcept { m_col_sol_ = v; }

  /// Physical-space primal solution:  `LP × var_scale`.
  ///
  /// For the alpha future-cost column this equals `LP × scale_alpha`,
  /// matching the `alpha_svar->col_sol() * sa` idiom at the forward-
  /// pass call sites.  For state variables with `var_scale = 1.0`
  /// (bare primal columns without a flatten-time semantic scale) this
  /// returns the same value as `col_sol()`.
  [[nodiscard]] constexpr double col_sol_physical() const noexcept
  {
    return m_col_sol_ * m_var_scale_;
  }

  /// Reduced cost of the dependent column in the target phase's last
  /// solve, in **raw LP** space.  Use `reduced_cost_physical()` below
  /// when building physical-space Benders cuts.
  [[nodiscard]] constexpr auto reduced_cost() const noexcept
  {
    return m_reduced_cost_;
  }
  constexpr void set_reduced_cost(double v) const noexcept
  {
    m_reduced_cost_ = v;
  }

  /// Physical-space reduced cost (`$/physical_unit`):
  ///   `rc_LP × scale_objective / var_scale`.
  ///
  /// This matches the `LinearInterface::get_col_cost()` convention
  /// (`LP × scale_objective / col_scale`), so the physical-space
  /// Benders cut builder (`build_benders_cut_physical`) can take
  /// either source with identical semantics.  `scale_objective` is
  /// passed as an argument because it's a global option, not a
  /// per-state-variable property.
  [[nodiscard]] constexpr double reduced_cost_physical(
      double scale_objective) const noexcept
  {
    return m_reduced_cost_ * scale_objective / m_var_scale_;
  }

private:
  double m_scost_ {0.0};
  double m_var_scale_ {1.0};
  LpContext m_context_ {};
  std::vector<DependentVariable> m_dependent_variables_;

  mutable double m_col_sol_ {0.0};
  mutable double m_reduced_cost_ {0.0};
};

/// Deferred state-variable link record.
///
/// Recorded by phase N+1's `add_to_lp` so that the eventual
/// `add_dependent_variable` call on phase N's matching `StateVariable`
/// can be performed in a sequential tightening pass *after* all phases
/// of a scene have been built — possibly in parallel.
///
/// The structured `prev_key` uniquely identifies the producing
/// `StateVariable` in the global `(scene, phase)`-partitioned registry,
/// so the tightening pass needs no reference to phase N's `SystemLP` to
/// resolve the link.  `(here_key, here_col)` names the dependent column
/// to register on the resolved `StateVariable`.
///
/// `prev_key.col_name` and `prev_key.class_name` are `std::string_view`s
/// — they must point to storage that outlives this record.  In practice
/// they always point at the `static constexpr` literals declared by the
/// element classes (e.g. `StorageLP::EfinName`, the class name constant)
/// so the lifetime requirement is satisfied trivially.
struct PendingStateLink
{
  StateVariable::Key prev_key;
  LPKey here_key;
  ColIndex here_col {unknown_index};
};

}  // namespace gtopt
