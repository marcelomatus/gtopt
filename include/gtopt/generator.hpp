/**
 * @file      generator.hpp
 * @brief     Header for generator components in power system planning
 * @date      Sat Mar 29 11:52:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing generators (thermal,
 * renewable, hydro) in power system planning models. A generator injects
 * active power at a bus and may be expanded via capacity-planning variables.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "g1",
 *   "bus": "b1",
 *   "pmin": 10,
 *   "pmax": 250,
 *   "gcost": 20,
 *   "capacity": 250
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant (e.g. `100`)
 * - A 2-D inline array indexed by `[stage][block]` (e.g. `[[100, 90], [95]]`)
 * - A filename string referencing a Parquet/CSV schedule in the
 *   `input_directory/Generator/` directory (e.g. `"pmax"`)
 */

#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/emission_capture.hpp>
#include <gtopt/emission_source.hpp>
#include <gtopt/lp_class_name.hpp>

namespace gtopt
{
/**
 * @struct GeneratorAttrs
 * @brief Core technical and economic attributes shared by generator objects
 *
 * Used as a lightweight value type when parsing generator-attribute updates
 * separately from the generator identity fields.
 */
struct GeneratorAttrs
{
  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  OptTBRealFieldSched pmin {};  ///< Minimum active power output [MW]
  OptTBRealFieldSched pmax {};  ///< Maximum active power output [MW]
  OptTBRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
                                      ///< per-(stage, block).  Accepts a
                                      ///< scalar (broadcast), a 2-D nested
                                      ///< array, or a file-backed schedule.
  OptTBRealFieldSched gcost {};  ///< Variable generation cost [$/MWh]
                                 ///< per-(stage, block).  Accepts a scalar
                                 ///< (broadcast), a 2-D nested array
                                 ///< ``[[block0, block1, ...], ...]``, or a
                                 ///< file-backed schedule.  Non-fuel adder
                                 ///< when ``fuel``+``heat_rate`` are set;
                                 ///< see ``Generator`` docstring.

  /// Optional penalty cost [$/MWh] that turns the `pmin` lower bound
  /// into a SOFT constraint.  When set (> 0), the hard floor
  /// ``generation ≥ pmin`` is replaced by ``generation + unserved ≥
  /// pmin`` with a non-negative ``unserved`` slack column priced at
  /// ``pmin_fcost`` in the objective.  Lets a forced-dispatch / must-run
  /// floor (e.g. a PLEXOS Fixed Load trajectory on a thermal unit) be
  /// honoured economically while staying feasible when a transmission /
  /// commitment / ramp limit drives the unit below ``pmin`` — instead of
  /// rendering the LP primal-infeasible.  Unset keeps ``pmin`` hard.
  OptTBRealFieldSched pmin_fcost {};

  /// Optional FK to a `Fuel` element.  When set together with
  /// `heat_rate` (scalar) OR `heat_rate_segments` (piecewise), the
  /// per-MWh fuel cost and combustion emissions are derived from the
  /// Fuel:
  ///
  ///   effective_gcost   = fuel.price        × heat_rate + gcost
  ///   effective_ef      = (fuel.combustion_ef + fuel.upstream_ef)
  ///                       × heat_rate + emission_rate
  ///
  /// Both `gcost` and `emission_rate` are kept as additive offsets
  /// (variable non-combustion O&M / process emissions respectively).
  /// Mirrors PLEXOS `Generator.Fuel` / SDDP `Combustível`.
  OptSingleId fuel {};

  /// Optional per-(stage, block) OVERRIDE of `fuel`.  When set, each
  /// (stage, block) cell resolves to a Fuel by `Uid` and that Fuel
  /// supersedes the static `fuel` field for that cell — the cost,
  /// emission, and `Fuel.max_offtake` bookkeeping all switch to the
  /// per-block fuel.  Cells with the sentinel value `0` (or any uid
  /// not present in `fuel_array`) fall back to the static `fuel`
  /// field, so the matrix only has to enumerate the cells that
  /// actually deviate from the default.
  ///
  /// `Uid`-only (not `SingleId`) on purpose: `Name = std::string` and
  /// `FileSched = std::string` are JSON-indistinguishable, so a
  /// single TB-scheduled field couldn't accept both the inline-name
  /// scalar form (`"Diesel_X"`) and the file-schedule form
  /// (`"fuel_uids.parquet"`).  `Uid` is a numeric strong type — every
  /// variant arm parses unambiguously.
  ///
  /// See Issue #510 Phase 1 for the design rationale.  Phase 2
  /// (endogenous binary-y commitment-style fuel switching) is
  /// deferred to a future `FuelSwitch` element.
  OptTBUidSched fuel_per_block {};

