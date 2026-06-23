/**
 * @file      system.cpp
 * @brief     Header of System class methods
 * @date      Sun Mar 30 16:04:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the methods of the System class, which represents the
 * core data.
 */

#include <algorithm>
#include <set>

#include <gtopt/as_label.hpp>
#include <gtopt/bus_island.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace
{
/**
 * @brief Returns the next available UID for an element array
 * @tparam T Element type (must have .uid member)
 * @param arr Array of elements
 * @return One past the maximum existing UID, or 1 if array is empty
 */
template<typename T>
[[nodiscard]] auto next_uid(const gtopt::Array<T>& arr) -> gtopt::Uid
{
  if (arr.empty()) {
    return 1;
  }
  auto max_it = std::ranges::max_element(
      arr, {}, [](const auto& elem) { return elem.uid; });
  return max_it->uid + 1;
}

/**
 * @brief Build a set of UIDs from an element array
 * @tparam T Element type (must have .uid member)
 * @param arr Array of elements
 * @return Set of existing UIDs
 */
template<typename T>
[[nodiscard]] auto build_uid_set(const gtopt::Array<T>& arr)
    -> std::set<gtopt::Uid>
{
  std::set<gtopt::Uid> result;
  for (const auto& elem : arr) {
    result.insert(elem.uid);
  }
  return result;
}

/**
 * @brief Build a set of names from an element array
 * @tparam T Element type (must have .name member)
 * @param arr Array of elements
 * @return Set of existing names
 */
template<typename T>
[[nodiscard]] auto build_name_set(const gtopt::Array<T>& arr)
    -> std::set<std::string>
{
  std::set<std::string> result;
  for (const auto& elem : arr) {
    result.insert(elem.name);
  }
  return result;
}

}  // namespace

namespace gtopt
{
namespace
{
/// Negate every numeric value inside an OptTBRealFieldSched, preserving
/// the variant shape (scalar → scalar, 2-D vector → 2-D vector).
/// File-backed schedules are passed through unchanged because we
/// cannot eagerly load + negate at expand time; callers that wire
/// ``charge_cost`` via a Parquet/CSV file route must already store
/// the value pre-negated.  Used by ``expand_batteries`` to push
/// ``Battery.charge_cost`` onto the synthetic Demand.fcost as a
/// negative coefficient (so the demand-LP substitution
/// ``lcol_cost = -fcost × block_factor`` lands on a positive per-MWh
/// cost on the charging column).
[[nodiscard]] auto negate_real_sched(const OptTBRealFieldSched& src)
    -> OptTBRealFieldSched
{
  if (!src.has_value()) {
    return std::nullopt;
  }
  return std::visit(
      [](const auto& v) -> RealFieldSched2
      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, double>) {
          return -v;
        } else if constexpr (std::is_same_v<T,
                                            std::vector<std::vector<double>>>) {
          std::vector<std::vector<double>> negated;
          negated.reserve(v.size());
          for (const auto& row : v) {
            std::vector<double> nrow;
            nrow.reserve(row.size());
            for (auto x : row) {
              nrow.push_back(-x);
            }
            negated.push_back(std::move(nrow));
          }
          return negated;
        } else {
          return v;
        }
      },
      *src);
}
}  // namespace

void System::setup_reference_bus(const PlanningOptionsLP& options)
{
  detect_islands_and_fix_references(bus_array, line_array, options);
}

