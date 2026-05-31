/**
 * @file      fuel_lp.hpp
 * @brief     LP wrapper for the Fuel data struct
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `FuelLP` resolves the time-schedulable fuel price + emission factors
 * that `GeneratorLP` consumes via `system_context.element<FuelLP>`.
 *
 * When `Fuel.max_offtake` is set the FuelLP also creates a per-(scenario,
 * stage) cap row enforcing
 *
 *   Σ_{g : Generator(g).fuel = this_fuel}
 *       (heat_rate_g(s, b) · generation_g(s, t, b) · duration_b)
 *     ≤  max_offtake(s)
 *
 * with an optional slack column priced at `max_offtake_cost` when the
 * cap is soft.  Mirrors PLEXOS's `FueMaxOffWeek_<fuel>` /
 * `FueMaxOffDay_<fuel>` Constraint pattern.
 *
 * Symmetrically, when `Fuel.min_offtake` is set FuelLP creates a
 * per-(scenario, stage) FLOOR row
 *
 *   Σ_g (heat_rate · gen · dur)  ≥  min_offtake(s)
 *
 * with an optional shortfall slack priced at `min_offtake_cost` when
 * the floor is soft.  Mirrors PLEXOS's `Fuel.Min Offtake {Hour, Day,
 * Week, Month, Year}` family (pids 595-600) used for take-or-pay
 * obligations.  When BOTH bounds are set the two rows share the
 * offtake DV LHS — same pattern as `Commitment::{min,max}_starts`.
 */

#pragma once

#include <gtopt/fuel.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

// Forward declarations
class InputContext;
class LinearProblem;
class OutputContext;
class SystemContext;

class FuelLP : public ObjectLP<Fuel>
{
public:
  using Base = ObjectLP<Fuel>;

  /// LP row / col name constants (snake_case for output filenames).
  static constexpr std::string_view MaxOfftakeName {"max_offtake"};
  static constexpr std::string_view MaxOfftakeSlackName {"max_offtake_slack"};
  /// Symmetric min-side floor names — see ``Fuel::min_offtake``.  When
  /// both ``min_offtake`` and ``max_offtake`` are populated FuelLP
  /// emits two separate rows sharing the offtake DV LHS (same
  /// pattern as ``Commitment::{min,max}_starts``).  Each side gets
  /// its own dual stream + slack column when softened by a positive
  /// ``*_cost``.
  static constexpr std::string_view MinOfftakeName {"min_offtake"};
  static constexpr std::string_view MinOfftakeSlackName {"min_offtake_slack"};
  /// Per-(scenario, stage, block) fuel offtake decision variable
  /// ``Y_f[b] = Σ_g heat_rate_g · dur_b · generation_g[b]`` exposed to
  /// PAMPL as ``fuel("X").offtake``.  Lets PLEXOS ``Offtake
  /// Coefficient`` UCs (``Gas_MaxOpDay*``, ``FueMaxOff*``) translate
  /// verbatim instead of via per-generator expansion.  See
  /// ``add_to_lp`` for the binding equation.  Note: the DV is created
  /// only when at least one generator consumes the fuel at the
  /// (scenario, stage); fuels with no active consumers do not register
  /// the AMPL attribute, so UC references to ``fuel(X).offtake`` for
  /// such fuels currently trip the strict resolver — see the
  /// ``GTOPT_USE_FUEL_OFFTAKE`` gate in plexos2gtopt/parsers.py and
  /// the matching TODO at the top of FuelLP::add_to_lp.
  static constexpr std::string_view OfftakeName {"offtake"};
  /// LP row name for the offtake binding equation
  /// ``Y_f[b] − Σ heat_rate_g · dur_b · gen_g[b] = 0``.  Hidden from
  /// the dual / row-name index (it's just a definition row, not a
  /// physical constraint), but emitted with this label for LP-debug
  /// clarity.
  static constexpr std::string_view OfftakeDefName {"offtake_def"};

  explicit FuelLP(const Fuel& fuel, const InputContext& ic);

  [[nodiscard]] constexpr auto&& fuel(this auto&& self) noexcept
  {
    return self.object();
  }

