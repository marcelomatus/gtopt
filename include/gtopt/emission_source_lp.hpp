/**
 * @file      emission_source_lp.hpp
 * @brief     LP-active wrapper for the EmissionSource bridge entity
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Resolves the per-stage combustion and upstream emission rates at
 * construction and exposes them via `param_rate` / `param_upstream_rate`.
 * In `add_to_lp` injects `weight · (1 − capture) · (rate + upstream) ·
 * dur` into every generator-dispatch column of its zone's balance row,
 * and adds a `capture · (rate + upstream) · capture_cost` adder when
 * CCS is configured for the matching (generator, pollutant) pair.
 */

#pragma once

#include <gtopt/emission_source.hpp>
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

class EmissionSourceLP : public ObjectLP<EmissionSource>
{
public:
  using Base = ObjectLP<EmissionSource>;

  explicit EmissionSourceLP(const EmissionSource& src, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission_source(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Inject `−weight · (1 − capture) · (rate + upstream) · dur` into
  /// the zone's balance row at every generator-dispatch column of this
  /// (scenario, stage, block), and add a per-MWh CCS opex adder when
  /// `Generator.emission_captures[]` carries a row for this pollutant.
  /// `EmissionZoneLP::add_to_lp` must run BEFORE this — guaranteed by
  /// the `lp_element_types_t` visitor ordering (same dependency
  /// `InertiaProvisionLP` has on `InertiaZoneLP`).
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  /// Emits per-source per-block emission breakdown streams.  Reads
  /// the dispatch primal via `OutputContext::primal(gen_col)` and
  /// multiplies by the per-(stage, block) factors cached in
  /// `add_to_lp` to produce:
  ///
  ///   `EmissionSource/emissions_sol.parquet`   — NET tons (post-capture)
  ///   `EmissionSource/combustion_sol.parquet`  — combustion-only post-capture
  ///   `EmissionSource/upstream_sol.parquet`    — WTT post-capture (only
  ///                                               emitted when upstream_rate
  ///                                               is non-zero at any
  ///                                               (s, t, b))
  ///   `EmissionSource/captured_sol.parquet`    — captured tons (only when
  ///                                               capture_rate > 0)
  ///   `EmissionSource/rate_sol.parquet`        — combustion rate [t/MWh],
  ///                                               replicated per block from
  ///                                               the per-stage schedule.
  ///   `EmissionSource/upstream_rate_sol.parquet`
  ///                                            — WTT rate [t/MWh] (only
  ///                                               emitted when upstream_rate
  ///                                               is non-zero).
  ///
  /// All tons values are physical tons per block, computed Path A from
  /// source schedules + post-solve generation primal; rate streams are
  /// the raw input coefficient broadcast to the (scenario, stage, block)
  /// grid for ease of post-processing in tools/notebooks.
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// @name Parameter accessors (resolved schedules)
  /// @{
  [[nodiscard]] auto param_rate(StageUid s) const { return rate_.at(s); }
  [[nodiscard]] auto param_upstream_rate(StageUid s) const
  {
    return upstream_rate_.at(s);
  }
  /// @}

private:
  OptTRealSched rate_;
  OptTRealSched upstream_rate_;

  // Caches populated in add_to_lp and consumed in add_to_output to
  // reconstruct the four per-source emission streams.  All four maps
  // share the same (scenario, stage, block) key set.
  STBIndexHolder<ColIndex> gen_cols_;
  /// `(1 − capture) · rate · dur` per (s, t, b)  — combustion-only
  /// multiplier on `gen_sol[col]`.
  STBIndexHolder<double> combustion_factor_;
  /// `(1 − capture) · upstream · dur` per (s, t, b).
  STBIndexHolder<double> upstream_factor_;
  /// `capture · (rate + upstream) · dur` per (s, t, b).
  STBIndexHolder<double> captured_factor_;
  /// Combustion `rate` [t/MWh] per (s, t, b) — broadcast of the
  /// per-stage scalar across the stage's blocks.  Constant inside a
  /// stage by construction.
  STBIndexHolder<double> rate_values_;
  /// Upstream `upstream_rate` [t/MWh] per (s, t, b) — same shape
  /// as `rate_values_`, only populated when upstream_rate is set.
  STBIndexHolder<double> upstream_rate_values_;
};

using EmissionSourceLPSId = ObjectSingleId<EmissionSourceLP>;

static_assert(EmissionSourceLP::Element::class_name
                  == LPClassName {"EmissionSource"},
              "EmissionSource::class_name must remain \"EmissionSource\"");

}  // namespace gtopt