void System::expand_batteries()
{
  // Compute starting UIDs for auto-generated elements.
  // These must be computed before appending to avoid shifting during iteration.
  auto gen_uid = next_uid(generator_array);
  auto dem_uid = next_uid(demand_array);
  auto conv_uid = next_uid(converter_array);
  auto bus_uid = next_uid(bus_array);
  auto cmt_uid = next_uid(commitment_array);

  for (auto& battery : battery_array) {
    if (!battery.bus.has_value()) {
      continue;  // traditional multi-element definition — skip
    }

    const auto gen_name = battery.name + "_gen";
    const auto dem_name = battery.name + "_dem";
    const auto conv_name = battery.name + "_conv";

    // Determine charge bus: internal (coupled mode) or external (standalone)
    SingleId charge_bus = *battery.bus;

    if (battery.source_generator.has_value()) {
      // Generation-coupled mode: create internal bus for the charge path
      const auto int_bus_name = battery.name + "_int_bus";
      bus_array.push_back(Bus {
          .uid = bus_uid,
          .name = int_bus_name,
      });
      charge_bus = SingleId {Uid {bus_uid}};
      bus_uid++;

      // Find the referenced source generator and set its bus to the internal
      // bus, overwriting any previously set bus value.
      const auto src_id = *battery.source_generator;  // captured by value
      auto it =
          std::ranges::find_if(generator_array,
                               [src_id](const Generator& g)
                               {
                                 if (std::holds_alternative<Uid>(src_id)) {
                                   return g.uid == std::get<Uid>(src_id);
                                 }
                                 return g.name == std::get<Name>(src_id);
                               });
      if (it != generator_array.end()) {
        it->bus = charge_bus;
      } else {
        const auto src_label = std::visit(
            [](const auto& v) { return std::format("{}", v); }, src_id);
        SPDLOG_WARN(
            "Battery '{}': source_generator '{}' not found in generator_array",
            battery.name,
            src_label);
      }
      battery.source_generator.reset();
    }

    // Discharge generator: power injected into the external bus.
    //
    // ``pmax_discharge`` (TB) → ``Generator.pmax`` — per-(stage,
    // block) operational ceiling.  ``Generator.pmin`` stays unset
    // (0) on purpose: the per-unit min stable level flows through
    // the synthesised ``Commitment.pmin`` below, and ConverterLP
    // looks up the resulting ``u_commit`` from CommitmentLP for
    // both discharge and charge gating ("one true source for
    // u_commit").  Putting a positive ``Generator.pmin`` here would
    // re-introduce a hard always-on floor that conflicts with the
    // commitment-conditional row ``gen ≥ Commitment.pmin × u``.
    //
    // ``Generator.capacity`` is left unset: the default capacity
    // sentinel is ``numeric_limits<double>::max()`` (unlimited), so
    // ``block_pmax = min(stage_capacity, pmax_at) = pmax_at``.  No
    // expansion column is created (batteries express their
    // investment side via ``Battery.expcap`` on the energy axis).
    generator_array.push_back(Generator {
        .uid = gen_uid++,
        .name = gen_name,
        .bus = *battery.bus,
        .pmax = battery.pmax_discharge,
        .gcost = battery.discharge_cost,
    });

    // Charge demand: power absorbed from the charge bus.
    //
    // ``pmax_charge`` (TB) → ``Demand.lmax`` — per-(stage, block)
    // operational ceiling.  ``pmin_charge`` (TB) → ``Demand.lmin`` —
    // HARD per-block floor.  Mirrors UC.jl ``Maximum/Minimum charge
    // rate (MW)`` and PLEXOS ``Max/Min Load`` on the charge side.
    //
    // ``fcost`` selects between two regimes:
    //
    //   * ``Battery.charge_cost`` unset (default): pin to 0 so the
    //     synthetic demand is truly dispatchable in [0, pmax_charge]
    //     regardless of the global ``model_options.demand_fail_cost`` —
    //     otherwise a positive global default would force the LP to
    //     charge at pmax to avoid the per-MWh fail-cost penalty.
    //   * ``Battery.charge_cost`` set: negate it so the demand-LP
    //     substitution (``demand_lp.cpp::add_to_lp``) stamps the
    //     charging column with a positive cost coefficient
    //     ``lcol_cost = -fcost × block_factor = +charge_cost × ...``.
    //     LP then pays ``charge_cost`` per MWh charged, mirroring
    //     UC.jl's ``Charge cost ($/MW)`` semantics.
    OptTBRealFieldSched dem_fcost = battery.charge_cost.has_value()
        ? negate_real_sched(battery.charge_cost)
        : OptTBRealFieldSched {RealFieldSched2 {0.0}};
    demand_array.push_back(Demand {
        .uid = dem_uid++,
        .name = dem_name,
        .bus = charge_bus,
        .lmax = battery.pmax_charge,
        .lmin = battery.pmin_charge,
        .fcost = std::move(dem_fcost),
    });

    // Converter linking battery, generator, and demand.  The
    // ``commitment`` flag still propagates (legacy path: ConverterLP
    // generates its own u_charge / u_discharge binaries).  In
    // parallel we now ALSO synthesise a ``Commitment`` element on
    // ``<bat>_gen`` (see below) so the gen-side u is reachable via
    // the standard CommitmentLP discovery — same machinery that
    // already powers reserve / inertia / urmin-drmin gating for
    // thermal units.  The single-source-of-truth migration (drop
    // Converter.commitment, have ConverterLP look up the gen's
    // CommitmentLP) is in progress; until that lands the duplication
    // is harmless because ConverterLP only creates binaries when
    // ``Converter.commitment = true``, and the legacy code path
    // tests assume that semantic.
    converter_array.push_back(Converter {
        .uid = conv_uid++,
        .name = conv_name,
        .battery = Name {battery.name},
        .generator = Name {gen_name},
        .demand = Name {dem_name},
        .commitment = battery.commitment,
    });

    // Synthesise a Commitment for the battery's discharge generator
    // UNCONDITIONALLY.  The ``<bat>_gen`` Generator always exists
    // (created above), so its companion ``uc_<bat>_gen`` Commitment
    // must always exist too: PLEXOS-derived UserConstraints routinely
    // reference ``commitment("uc_<bat>_gen").status`` for system
    // min-units / inertia rows (e.g. ``CSF_MinUnits``), and PLEXOS
    // itself synthesises an internal commitment binary from the
    // battery's ``Units`` property for every battery.  Previously
    // gtopt created the commitment only when the battery carried
    // ``pmin_discharge`` / ``pmin_charge`` / explicit
    // ``commitment = true`` — so a UC referencing a plain battery's
    // commitment crashed the resolver with "element is missing or
    // inactive" (observed on CEN PCP CSF_MinUnits →
    // ``uc_BAT_MANZANO_FV_gen``).
    //
    // The commitment owns the ``u`` variable that gates dispatch
    // direction and reserve provision; ``Commitment.pmin`` carries
    // the per-unit Min Discharge Level (PLEXOS ``Generator.Min
    // Stable Level`` semantics).  ``relax = true`` keeps the LP path
    // LP-only (continuous ``u ∈ [0, 1]``) UNLESS the modeller opts
    // into MIP via ``Battery.commitment = true`` — then the binary
    // is honoured.  When the battery carries no commitment economics
    // (no pmin, no explicit flag) the relaxed ``u`` is free in
    // ``[0, 1]`` at zero cost, exactly mirroring PLEXOS's "Units"
    // availability flag: it provides the column UCs reference
    // without distorting the dispatch optimum.
    Commitment bat_commit {
        .uid = cmt_uid++,
        .name = "uc_" + gen_name,
        .generator = Name {gen_name},
        // Honour an explicit MIP request; otherwise stay LP-relaxed
        // so the always-on synthesis adds no integer columns.
        .relax = OptBool {!battery.commitment.value_or(false)},
    };
    // Carry the discharge-side floor as the commitment pmin (the
    // gen-side per-unit min stable level).  Charge-side gating is
    // applied by ConverterLP using the same u column.
    //
    // Both ``Commitment.pmin`` and ``Battery.pmin_discharge`` are
    // ``OptTBRealFieldSched`` (variant of scalar / per-block vector /
    // file), so the discharge floor — scalar or full per-block
    // schedule — carries over verbatim.
    if (battery.pmin_discharge.has_value()) {
      bat_commit.pmin = battery.pmin_discharge;
    }
    commitment_array.push_back(std::move(bat_commit));

    SPDLOG_TRACE(
        std::format("Expanded battery '{}': gen='{}' dem='{}' conv='{}'",
                    battery.name,
                    gen_name,
                    dem_name,
                    conv_name));

    // Clear bus so re-expansion is idempotent
    battery.bus.reset();
  }
}

