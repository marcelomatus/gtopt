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

#include <span>
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

  [[nodiscard]] static constexpr auto key(
      std::string_view class_name,
      Uid uid,
      std::string_view col_name,
      PhaseIndex phase_index,
      StageUid stage_uid,
      SceneIndex scene_index = SceneIndex {unknown_index},
      ScenarioUid scenario_uid = make_uid<Scenario>(unknown_uid)) noexcept
      -> Key
  {
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
  // These replace the former per-PhaseState full-vector caches
  // (`forward_col_sol`, `forward_col_cost`).
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

  /// Forward-pass primal solution of this state variable's source column.
  [[nodiscard]] constexpr auto col_sol() const noexcept { return m_col_sol_; }
  constexpr void set_col_sol(double v) const noexcept { m_col_sol_ = v; }

  /// Reduced cost of the dependent column in the target phase's last solve.
  [[nodiscard]] constexpr auto reduced_cost() const noexcept
  {
    return m_reduced_cost_;
  }
  constexpr void set_reduced_cost(double v) const noexcept
  {
    m_reduced_cost_ = v;
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