  /// LP-active hooks.  When `max_offtake` is unset on every stage these
  /// are effectively no-ops — FuelLP retains its passive-parameter
  /// behaviour for downstream consumers (`GeneratorLP`, `EmissionLP`).
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  [[nodiscard]] auto param_heat_content(StageUid s) const
  {
    return heat_content_.at(s);
  }
  [[nodiscard]] auto param_combustion_emission_factor(StageUid s) const
  {
    return combustion_ef_.at(s);
  }
  [[nodiscard]] auto param_upstream_emission_factor(StageUid s) const
  {
    return upstream_ef_.at(s);
  }
  [[nodiscard]] auto param_max_offtake(StageUid s) const
  {
    return max_offtake_.at(s);
  }
  [[nodiscard]] auto param_max_offtake_cost(StageUid s) const
  {
    return max_offtake_cost_.at(s);
  }
  [[nodiscard]] auto param_min_offtake(StageUid s) const
  {
    return min_offtake_.at(s);
  }
  [[nodiscard]] auto param_min_offtake_cost(StageUid s) const
  {
    return min_offtake_cost_.at(s);
  }
  /// @}

private:
  OptTRealSched price_;
  OptTRealSched heat_content_;
  OptTRealSched combustion_ef_;
  OptTRealSched upstream_ef_;
  OptTRealSched max_offtake_;
  OptTRealSched max_offtake_cost_;
  OptTRealSched min_offtake_;
  OptTRealSched min_offtake_cost_;

  /// Per-(scenario, stage) cap row + optional slack column.  Empty
  /// when `max_offtake` is unset for every (scenario, stage), or
  /// when the per-block path is selected via
  /// `Fuel.max_offtake_per_block = true`.
  STIndexHolder<RowIndex> max_offtake_rows_;
  STIndexHolder<ColIndex> max_offtake_slack_cols_;

  /// Per-(scenario, stage, block) cap rows + optional slack columns.
  /// Populated only when `Fuel.max_offtake_per_block = true` —
  /// mirrors PLEXOS's per-period `FueMaxOffWeek_<fuel>` semantics
  /// by pro-rating the stage cap by each block's share of total
  /// stage duration.
  STBIndexHolder<RowIndex> max_offtake_block_rows_;
  STBIndexHolder<ColIndex> max_offtake_block_slack_cols_;

  /// Per-(scenario, stage) FLOOR row + optional shortfall-slack column
  /// (``Σ_b Y_f[b] + slack ≥ min_offtake``).  Empty when
  /// `min_offtake` is unset for every (scenario, stage), or when the
  /// per-block path is selected via `Fuel.min_offtake_per_block`.
  /// Symmetric to ``max_offtake_rows_`` / ``max_offtake_slack_cols_``.
  STIndexHolder<RowIndex> min_offtake_rows_;
  STIndexHolder<ColIndex> min_offtake_slack_cols_;

  /// Per-(scenario, stage, block) FLOOR rows + optional shortfall
  /// slack columns.  Populated only when
  /// `Fuel.min_offtake_per_block = true` — pro-rates the stage floor
  /// by each block's share of total stage duration.
  STBIndexHolder<RowIndex> min_offtake_block_rows_;
  STBIndexHolder<ColIndex> min_offtake_block_slack_cols_;

  /// Per-(scenario, stage, block) offtake decision variable
  /// ``Y_f[b]``.  Populated only for cells where at least one
  /// generator consuming this fuel is active at the block — exposed
  /// to PAMPL as ``fuel("X").offtake`` so UCs can reference it
  /// directly.
  STBIndexHolder<ColIndex> offtake_cols_;
};

/// SingleId-style reference into `Collection<FuelLP>`.  Used by
/// downstream consumers (e.g. `Generator.fuel`, `Commitment.fuel`)
/// that resolve the Fuel at LP-build time via
/// `sc.element<FuelLP>(FuelLPSId{...})`.
using FuelLPSId = ObjectSingleId<FuelLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Fuel::class_name` literal fails the build (LP row labels and CSV
// outputs depend on the exact string `"Fuel"`).
static_assert(FuelLP::Element::class_name == LPClassName {"Fuel"},
              "Fuel::class_name must remain \"Fuel\"");

}  // namespace gtopt