void System::expand_reservoir_constraints()
{
  auto seep_uid = next_uid(reservoir_seepage_array);
  auto dlim_uid = next_uid(reservoir_discharge_limit_array);
  auto pfac_uid = next_uid(reservoir_production_factor_array);

  // Build sets of existing UIDs/names for O(1) duplicate detection
  auto seep_uids = build_uid_set(reservoir_seepage_array);
  auto seep_names = build_name_set(reservoir_seepage_array);
  auto dlim_uids = build_uid_set(reservoir_discharge_limit_array);
  auto dlim_names = build_name_set(reservoir_discharge_limit_array);
  auto pfac_uids = build_uid_set(reservoir_production_factor_array);
  auto pfac_names = build_name_set(reservoir_production_factor_array);

  for (auto& rsv : reservoir_array) {
    const SingleId rsv_id {rsv.uid};

    for (auto& s : rsv.seepage) {
      if (s.uid == unknown_uid || seep_uids.contains(s.uid)) {
        s.uid = seep_uid++;
      }
      if (s.name.empty() || seep_names.contains(s.name)) {
        // `as_label(name, "seepage", uid)` formats uid via `to_chars`
        // into a stack buffer and joins all parts with the `_`
        // separator in a single allocation.  Saves the two extra
        // allocations the previous `name + "_seepage_" +
        // std::to_string(uid)` chain incurred.
        s.name = as_label(rsv.name, "seepage", s.uid);
      }
      s.reservoir = rsv_id;
      seep_uids.insert(s.uid);
      seep_names.insert(s.name);
      reservoir_seepage_array.push_back(std::move(s));
    }
    rsv.seepage.clear();

    for (auto& d : rsv.discharge_limit) {
      if (d.uid == unknown_uid || dlim_uids.contains(d.uid)) {
        d.uid = dlim_uid++;
      }
      if (d.name.empty() || dlim_names.contains(d.name)) {
        // See `as_label` rationale on the analogous seepage branch
        // above — 3 allocs → 1.
        d.name = as_label(rsv.name, "dlim", d.uid);
      }
      d.reservoir = rsv_id;
      dlim_uids.insert(d.uid);
      dlim_names.insert(d.name);
      reservoir_discharge_limit_array.push_back(std::move(d));
    }
    rsv.discharge_limit.clear();

    for (auto& p : rsv.production_factor) {
      if (p.uid == unknown_uid || pfac_uids.contains(p.uid)) {
        p.uid = pfac_uid++;
      }
      if (p.name.empty() || pfac_names.contains(p.name)) {
        // See `as_label` rationale on the analogous seepage branch
        // above — 3 allocs → 1.
        p.name = as_label(rsv.name, "pfac", p.uid);
      }
      p.reservoir = rsv_id;
      pfac_uids.insert(p.uid);
      pfac_names.insert(p.name);
      reservoir_production_factor_array.push_back(std::move(p));
    }
    rsv.production_factor.clear();
  }
}

