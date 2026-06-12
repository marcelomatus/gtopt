/**
 * @file      generator_lp.hpp
 * @brief     Linear Programming representation of a Generator for optimization
 * @date      Sat Mar 29 00:53:51 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The GeneratorLP class provides a linear programming (LP) compatible
 * representation of a Generator, which is a fundamental component for power
 * system optimization. It maintains the generator's operational constraints
 * and provides methods for LP formulation.
 *
 * @note Uses C++23 features including deducing this and structured bindings
 */

#pragma once

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

class GeneratorLP : public CapacityObjectLP<Generator>
{
public:
  static constexpr std::string_view GenerationName {"generation"};
  static constexpr std::string_view CapacityName {"capacity"};
  /// PAMPL-visible slack column name for the convex piecewise heat-rate
  /// encoding.  Disambiguated by segment index via the outer vector
  /// `heat_rate_slack_cols_` (one holder per segment k = 1..K-1).
  static constexpr std::string_view HeatRateSlackName {"heat_rate_slack"};
  /// PAMPL-visible row name for piecewise kink rows
  /// `p − s_k ≤ pmax_segments[k-1]`.
  static constexpr std::string_view HeatRateKinkName {"heat_rate_kink"};
  /// PAMPL-visible slack column name for the soft-`pmin` encoding: the
  /// non-negative shortfall `unserved ≥ pmin − generation`, priced at
  /// `pmin_fcost`.  Present only on blocks where `pmin_fcost > 0`.
  static constexpr std::string_view UnservedName {"unserved"};
  /// PAMPL-visible row name for the soft-`pmin` floor
  /// `generation + unserved ≥ pmin`.
  static constexpr std::string_view PminSoftName {"pmin_soft"};
  /// Filter metadata keys published by `add_to_lp` for `sum(...)`
  /// predicate matching.
  static constexpr std::string_view TypeKey {"type"};
  static constexpr std::string_view BusKey {"bus"};

  using CapacityBase = CapacityObjectLP<Generator>;

  [[nodiscard]]
  explicit GeneratorLP(const Generator& generator, const InputContext& ic);