  /// Constant (or per-stage) heat rate slope `[<fuel_unit>/MWh]`.
  /// PLEXOS "Heat Rate" / "Heat Rate Incr" / SDDP "Consumo Específico".
  /// MUTUALLY EXCLUSIVE with `heat_rate_segments` — setting both
  /// raises a validation error.
  OptTBRealFieldSched heat_rate {};

  /// Piecewise-linear convex heat-rate function.  When both arrays
  /// are present, the generation range `[pmin, pmax]` is decomposed
  /// into K segments with strictly INCREASING heat rates (convexity
  /// is required so the LP picks the cheapest segment first by
  /// construction — no binary / SOS-2).  Mirrors PLEXOS
  /// `Generator.Heat Rate Function`.
  ///
  ///   `pmax_segments` = `[P̄₁, …, P̄ₖ]` cumulative MW breakpoints
  ///   `heat_rate_segments` = `[h₁, …, hₖ]` `<fuel_unit>/MWh` slopes
  ///
  /// Segment k covers `[P̄_{k-1}, P̄ₖ]` where `P̄₀ = pmin` and
  /// `P̄ₖ = pmax`.  MUTUALLY EXCLUSIVE with `heat_rate` (scalar).
  /// @{
  Array<Real> pmax_segments {};
  Array<Real> heat_rate_segments {};
  /// @}

  OptTRealFieldSched capacity {};  ///< Installed generation capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable

  /// Initial number of units online at t = 0 [dimensionless].
  /// Sourced from PLEXOS ``Gen_IniUnits.csv``.  Informational on the
  /// gtopt side — the unit-commitment continuity / hot-start state is
  /// carried by ``Commitment.initial_status`` / ``initial_hours`` (one
  /// value per gtopt Generator, which models a single physical unit).
  /// Kept here so the PLEXOS conversion round-trips the raw input
  /// faithfully; future multi-unit / aggregated-generator work can
  /// consume it directly.  Unset → no information published.
  OptReal uini {};
};

/**
 * @struct Generator
 * @brief Represents a generation unit (thermal, renewable, hydro) at a bus
 *
 * A generator injects active power `p ∈ [pmin, pmax]` at its connected bus.
 * The LP objective includes `gcost × power × duration` for operational cost.
 * When `expcap` and `expmod` are non-null the solver may invest in additional
 * capacity modules at cost `annual_capcost` per module per year.
 *
 * @see GeneratorProfile for time-varying capacity-factor profiles
 * @see GeneratorLP for the LP formulation
 */
struct Generator
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `GeneratorLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `Generator::class_name` directly (or
  /// `GeneratorLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Generator"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Generator name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional generator type tag (e.g. "thermal", "hydro",
                    ///< "solar")
  OptName description {};  ///< Free-form label for UI/post-processing; not
                           ///< used by the LP solver.

  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  OptTBRealFieldSched pmin {};  ///< Minimum active power output [MW]
  OptTBRealFieldSched pmax {};  ///< Maximum active power output [MW]
  OptTBRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
                                      ///< per-(stage, block).  Accepts a
                                      ///< scalar (broadcast), a 2-D nested
                                      ///< array, or a file-backed schedule.
  OptTBRealFieldSched gcost {};  ///< Variable generation cost [$/MWh]
                                 ///< per-(stage, block); see
                                 ///< ``GeneratorAttrs::gcost`` for accepted
                                 ///< JSON shapes.

  /// Optional penalty cost [$/MWh] making `pmin` soft via an
  /// ``unserved`` slack.  See ``GeneratorAttrs::pmin_fcost``.
  OptTBRealFieldSched pmin_fcost {};

  /// Optional FK to a `Fuel` element.  See `GeneratorAttrs::fuel`.
  OptSingleId fuel {};