void System::expand_emission_sources()
{
  auto src_uid = next_uid(emission_source_array);
  auto src_uids = build_uid_set(emission_source_array);
  auto src_names = build_name_set(emission_source_array);

  for (auto& gen : generator_array) {
    if (gen.emissions.empty()) {
      continue;
    }
    const SingleId gen_id {gen.uid};
    for (auto& es : gen.emissions) {
      if (es.uid == unknown_uid || src_uids.contains(es.uid)) {
        es.uid = src_uid++;
      }
      if (es.name.empty() || src_names.contains(es.name)) {
        // `as_label(gen.name, "emission", uid)` — 3 allocs → 1.  See
        // expand_reservoir_constraints() for the same idiom.
        es.name = as_label(gen.name, "emission", es.uid);
      }
      es.generator = OptSingleId {gen_id};
      src_uids.insert(es.uid);
      src_names.insert(es.name);
      emission_source_array.push_back(std::move(es));
    }
    gen.emissions.clear();
  }
}

namespace
{

/// Extract a scalar `Real` from an `Opt*FieldSched`; returns
/// `nullopt` for unset, vector, or FileSched forms.  Works on both
/// the 1D `OptTRealFieldSched` and the 2D `OptTBRealFieldSched`
/// because both wrap `FieldSched<Real, …>` — the scalar arm is
/// `Real` in either case.
template<typename T, typename Vector>
[[nodiscard]] std::optional<Real> scalar_or(
    const std::optional<FieldSched<T, Vector>>& field)
{
  if (!field.has_value() || !std::holds_alternative<T>(*field)) {
    return std::nullopt;
  }
  return std::get<T>(*field);
}

/// Resolve a `Generator.fuel` SingleId against `fuel_array`.  Returns
/// nullptr when the reference dangles (validation catches this earlier;
/// the lookup is robust against the absence of validation in tests).
[[nodiscard]] const Fuel* find_fuel(const gtopt::Array<Fuel>& fuel_array,
                                    const SingleId& ref) noexcept
{
  const auto it =
      std::ranges::find_if(fuel_array,
                           [&](const Fuel& f)
                           {
                             // get_if (non-throwing) keeps this noexcept lookup
                             // free of bad_variant_access; the alternatives are
                             // discriminated here.
                             if (const auto* uid = std::get_if<Uid>(&ref)) {
                               return f.uid == *uid;
                             }
                             const auto* name = std::get_if<Name>(&ref);
                             return name != nullptr && f.name == *name;
                           });
  return it == fuel_array.end() ? nullptr : &*it;
}

/// Format the emission label component of an auto-generated source
/// name — uses the user-friendly `co2`/`nox`/… name when the FK is
/// a `Name`, falls back to `uid<N>` when it's a numeric `Uid`.
[[nodiscard]] std::string emission_label(const SingleId& ref)
{
  if (std::holds_alternative<Uid>(ref)) {
    return std::format("uid{}", std::get<Uid>(ref));
  }
  return std::get<Name>(ref);
}

}  // namespace