  [[nodiscard]] constexpr auto&& generator(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]]
  constexpr auto bus_sid() const noexcept
  {
    return BusLPSId {generator().bus};
  }

  [[nodiscard]]
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  [[nodiscard]]
  bool add_to_output(OutputContext& out) const;

  [[nodiscard]]
  const auto& generation_cols_at(const ScenarioLP& scenario,
                                 const StageLP& stage) const
  {
    return generation_cols.at({scenario.uid(), stage.uid()});
  }

  /// Per-(scenario, stage) map of `unserved` soft-`pmin` slack columns,
  /// keyed by block uid.  Only populated for blocks where the soft-pmin
  /// branch fired (`pmin_fcost > 0` and `pmin > 0`); tolerant lookup —
  /// returns an empty inner map when the outer key is absent.
  [[nodiscard]]
  const auto& lookup_unserved_cols(const ScenarioLP& scenario,
                                   const StageLP& stage) const noexcept
  {
    return find_or_empty_inner(unserved_cols, scenario, stage);
  }

  /// Tolerant inner-map lookup over `generation_cols`.  When every
  /// block of `(scenario, stage)` was skipped by the P1 zero-pmax
  /// optimization, the outer key is absent and `generation_cols_at`
  /// would throw.  Mirrors `WaterwayLP::flow_cols_at`'s tolerant
  /// behaviour via `find_or_empty_inner`.
  [[nodiscard]]
  const auto& lookup_generation_cols(const ScenarioLP& scenario,
                                     const StageLP& stage) const noexcept
  {
    return find_or_empty_inner(generation_cols, scenario, stage);
  }

  /// Tolerant block-level lookup over `generation_cols` — see
  /// `lookup_inner` (`index_holder.hpp`).  Consumers (turbines, etc.)
  /// use this to handle blocks where the P1 zero-pmax skip elided
  /// the gen column, instead of `flat_map::at`-throwing.
  [[nodiscard]]
  std::optional<ColIndex> lookup_generation_col(const ScenarioLP& scenario,
                                                const StageLP& stage,
                                                BlockUid buid) const noexcept
  {
    return lookup_inner(generation_cols, scenario, stage, buid);
  }

  [[nodiscard]]
  const auto& capacity_rows_at(const ScenarioLP& scenario,
                               const StageLP& stage) const
  {
    return capacity_rows.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_pmax(StageUid s, BlockUid b) const
  {
    return pmax.at(s, b);
  }
  [[nodiscard]] auto param_pmin(StageUid s, BlockUid b) const
  {
    return pmin.at(s, b);
  }
  [[nodiscard]] auto param_gcost(StageUid s, BlockUid b) const
  {
    return gcost.at(s, b);
  }
  [[nodiscard]] auto param_heat_rate(StageUid s, BlockUid b) const
  {
    return heat_rate.at(s, b);
  }
  [[nodiscard]] auto param_lossfactor(StageUid s, BlockUid b) const
  {
    return lossfactor.at(s, b);
  }
  [[nodiscard]] auto param_emission_rate(StageUid s, BlockUid b) const
  {
    return emission_rate.at(s, b);
  }
  /// @}

  /// True iff this generator carries a per-(stage, block) fuel override
  /// schedule (`Generator.fuel_per_block`).  When true, callers should
  /// consult `param_fuel_per_block(stage, block)` to obtain the resolved
  /// uid for each block and fall back to `Generator.fuel` only when the
  /// per-block value is the sentinel uid 0 (or absent from `fuel_array`).
  [[nodiscard]] bool has_fuel_per_block() const noexcept
  {
    return fuel_per_block_.has_value();
  }

  /// Resolved per-(stage, block) fuel-uid override.  Returns `nullopt`
  /// only when `has_fuel_per_block()` is false; when the schedule is
  /// set but the cell is unspecified, the OptSchedule layer returns
  /// the schedule's default (`Uid {0}` by convention), which callers
  /// interpret as "no override, use the static `Generator.fuel`".
  [[nodiscard]] std::optional<Uid> param_fuel_per_block(StageUid s,
                                                        BlockUid b) const
  {
    return fuel_per_block_.optval(s, b);
  }

  /// True iff this generator has piecewise-linear heat-rate segments
  /// (both `pmax_segments` and `heat_rate_segments` non-empty + same
  /// length).  When true, `add_to_lp` installs K−1 slack columns +
  /// K−1 kink rows on top of the single primary generation column to
  /// express the convex piecewise cost.
  [[nodiscard]] bool has_heat_rate_segments() const noexcept
  {
    return !object().pmax_segments.empty()
        && object().pmax_segments.size() == object().heat_rate_segments.size();
  }

  /// Resolved per-segment piecewise slack columns, ordered as
  /// `[s_1, s_2, ..., s_{K-1}]` matching breakpoints
  /// `[pmax_segments[0], ..., pmax_segments[K-2]]`.  Empty when
  /// `has_heat_rate_segments()` is false.  Used by
  /// `add_emission_cap` (in `system_lp.cpp`) to inject per-segment
  /// emission coefficients alongside the primary `generation_cols`
  /// entry.
  [[nodiscard]] const auto& heat_rate_slack_cols() const noexcept
  {
    return heat_rate_slack_cols_;
  }

  /// `[segment_index] -> STBIndexHolder<RowIndex>` of piecewise kink
  /// rows.  Same shape as ``heat_rate_slack_cols_``.  Exposed so
  /// ``CommitmentLP::add_to_lp`` can retro-fit the rows with the
  /// ``-pmax_segs[k-1] · u`` term that gates the segment by the
  /// commitment binary — the refactor that moved piecewise off
  /// Commitment dropped that u-link, producing an over-loose LP-relax
  /// (fractional ``u`` could dispatch full piecewise capacity).  See
  /// ``commitment_lp.cpp`` "u-gate piecewise heat-rate kink rows".
  [[nodiscard]] const auto& heat_rate_kink_rows() const noexcept
  {
    return heat_rate_kink_rows_;
  }

  /// Per-segment cumulative breakpoints (cumulative MW) used by the
  /// u-gating retro-fit in ``CommitmentLP::add_to_lp``.  Mirrors
  /// ``Generator.pmax_segments[0..K-2]`` (the kink RHS values) — the
  /// generator-side accessor avoids forcing CommitmentLP to re-read
  /// the OptTBRealSched and re-resolve the per-block sample.  Empty
  /// when no segments are configured.
  [[nodiscard]] const auto& heat_rate_kink_breakpoints() const noexcept
  {
    return heat_rate_kink_breakpoints_;
  }

private:
  OptTBRealSched pmin;
  OptTBRealSched pmax;
  OptTBRealSched lossfactor;
  OptTBRealSched gcost;
  OptTBRealSched pmin_fcost;
  OptTBRealSched heat_rate;
  OptTBRealSched emission_rate;
  /// Resolved per-(stage, block) fuel-uid override; backs
  /// `param_fuel_per_block` / `has_fuel_per_block`.  Mirrors
  /// `Generator.fuel_per_block` after `InputContext` array-index
  /// resolution.  When the parent `Generator.fuel_per_block` is
  /// unset, this stays empty and the LP wiring falls back to the
  /// static `Generator.fuel` exactly as before — byte-for-byte.
  OptTBUidSchedRT fuel_per_block_;

  STBIndexHolder<ColIndex> generation_cols;
  /// `unserved` soft-`pmin` slack columns (see `lookup_unserved_cols`).
  STBIndexHolder<ColIndex> unserved_cols;
  STBIndexHolder<RowIndex> capacity_rows;
  /// `[segment_index] -> STBIndexHolder<ColIndex>` of piecewise slack
  /// columns.  Outer vector has size `K - 1` where `K` is the number
  /// of heat-rate segments.  Empty when no segments are configured.
  std::vector<STBIndexHolder<ColIndex>> heat_rate_slack_cols_;
  /// Companion to ``heat_rate_slack_cols_``: the row indices of the
  /// kink rows ``p - δ_k ≤ pmax_segs[k-1]``.  CommitmentLP retrieves
  /// these to add the ``- pmax_segs[k-1] · u`` term that gates the
  /// segment by the commit binary.
  std::vector<STBIndexHolder<RowIndex>> heat_rate_kink_rows_;
  /// Per-segment breakpoint values used as the RHS of the kink rows
  /// at emission time.  CommitmentLP reads this to compute the
  /// retro-fit coefficient ``- pmax_segs[k-1]`` on ``ucol``.
  std::vector<double> heat_rate_kink_breakpoints_;

  /// Cached per-block cost-stack components (physical $/MWh, source-
  /// schedule path — Path A in the design doc).  Populated during
  /// `add_to_lp` and emitted as
  /// `Generator/{vom_cost,fuel_cost,srmc}_sol.parquet` by `add_to_output`.
  /// PLEXOS-aligned naming:
  ///   - VOM Cost: `Generator.gcost(stage, block)`.
  ///   - Fuel Cost: `heat_rate · fuel.price` (primary segment for piecewise).
  ///   - SRMC: VOM + Fuel (Short-Run Marginal Cost; matches the
  ///     coefficient on the primary generation column).  Emission
  ///     tax adds in via EmissionZone.price on a separate column.
  STBIndexHolder<double> vom_cost_values_;
  STBIndexHolder<double> fuel_cost_values_;
  STBIndexHolder<double> srmc_values_;
  /// Per-block resolved fuel uid (Issue #510 Phase 1) for emission of
  /// `Generator/fuel_sol.parquet`.  Stored as `double` so it can ride
  /// the existing per-block value-output infrastructure; the value is
  /// always an integer Uid in [0, INT32_MAX] so the conversion is exact.
  /// Populated by `add_to_lp` ONLY when `has_fuel_per_block()` — leaving
  /// it empty for the legacy single-fuel case (the static `Generator.fuel`
  /// uid is already in the input JSON, so a constant per-block parquet
  /// would be pure redundancy — see Issue #510 acceptance criterion #4).
  STBIndexHolder<double> fuel_uid_values_;
};

using GeneratorLPId = ObjectId<GeneratorLP>;
using GeneratorLPSId = ObjectSingleId<GeneratorLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Generator::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Generator"`).
static_assert(GeneratorLP::Element::class_name == LPClassName {"Generator"},
              "Generator::class_name must remain \"Generator\"");

}  // namespace gtopt