  /// Optional per-(stage, block) override of `fuel`.  See
  /// `GeneratorAttrs::fuel_per_block`.
  OptTBUidSched fuel_per_block {};

  /// Per-(stage, block) heat rate slope [`<fuel_unit>`/MWh].
  /// See `GeneratorAttrs::heat_rate`.
  OptTBRealFieldSched heat_rate {};

  /// Piecewise-linear convex heat-rate function.  See
  /// `GeneratorAttrs::heat_rate_segments`.  Mutually exclusive with
  /// `heat_rate`.
  /// @{
  Array<Real> pmax_segments {};
  Array<Real> heat_rate_segments {};
  /// @}

  OptTRealFieldSched capacity {};  ///< Installed generation capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable

  /// Initial number of units online at t = 0.  See
  /// ``GeneratorAttrs::uini`` for semantics.  Sourced from PLEXOS
  /// ``Gen_IniUnits.csv`` by the plexos2gtopt converter.
  OptReal uini {};

  OptTBRealFieldSched emission_rate {};  ///< Direct CO₂ emission rate
                                         ///< [tCO₂/MWh] per-(stage, block).
                                         ///< Additive with fuel-derived
                                         ///< combustion+upstream when
                                         ///< `fuel`+`heat_rate` are set
                                         ///< (treats `emission_rate` as a
                                         ///< non-combustion adder, e.g.
                                         ///< process / venting / fugitive).

  /**
   * @brief Sets generator attributes from a GeneratorAttrs object
   * @param self  The generator object to update (deduced; supports
   * const/non-const).
   * @param attrs Generator attributes to be set
   * @return Reference to this Generator object
   *
   * @details Example usage:
   *
   * @code{.cpp}
   * Generator gen;
   * GeneratorAttrs attrs;
   * attrs.bus = 1;
   * attrs.pmax = 100.0;
   * gen.set_attrs(std::move(attrs));
   * // gen.bus should now be 1, and attrs.bus should be empty
   * @endcode
   */

  auto& set_attrs(this auto&& self, auto&& attrs)
  {
    self.bus = std::exchange(attrs.bus, {});
    self.pmin = std::exchange(attrs.pmin, {});
    self.pmax = std::exchange(attrs.pmax, {});
    self.lossfactor = std::exchange(attrs.lossfactor, {});
    self.gcost = std::exchange(attrs.gcost, {});
    self.pmin_fcost = std::exchange(attrs.pmin_fcost, {});
    self.fuel = std::exchange(attrs.fuel, {});
    self.fuel_per_block = std::exchange(attrs.fuel_per_block, {});
    self.heat_rate = std::exchange(attrs.heat_rate, {});
    self.pmax_segments = std::exchange(attrs.pmax_segments, {});
    self.heat_rate_segments = std::exchange(attrs.heat_rate_segments, {});

    self.capacity = std::exchange(attrs.capacity, {});
    self.expcap = std::exchange(attrs.expcap, {});
    self.expmod = std::exchange(attrs.expmod, {});
    self.capmax = std::exchange(attrs.capmax, {});
    self.annual_capcost = std::exchange(attrs.annual_capcost, {});
    self.annual_derating = std::exchange(attrs.annual_derating, {});
    self.integer_expmod = std::exchange(attrs.integer_expmod, {});
    self.uini = std::exchange(attrs.uini, {});

    return self;
  }

  /// Inline emission contributions — list of `EmissionSource` rows
  /// scoped to THIS generator.  Each entry carries the destination
  /// zone FK and the per-MWh emission rate; `generator` is set
  /// automatically by `System::expand_emission_sources()` and the
  /// row is appended to the flat `emission_source_array`.  Mirrors
  /// `Reservoir.seepage[]` / `Battery.bus` inline expansion.
  ///
  Array<EmissionSource> emissions {};

  /// Inline CCS / abatement rows for THIS generator, one per pollutant
  /// kind captured.  Each entry's `(1 − rate)` factor scales the
  /// matching `EmissionSource` contribution to its zone's balance row
  /// AND adds `rate × (combustion + upstream) × cost` to the
  /// generator's dispatch-column cost (paid per MWh generated).
  Array<EmissionCapture> emission_captures {};
};

}  // namespace gtopt