void System::fold_legacy_fuel_emission_factors()
{
  const bool any_legacy =
      std::ranges::any_of(fuel_array,
                          [](const Fuel& f)
                          {
                            return f.combustion_emission_factor.has_value()
                                || f.upstream_emission_factor.has_value();
                          });
  if (!any_legacy) {
    return;
  }

  // Ensure the CO₂ pollutant tag exists; legacy fields are CO₂-only.
  auto co2_it = std::ranges::find_if(
      emission_array, [](const Emission& e) { return e.name == "co2"; });
  if (co2_it == emission_array.end()) {
    emission_array.push_back(
        Emission {.uid = next_uid(emission_array), .name = "co2"});
    co2_it = std::prev(emission_array.end());
  }
  const auto co2_uid = co2_it->uid;

  for (auto& f : fuel_array) {
    if (!f.combustion_emission_factor.has_value()
        && !f.upstream_emission_factor.has_value())
    {
      continue;
    }

    auto fef_it =
        std::ranges::find_if(f.emission_factors,
                             [co2_uid](const FuelEmissionFactor& fef)
                             {
                               return std::holds_alternative<Uid>(fef.emission)
                                   && std::get<Uid>(fef.emission) == co2_uid;
                             });

    if (fef_it == f.emission_factors.end()) {
      f.emission_factors.push_back(FuelEmissionFactor {
          .emission = SingleId {co2_uid},
          .combustion = std::move(f.combustion_emission_factor),
          .upstream = std::move(f.upstream_emission_factor),
      });
    } else {
      // Merge legacy fields into an existing CO₂ row — legacy only
      // fills slots that are currently null (new table wins on conflict).
      if (!fef_it->combustion.has_value()) {
        fef_it->combustion = std::move(f.combustion_emission_factor);
      }
      if (!fef_it->upstream.has_value()) {
        fef_it->upstream = std::move(f.upstream_emission_factor);
      }
    }

    f.combustion_emission_factor.reset();
    f.upstream_emission_factor.reset();
  }
}

