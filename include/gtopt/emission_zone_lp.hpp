/**
 * @file      emission_zone_lp.hpp
 * @brief     LP-active wrapper for the EmissionZone bridge entity
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionZoneLP` owns only the LP artefacts that are actually
 * needed.  The per-block `production` column and the `balance` row
 * have been **substituted out**: every emission contribution to a
 * constraint or to the objective is wired directly on the
 * **generator dispatch column** by `EmissionSourceLP`, using the
 * per-block scalar
 *
 *   α_{s,b}  =  weight_z,p · (1 − capture_s,p) · (rate_s + upstream_s) · dur_b
 *
 * (tonnes of CO₂-eq per MW of dispatch, for source `s`, pollutant
 * `p`, in block `b`).
 *
 * What the zone DOES build, gated by which fields are set:
 *
 *   * `cap` set     → a per-stage cap row
 *                     `Σ_b Σ_{s ∈ sources(z)} α_{s,b} · gen_{s,b} ≤ cap_z,s`
 *                     (coefficients injected by `EmissionSourceLP`).
 *   * `cap_cost`    → a slack column on the cap row, penalised at
 *                     `cap_cost · prob · discount` (cap becomes soft).
 *   * `price` set OR
 *     `objective_mode = "emissions"`
 *                   → an objective-coefficient adder on every
 *                     generator dispatch column: `+ price · α_{s,b}`
 *                     ($/MWh scaled through `block_ecost`, mirroring
 *                     the CCS opex adder already on
 *                     `EmissionSourceLP`).
 *   * `allowance_pool` set
 *                   → each generator dispatch column is injected with
 *                     coefficient `+ α_{s,b}` (tCO₂) into the
 *                     referenced `AllowancePoolLP`'s per-block energy-
 *                     balance row.  The pool's banked SoC then becomes
 *                     the binding multi-stage cap; the per-stage `cap`
 *                     row is skipped on this zone.
 *
 * When **none** of `cap`, `price`, `allowance_pool`, or
 * `objective_mode = "emissions"` applies, the zone is *pure
 * reporting*: no LP column, no LP row.  The per-source emission
 * streams (`EmissionSource/emissions_sol.parquet` etc.) are still
 * produced post-solve from generator primals × `α` factors cached
 * inside `EmissionSourceLP`, so the cost of "zone configured but no
 * constraint set" is now zero LP variables and zero LP rows.
 *
 * `AllowancePoolLP` is visited before `EmissionZoneLP` (see
 * `system_lp.hpp` `collections_t` ordering) so its energy rows exist
 * when sources inject their α-weighted coefficients.
 */

#pragma once

#include <gtopt/emission_zone.hpp>
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

class EmissionZoneLP : public ObjectLP<EmissionZone>
{
public:
  using Base = ObjectLP<EmissionZone>;

  /// Row / col name constants (snake_case for output filenames).
  /// `production` / `balance` are no longer emitted — the column and
  /// equality row have been substituted out (see file header).
  static constexpr std::string_view CapName {"cap"};
  static constexpr std::string_view CapSlackName {"cap_slack"};

  explicit EmissionZoneLP(const EmissionZone& zone, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission_zone(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_cap(StageUid s) const { return cap_.at(s); }
  [[nodiscard]] auto param_cap_cost(StageUid s) const
  {
    return cap_cost_.at(s);
  }
  [[nodiscard]] auto param_price(StageUid s) const { return price_.at(s); }
  /// @}

  /// @name LP-row accessors — consumed by `EmissionSourceLP` at
  /// `add_to_lp` time to inject the per-block α coefficient into the
  /// matching cap row (when one exists for this `(scenario, stage)`).
  /// Reporting-only zones have no entries here — sources then only
  /// charge price / pool / CCS adders if those apply, and otherwise
  /// just cache factors for the per-source output streams.
  /// @{
  [[nodiscard]] constexpr const auto& cap_rows() const noexcept
  {
    return cap_rows_;
  }
  [[nodiscard]] constexpr const auto& cap_slack_cols() const noexcept
  {
    return cap_slack_cols_;
  }
  /// @}

private:
  // Schedule parameter holders
  OptTRealSched cap_;
  OptTRealSched cap_cost_;
  OptTRealSched price_;

  // Per-(scenario, stage) cap row + optional slack column.  Only
  // populated when `EmissionZone.cap` is set for that stage.
  STIndexHolder<RowIndex> cap_rows_;
  STIndexHolder<ColIndex> cap_slack_cols_;
};

using EmissionZoneLPSId = ObjectSingleId<EmissionZoneLP>;

static_assert(EmissionZoneLP::Element::class_name
                  == LPClassName {"EmissionZone"},
              "EmissionZone::class_name must remain \"EmissionZone\"");

}  // namespace gtopt