void System::expand_fuel_emission_sources()
{
  if (fuel_array.empty() || generator_array.empty()
      || emission_zone_array.empty())
  {
    return;
  }

  auto src_uid = next_uid(emission_source_array);
  auto src_uids = build_uid_set(emission_source_array);
  auto src_names = build_name_set(emission_source_array);

  for (auto& gen : generator_array) {
    if (!gen.fuel.has_value()) {
      continue;
    }

    // Per-block fuel override (Issue #510 Phase 1): the synthetic
    // EmissionSource carries a per-STAGE rate, so it cannot capture
    // per-block fuel switching with full accuracy — blocks burning
    // gas vs diesel would share a single combustion factor.  Skip
    // auto-synthesis and require explicit per-block EmissionSource
    // rows for accuracy.  This mirrors the non-scalar heat_rate
    // policy below.
    if (gen.fuel_per_block.has_value()) {
      SPDLOG_WARN(
          "Generator '{}' uid={}: fuel_per_block is set — fuel-derived "
          "emission sources skipped.  Declare explicit per-block "
          "EmissionSource entries (one per fuel) to retain emission "
          "accuracy under fuel switching.",
          gen.name,
          gen.uid);
      continue;
    }

    const Fuel* fuel = find_fuel(fuel_array, *gen.fuel);
    if (fuel == nullptr || fuel->emission_factors.empty()) {
      continue;
    }

    // Scalar heat_rate only — time-varying schedules can't be folded
    // lossless into the per-stage `EmissionSource.rate` field.
    const auto hr_opt = scalar_or(gen.heat_rate);
    if (!hr_opt.has_value() || *hr_opt == 0.0) {
      if (gen.heat_rate.has_value()) {
        SPDLOG_WARN(
            "Generator '{}' uid={}: non-scalar heat_rate — fuel-derived "
            "emission sources skipped.  Declare explicit EmissionSource "
            "entries to retain per-block accuracy.",
            gen.name,
            gen.uid);
      }
      continue;
    }
    const double hr = *hr_opt;

    // Unit alignment between heat_rate and emission_factor.combustion:
    //
    //   * IPCC default factors are on an ENERGY basis: tCO2 / GJ.
    //   * PLEXOS (notably the CEN-Chile convention) ships heat_rate
    //     on a FUEL-MASS basis: tonnes_coal / MWh, m³_gas / MWh, etc.,
    //     with ``Fuel.heat_content`` carrying the energy density
    //     (GJ per fuel-unit).  Verified on CEN PCP 2026-04-12:
    //     SANTA_MARIA has heat_rate=0.346 t/MWh + heat_content=25.8
    //     GJ/t → real heat-rate ≈ 8.93 GJ/MWh (matches typical coal).
    //
    // The unit-correct product is therefore:
    //   tCO2/MWh = heat_rate [unit/MWh] × heat_content [GJ/unit]
    //              × combustion [tCO2/GJ]
    //
    // When ``Fuel.heat_content`` is 0 / unset (the gtopt default for
    // fuels converted from sources that already ship heat_rate in
    // GJ/MWh — sddp2gtopt, plp2gtopt synthetic Fuels, 118-Bus virtual
    // fuel) the legacy formula stays correct.  Use 1.0 as the no-op
    // multiplier so the byte-for-byte output is unchanged for those.
    // ``heat_content`` is OptTRealFieldSched (per-stage schedulable);
    // collapse to a scalar the same way the rest of this pass treats
    // heat_rate (line 558).  Time-varying heat_content is rare in
    // practice and silently uses the scalar fallback here — declare
    // explicit EmissionSource rows for per-stage accuracy.
    const double hc_raw = scalar_or(fuel->heat_content).value_or(0.0);
    const double hc = hc_raw > 0.0 ? hc_raw : 1.0;

    for (const auto& fef : fuel->emission_factors) {
      const double combustion = scalar_or(fef.combustion).value_or(0.0);
      const double upstream = scalar_or(fef.upstream).value_or(0.0);
      if (combustion == 0.0 && upstream == 0.0) {
        continue;
      }

      // One synthesized EmissionSource per (generator, covering zone).
      // Stacked-cap users (e.g. one global CO₂ zone plus a regional
      // basket) naturally pick up one source per zone they belong to.
      for (const auto& zone : emission_zone_array) {
        const bool covers =
            std::ranges::any_of(zone.emissions,
                                [&](const EmissionZoneFactor& zef)
                                { return zef.emission == fef.emission; });
        if (!covers) {
          continue;
        }

        auto sname = as_label(
            gen.name, emission_label(fef.emission), "via_fuel", zone.name);
        // Skip auto-generated duplicates — keeps the pass idempotent
        // under repeated invocation (e.g. merge → re-expand).
        if (src_names.contains(sname)) {
          continue;
        }

        while (src_uids.contains(src_uid)) {
          ++src_uid;
        }

        emission_source_array.push_back(EmissionSource {
            .uid = src_uid++,
            .name = sname,
            .generator = OptSingleId {SingleId {gen.uid}},
            .zone = SingleId {zone.uid},
            .emission = fef.emission,
            .rate = OptTRealFieldSched {hr * hc * combustion},
            .upstream_rate = upstream != 0.0
                ? OptTRealFieldSched {hr * hc * upstream}
                : OptTRealFieldSched {},
        });
        src_uids.insert(emission_source_array.back().uid);
        src_names.insert(sname);
      }
    }
  }
}

void System::fold_legacy_emission_rate()
{
  // Short-circuit: if no generator has a legacy emission_rate set,
  // there's nothing to fold.
  bool any_legacy = false;
  for (const auto& gen : generator_array) {
    if (gen.emission_rate.has_value()) {
      any_legacy = true;
      break;
    }
  }
  if (!any_legacy) {
    return;
  }

  // ── Ensure the synthetic CO₂ pollutant tag exists ──────────────────
  auto co2_it = std::ranges::find_if(
      emission_array, [](const Emission& e) { return e.name == "co2"; });
  if (co2_it == emission_array.end()) {
    auto next = next_uid(emission_array);
    emission_array.push_back(Emission {
        .uid = next,
        .name = "co2",
    });
    co2_it = std::prev(emission_array.end());
  }
  const auto co2_uid = co2_it->uid;

  // ── Ensure a CO₂-covering EmissionZone exists ──────────────────────
  auto zone_it = std::ranges::find_if(
      emission_zone_array,
      [co2_uid](const EmissionZone& z)
      {
        return std::ranges::any_of(
            z.emissions,
            [co2_uid](const EmissionZoneFactor& f)
            {
              return std::holds_alternative<Uid>(f.emission)
                  && std::get<Uid>(f.emission) == co2_uid;
            });
      });
  if (zone_it == emission_zone_array.end()) {
    auto next = next_uid(emission_zone_array);
    emission_zone_array.push_back(EmissionZone {
        .uid = next,
        .name = "default_co2",
        .emissions =
            {
                {
                    .emission = SingleId {co2_uid},
                    .weight = OptReal {1.0},
                },
            },
    });
    zone_it = std::prev(emission_zone_array.end());
  }
  const auto zone_uid = zone_it->uid;

  // ── Synthesize EmissionSource rows ─────────────────────────────────
  //
  // Important: we DO NOT reset `gen.emission_rate` after folding.  A
  // generator that also has `fuel` + `heat_rate_*` set carries TWO
  // physical emission components — direct process / non-combustion
  // (this field) and fuel combustion+upstream (synthesized by
  // `expand_fuel_emission_sources`).  Both must accumulate as
  // independent EmissionSource rows, not overwrite each other.
  // Idempotency is preserved by skipping when an EmissionSource with
  // the canonical legacy name already exists, so re-running the fold
  // (e.g. after a `merge`) does not duplicate rows.
  auto src_uid = next_uid(emission_source_array);
  auto src_uids = build_uid_set(emission_source_array);
  auto src_names = build_name_set(emission_source_array);

  for (auto& gen : generator_array) {
    if (!gen.emission_rate.has_value()) {
      continue;
    }
    const auto& legacy = *gen.emission_rate;

    // Downgrade `OptTBRealFieldSched` → `OptTRealFieldSched`.  Only
    // the scalar case is folded losslessly; vector and FileSched
    // require manual migration (we emit a one-shot warning and skip).
    OptTRealFieldSched rate;
    if (std::holds_alternative<Real>(legacy)) {
      rate = OptTRealFieldSched {std::get<Real>(legacy)};
    } else {
      SPDLOG_WARN(
          "Generator '{}' (uid={}): legacy emission_rate has "
          "non-scalar shape — skipped by auto-fold.  Replace with an "
          "explicit `emission_source_array` entry to keep the "
          "per-block / file-backed rate.",
          gen.name,
          gen.uid);
      continue;
    }

    auto sname = as_label(gen.name, "co2_legacy");
    // Idempotency: the legacy field is intentionally left populated,
    // so without a name-dedup the second call would re-add the row.
    if (src_names.contains(sname)) {
      continue;
    }

    while (src_uids.contains(src_uid)) {
      ++src_uid;
    }
    src_uids.insert(src_uid);
    src_names.insert(sname);

    emission_source_array.push_back(EmissionSource {
        .uid = src_uid++,
        .name = std::move(sname),
        .generator = OptSingleId {SingleId {gen.uid}},
        .zone = SingleId {zone_uid},
        .emission = SingleId {co2_uid},
        .rate = std::move(rate),
    });
  }
}

void System::fold_legacy_profiles()
{
  capacity_profile_array.reserve(capacity_profile_array.size()
                                 + generator_profile_array.size()
                                 + demand_profile_array.size());

  for (auto& gp : generator_profile_array) {
    capacity_profile_array.push_back(CapacityProfile {
        .uid = gp.uid,
        .name = std::move(gp.name),
        .active = std::move(gp.active),
        .owner_kind = ProfileOwnerKind::Generator,
        .owner = std::move(gp.generator),
        .profile = std::move(gp.profile),
        .scost = std::move(gp.scost),
    });
  }
  generator_profile_array.clear();

  for (auto& dp : demand_profile_array) {
    capacity_profile_array.push_back(CapacityProfile {
        .uid = dp.uid,
        .name = std::move(dp.name),
        .active = std::move(dp.active),
        .owner_kind = ProfileOwnerKind::Demand,
        .owner = std::move(dp.demand),
        .profile = std::move(dp.profile),
        .scost = std::move(dp.scost),
    });
  }
  demand_profile_array.clear();
}

// `sys` is consumed member-by-member via std::move on each field — the
// check only sees ``std::move(sys.foo)``, not ``std::move(sys)``.
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
void System::merge(System&& sys)
{
  if (!sys.name.empty()) {
    name = std::move(sys.name);
  }

  if (!sys.version.empty()) {
    version = std::move(sys.version);
  }

  gtopt::merge(bus_array, std::move(sys.bus_array));
  gtopt::merge(demand_array, std::move(sys.demand_array));
  gtopt::merge(generator_array, std::move(sys.generator_array));
  gtopt::merge(line_array, std::move(sys.line_array));

  gtopt::merge(generator_profile_array, std::move(sys.generator_profile_array));
  gtopt::merge(demand_profile_array, std::move(sys.demand_profile_array));
  gtopt::merge(capacity_profile_array, std::move(sys.capacity_profile_array));

  gtopt::merge(battery_array, std::move(sys.battery_array));
  gtopt::merge(converter_array, std::move(sys.converter_array));
  gtopt::merge(lng_terminal_array, std::move(sys.lng_terminal_array));

  gtopt::merge(reserve_zone_array, std::move(sys.reserve_zone_array));
  gtopt::merge(reserve_provision_array, std::move(sys.reserve_provision_array));
  gtopt::merge(fuel_array, std::move(sys.fuel_array));
  gtopt::merge(emission_array, std::move(sys.emission_array));
  gtopt::merge(emission_zone_array, std::move(sys.emission_zone_array));
  gtopt::merge(emission_source_array, std::move(sys.emission_source_array));
  gtopt::merge(commitment_array, std::move(sys.commitment_array));
  gtopt::merge(simple_commitment_array, std::move(sys.simple_commitment_array));
  gtopt::merge(line_commitment_array, std::move(sys.line_commitment_array));

  gtopt::merge(inertia_zone_array, std::move(sys.inertia_zone_array));
  gtopt::merge(inertia_provision_array, std::move(sys.inertia_provision_array));

  gtopt::merge(junction_array, std::move(sys.junction_array));
  gtopt::merge(waterway_array, std::move(sys.waterway_array));
  gtopt::merge(flow_array, std::move(sys.flow_array));
  gtopt::merge(reservoir_array, std::move(sys.reservoir_array));
  gtopt::merge(reservoir_seepage_array, std::move(sys.reservoir_seepage_array));
  gtopt::merge(reservoir_discharge_limit_array,
               std::move(sys.reservoir_discharge_limit_array));
  gtopt::merge(turbine_array, std::move(sys.turbine_array));
  gtopt::merge(pump_array, std::move(sys.pump_array));
  gtopt::merge(reservoir_production_factor_array,
               std::move(sys.reservoir_production_factor_array));

  gtopt::merge(flow_right_array, std::move(sys.flow_right_array));
  gtopt::merge(volume_right_array, std::move(sys.volume_right_array));

  gtopt::merge(user_param_array, std::move(sys.user_param_array));
  gtopt::merge(decision_variable_array, std::move(sys.decision_variable_array));
  gtopt::merge(plant_array, std::move(sys.plant_array));
  gtopt::merge(user_constraint_array, std::move(sys.user_constraint_array));
  gtopt::merge(user_model_array, std::move(sys.user_model_array));

  if (sys.user_constraint_file.has_value()) {
    user_constraint_file = std::move(sys.user_constraint_file);
  }

  if (!sys.user_constraint_files.empty()) {
    for (auto& f : sys.user_constraint_files) {
      user_constraint_files.push_back(std::move(f));
    }
  }
}

}  // namespace gtopt
