/**
 * @file      validate_planning.cpp
 * @brief     Semantic validation of a parsed Planning object
 * @date      Wed Mar 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements referential integrity, range, and completeness checks
 * that run after JSON parsing but before LP construction.
 */

#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <gtopt/enum_option.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/line_enums.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Helper: format a SingleId for error messages.
[[nodiscard]] auto format_single_id(const SingleId& sid) -> std::string
{
  return std::visit(
      [](const auto& val) -> std::string
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return std::format("uid={}", val);
        } else {
          return std::format("name=\"{}\"", val);
        }
      },
      sid);
}

/// O(1) membership over an element array's uids and names.  Built once per
/// array (see `IdIndexCache`) so referential-integrity validation runs in
/// O(elements) instead of O(elements × targets): on a large system
/// (case3375wp, 3375 buses) the old per-reference linear scan of `bus_array`
/// via `std::ranges::any_of` was ~9% of LP-build wall time, all on the
/// single-threaded load path.
struct ElementIdIndex
{
  std::unordered_set<Uid> uids;
  // Views into the array elements' `name` strings; the array outlives the
  // index (both are call-scoped in `check_referential_integrity`).
  std::unordered_set<std::string_view> names;

  template<typename Elem>
  explicit ElementIdIndex(const Array<Elem>& arr)
  {
    uids.reserve(arr.size());
    names.reserve(arr.size());
    for (const auto& e : arr) {
      uids.insert(e.uid);
      names.insert(std::string_view {e.name});
    }
  }

  /// True iff `sid` (a uid- or name-form reference) matches an element.
  [[nodiscard]] auto contains(const SingleId& sid) const -> bool
  {
    return std::visit(
        [this](const auto& val) -> bool
        {
          if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
            return uids.contains(val);
          } else {
            return names.contains(std::string_view {val});
          }
        },
        sid);
  }
};

/// Lazily builds and memoises one `ElementIdIndex` per target array, keyed by
/// the array's address.  Scoped to a single `check_referential_integrity`
/// call, so the addresses are stable and never reused across `System`s.
class IdIndexCache
{
public:
  template<typename Elem>
  [[nodiscard]] auto index_for(const Array<Elem>& arr) -> const ElementIdIndex&
  {
    const auto* key = static_cast<const void*>(&arr);
    if (const auto it = m_indices_.find(key); it != m_indices_.end()) {
      return it->second;
    }
    return m_indices_.emplace(key, ElementIdIndex {arr}).first->second;
  }

private:
  std::unordered_map<const void*, ElementIdIndex> m_indices_;
};

/// Check whether a SingleId names a synthetic ``<battery>_gen`` discharge
/// generator that ``System::expand_batteries`` will materialise at LP-build
/// time (it is absent from ``generator_array`` during pre-build validation).
/// Only name-form ids can match; uid-form ids never name a synthetic gen.
[[nodiscard]] inline auto synthetic_battery_gen_exists(
    const SingleId& sid, const Array<Battery>& batteries) -> bool
{
  return std::visit(
      [&batteries](const auto& val) -> bool
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return false;
        } else {
          constexpr std::string_view suffix {"_gen"};
          const std::string_view name {val};
          if (!name.ends_with(suffix)) {
            return false;
          }
          const auto base = name.substr(0, name.size() - suffix.size());
          return std::ranges::any_of(
              batteries, [&base](const auto& b) { return b.name == base; });
        }
      },
      sid);
}

/// Validate referential integrity of all components.
void check_referential_integrity(ValidationResult& result, const System& sys)
{
  // One O(1) membership index per target array, built on first use, so the
  // checks below run in O(elements) instead of O(elements × targets).  The
  // generic lambda keeps `check_ref` call sites identical to the former free
  // function that linearly scanned `arr` per reference.
  IdIndexCache cache;
  const auto check_ref = [&cache](ValidationResult& res,
                                  const SingleId& sid,
                                  const auto& arr,
                                  std::string_view owner_kind,
                                  std::string_view owner_name,
                                  std::string_view field_name,
                                  std::string_view target_kind)
  {
    if (!cache.index_for(arr).contains(sid)) {
      res.errors.push_back(
          std::format("{} '{}': {} references non-existent {} ({})",
                      owner_kind,
                      owner_name,
                      field_name,
                      target_kind,
                      format_single_id(sid)));
    }
  };

  // Generator.bus -> Bus
  for (const auto& gen : sys.generator_array) {
    check_ref(
        result, gen.bus, sys.bus_array, "Generator", gen.name, "bus", "Bus");
  }

  // Demand.bus -> Bus
  for (const auto& dem : sys.demand_array) {
    check_ref(result, dem.bus, sys.bus_array, "Demand", dem.name, "bus", "Bus");
  }

  // Line.bus_a, Line.bus_b -> Bus
  for (const auto& line : sys.line_array) {
    check_ref(
        result, line.bus_a, sys.bus_array, "Line", line.name, "bus_a", "Bus");
    check_ref(
        result, line.bus_b, sys.bus_array, "Line", line.name, "bus_b", "Bus");
  }

  // Turbine.waterway -> Waterway, Turbine.flow -> Flow (alternative),
  // Turbine.generator -> Generator (all paths require generator).
  //
  // Critical invariant: a turbine MUST carry an electrical generator
  // and MUST connect to either a waterway or a flow.  Without the
  // generator there is no MW output to dispatch; without a waterway
  // or flow there is no water volume to convert.  Either omission
  // produces a silently-broken LP — `TurbineLP::add_to_lp` logs
  // a `WARN` and returns false, leaving the model with a registered
  // turbine that contributes no constraints or columns.  Promote
  // both omissions to errors at the validation gate so the user
  // sees the problem before the solver burns CPU on a degenerate
  // LP.
  for (const auto& turb : sys.turbine_array) {
    if (turb.waterway.has_value()) {
      check_ref(result,
                turb.waterway.value(),
                sys.waterway_array,
                "Turbine",
                turb.name,
                "waterway",
                "Waterway");
    }
    if (turb.flow.has_value()) {
      check_ref(result,
                turb.flow.value(),
                sys.flow_array,
                "Turbine",
                turb.name,
                "flow",
                "Flow");
    }
    // Built-in waterway mode: the turbine carries its own flow arc
    // between junction_a and (optionally) junction_b.  junction_a is the
    // intake; junction_b, when present, is the downstream junction —
    // unset means the turbined flow drains out of the system.
    if (turb.junction_a.has_value()) {
      check_ref(result,
                turb.junction_a.value(),
                sys.junction_array,
                "Turbine",
                turb.name,
                "junction_a",
                "Junction");
    }
    if (turb.junction_b.has_value()) {
      check_ref(result,
                turb.junction_b.value(),
                sys.junction_array,
                "Turbine",
                turb.name,
                "junction_b",
                "Junction");
    }
    if (!turb.waterway.has_value() && !turb.flow.has_value()
        && !turb.junction_a.has_value())
    {
      result.errors.push_back(std::format(
          "Turbine '{}' has none of waterway, flow or junction_a set "
          "(at least one is required to drive the water-to-power conversion)",
          turb.name));
    }
    check_ref(result,
              turb.generator,
              sys.generator_array,
              "Turbine",
              turb.name,
              "generator",
              "Generator");
  }

  // Pump.waterway -> Waterway, Pump.demand -> Demand
  for (const auto& pump : sys.pump_array) {
    check_ref(result,
              pump.waterway,
              sys.waterway_array,
              "Pump",
              pump.name,
              "waterway",
              "Waterway");
    check_ref(result,
              pump.demand,
              sys.demand_array,
              "Pump",
              pump.name,
              "demand",
              "Demand");
  }

  // Flow.junction -> Junction (optional in flow-turbine mode)
  for (const auto& flow : sys.flow_array) {
    if (flow.junction.has_value()) {
      check_ref(result,
                flow.junction.value(),
                sys.junction_array,
                "Flow",
                flow.name,
                "junction",
                "Junction");
    }
  }

  // Waterway.junction_a -> Junction (required), .junction_b -> Junction
  // (OPTIONAL: unset means outflow / drain mode — the flow leaves the
  // system at junction_a, no downstream credit).
  for (const auto& ww : sys.waterway_array) {
    check_ref(result,
              ww.junction_a,
              sys.junction_array,
              "Waterway",
              ww.name,
              "junction_a",
              "Junction");
    if (ww.junction_b.has_value()) {
      check_ref(result,
                ww.junction_b.value(),
                sys.junction_array,
                "Waterway",
                ww.name,
                "junction_b",
                "Junction");
    }
  }

  // Converter.battery -> Battery, generator -> Generator, demand -> Demand
  for (const auto& conv : sys.converter_array) {
    check_ref(result,
              conv.battery,
              sys.battery_array,
              "Converter",
              conv.name,
              "battery",
              "Battery");
    check_ref(result,
              conv.generator,
              sys.generator_array,
              "Converter",
              conv.name,
              "generator",
              "Generator");
    check_ref(result,
              conv.demand,
              sys.demand_array,
              "Converter",
              conv.name,
              "demand",
              "Demand");
  }

  // Reservoir.junction -> Junction
  for (const auto& res : sys.reservoir_array) {
    check_ref(result,
              res.junction,
              sys.junction_array,
              "Reservoir",
              res.name,
              "junction",
              "Junction");
  }

  // Generator.fuel -> Fuel (optional FK)
  for (const auto& gen : sys.generator_array) {
    if (gen.fuel.has_value()) {
      check_ref(result,
                gen.fuel.value(),
                sys.fuel_array,
                "Generator",
                gen.name,
                "fuel",
                "Fuel");
    }
  }

  // Commitment.fuel was removed on 2026-05-20.  Fuel FK validation
  // for the dispatch-cost path now happens on Generator (see the
  // Generator.fuel branch above).

  // ── P0 referential checks added 2026-05-20 ────────────────────────
  //
  // All of the following elements have foreign-key fields that used to
  // be silently accepted at validation time and then either crashed at
  // LP-build (when the lookup throws) or — worse — emitted a
  // `SPDLOG_WARN` + `return false` from the *_lp.cpp ``add_to_lp``,
  // leaving a registered element that contributes no constraints or
  // columns to the LP.  Promote every such case to a hard validation
  // error so the user sees the broken model before the solver runs.
  // See the matching audit notes in `validate_planning - <element>` test
  // SUBCASEs.

  // EmissionSource: generator (optional), zone (required),
  // emission (required).  `emission_source_lp.cpp:79` used to
  // silently skip a source without a generator.
  for (const auto& src : sys.emission_source_array) {
    if (src.generator.has_value()) {
      check_ref(result,
                src.generator.value(),
                sys.generator_array,
                "EmissionSource",
                src.name,
                "generator",
                "Generator");
    }
    check_ref(result,
              src.zone,
              sys.emission_zone_array,
              "EmissionSource",
              src.name,
              "zone",
              "EmissionZone");
    check_ref(result,
              src.emission,
              sys.emission_array,
              "EmissionSource",
              src.name,
              "emission",
              "Emission");
  }

  // GeneratorProfile.generator -> Generator (required).
  for (const auto& gp : sys.generator_profile_array) {
    check_ref(result,
              gp.generator,
              sys.generator_array,
              "GeneratorProfile",
              gp.name,
              "generator",
              "Generator");
  }

  // DemandProfile.demand -> Demand (required).
  for (const auto& dp : sys.demand_profile_array) {
    check_ref(result,
              dp.demand,
              sys.demand_array,
              "DemandProfile",
              dp.name,
              "demand",
              "Demand");
  }

  // ReserveProvision.generator -> Generator (required).
  // `reserve_zones` is an array of `ReserveZone` ids/names — each
  // entry must resolve.
  for (const auto& rp : sys.reserve_provision_array) {
    // Accept both real generators and the synthetic ``<battery>_gen``
    // discharge generators created by ``expand_batteries`` (BESS reserve
    // provision from ``SSCC_Activation_BESS.csv`` targets these).
    if (!cache.index_for(sys.generator_array).contains(rp.generator)
        && !synthetic_battery_gen_exists(rp.generator, sys.battery_array))
    {
      result.errors.push_back(
          std::format("ReserveProvision '{}': generator references "
                      "non-existent Generator ({})",
                      rp.name,
                      format_single_id(rp.generator)));
    }
    for (const auto& rz_id : rp.reserve_zones) {
      check_ref(result,
                rz_id,
                sys.reserve_zone_array,
                "ReserveProvision",
                rp.name,
                "reserve_zones",
                "ReserveZone");
    }
  }

  // InertiaProvision.generator -> Generator (required).
  // `inertia_zones` is the analogous array reference.
  for (const auto& ip : sys.inertia_provision_array) {
    check_ref(result,
              ip.generator,
              sys.generator_array,
              "InertiaProvision",
              ip.name,
              "generator",
              "Generator");
    for (const auto& iz_id : ip.inertia_zones) {
      check_ref(result,
                iz_id,
                sys.inertia_zone_array,
                "InertiaProvision",
                ip.name,
                "inertia_zones",
                "InertiaZone");
    }
  }

  // SimpleCommitment.generator -> Generator (required).
  for (const auto& sc : sys.simple_commitment_array) {
    check_ref(result,
              sc.generator,
              sys.generator_array,
              "SimpleCommitment",
              sc.name,
              "generator",
              "Generator");
  }

  // LineCommitment.line -> Line (required FK), plus per-row sanity
  // checks introduced by issue #509 §"Validation".  The presence of a
  // LineCommitment row makes the referenced Line a switching candidate,
  // so an unresolved FK silently leaves the LP without OTS on that
  // line — exactly the asymmetry we want to surface as a hard error.
  for (const auto& lc : sys.line_commitment_array) {
    check_ref(result,
              lc.line,
              sys.line_array,
              "LineCommitment",
              lc.name,
              "line",
              "Line");

    if (lc.kvl_big_m.has_value() && lc.kvl_big_m.value() <= 0.0) {
      result.errors.push_back(std::format(
          "LineCommitment '{}': kvl_big_m must be > 0 when set (got {}); "
          "a non-positive big-M makes the KVL disjunction unbounded "
          "and the LP-relaxation trivially feasible (issue #509)",
          lc.name,
          lc.kvl_big_m.value()));
    }

    if (lc.initial_status.has_value()) {
      const auto v = lc.initial_status.value();
      if (v != 0.0 && v != 1.0) {
        result.errors.push_back(std::format(
            "LineCommitment '{}': initial_status must be 0 or 1 (got {})",
            lc.name,
            v));
      }
    }
  }

  // ReservoirProductionFactor.turbine -> Turbine,
  //                         .reservoir -> Reservoir (both required).
  // The production-factor row drives the water-to-MW LP coefficient;
  // either invalid FK silently breaks SDDP dispatch.
  for (const auto& rpf : sys.reservoir_production_factor_array) {
    check_ref(result,
              rpf.turbine,
              sys.turbine_array,
              "ReservoirProductionFactor",
              rpf.name,
              "turbine",
              "Turbine");
    check_ref(result,
              rpf.reservoir,
              sys.reservoir_array,
              "ReservoirProductionFactor",
              rpf.name,
              "reservoir",
              "Reservoir");
  }

  // ReservoirSeepage.waterway -> Waterway, .reservoir -> Reservoir
  // (both required).  Seepage flow + reservoir-balance row depend on
  // both FKs resolving.
  for (const auto& seep : sys.reservoir_seepage_array) {
    check_ref(result,
              seep.waterway,
              sys.waterway_array,
              "ReservoirSeepage",
              seep.name,
              "waterway",
              "Waterway");
    check_ref(result,
              seep.reservoir,
              sys.reservoir_array,
              "ReservoirSeepage",
              seep.name,
              "reservoir",
              "Reservoir");
  }

  // ── P1 referential checks added 2026-05-20 ────────────────────────
  //
  // Optional FKs that should still be VALIDATED when set.  Today an
  // invalid `bus` or `reservoir` uid on these structs just silently
  // produces a wrong dispatch because the lookup gates the
  // corresponding LP branch on `has_value()` but never on lookup
  // success.

  // Battery.bus -> Bus (optional FK).  When set, `System::expand_batteries()`
  // auto-generates the discharge Generator + charge Demand + Converter
  // wired to this bus.  Invalid uid here leaves the expansion silently
  // wired to a non-existent bus.
  // Battery.source_generator -> Generator (optional FK).  When set,
  // `expand_batteries()` creates an internal bus and re-routes the
  // source generator to it.  Invalid uid → wrong internal-bus wiring.
  for (const auto& bat : sys.battery_array) {
    if (bat.bus.has_value()) {
      check_ref(result,
                bat.bus.value(),
                sys.bus_array,
                "Battery",
                bat.name,
                "bus",
                "Bus");
    }
    if (bat.source_generator.has_value()) {
      check_ref(result,
                bat.source_generator.value(),
                sys.generator_array,
                "Battery",
                bat.name,
                "source_generator",
                "Generator");
    }
  }

  // VolumeRight.reservoir -> Reservoir (optional FK, consumptive source).
  // VolumeRight.right_reservoir -> VolumeRight (optional FK, hierarchical
  // parent / child volume balance).  Each branch in `volume_right_lp.cpp`
  // gates on `has_value()` but never on lookup success; invalid uid =
  // silent contribution to wrong reservoir or wrong parent right.
  for (const auto& vr : sys.volume_right_array) {
    if (vr.reservoir.has_value()) {
      check_ref(result,
                vr.reservoir.value(),
                sys.reservoir_array,
                "VolumeRight",
                vr.name,
                "reservoir",
                "Reservoir");
    }
    if (vr.right_reservoir.has_value()) {
      check_ref(result,
                vr.right_reservoir.value(),
                sys.volume_right_array,
                "VolumeRight",
                vr.name,
                "right_reservoir",
                "VolumeRight");
    }
  }

  // ReservoirDischargeLimit: the flow source is exactly one of ``waterway``
  // (classic Waterway flow column) or ``turbine`` (built-in waterway turbine
  // owning its own flow column).  The discharge-limit piecewise row binds
  // that flow to the reservoir's volume state, so neither set is meaningless
  // and both set is ambiguous — flag both omissions.
  for (const auto& rdl : sys.reservoir_discharge_limit_array) {
    if (rdl.waterway.has_value()) {
      check_ref(result,
                rdl.waterway.value(),
                sys.waterway_array,
                "ReservoirDischargeLimit",
                rdl.name,
                "waterway",
                "Waterway");
    }
    if (rdl.turbine.has_value()) {
      check_ref(result,
                rdl.turbine.value(),
                sys.turbine_array,
                "ReservoirDischargeLimit",
                rdl.name,
                "turbine",
                "Turbine");
    }
    if (!rdl.waterway.has_value() && !rdl.turbine.has_value()) {
      result.errors.push_back(std::format(
          "ReservoirDischargeLimit '{}' has neither a waterway nor a turbine "
          "reference set (exactly one is required to bind the flow column)",
          rdl.name));
    } else if (rdl.waterway.has_value() && rdl.turbine.has_value()) {
      result.errors.push_back(std::format(
          "ReservoirDischargeLimit '{}' has both waterway and turbine set — "
          "exactly one is required",
          rdl.name));
    }
    check_ref(result,
              rdl.reservoir,
              sys.reservoir_array,
              "ReservoirDischargeLimit",
              rdl.name,
              "reservoir",
              "Reservoir");
  }
}

/// Validate fuel/heat-rate schema rules: mutual exclusion (scalar vs
/// piecewise), shape consistency (pmax_segments and heat_rate_segments
/// must have equal length), and strict-increasing convexity on the
/// piecewise slopes (so the LP picks the cheapest segment first by
/// construction — see `GeneratorAttrs::heat_rate_segments` docstring).
void check_heat_rate(ValidationResult& result, const System& sys)
{
  for (const auto& gen : sys.generator_array) {
    const bool has_scalar = gen.heat_rate.has_value();
    const bool has_pieces =
        !gen.heat_rate_segments.empty() || !gen.pmax_segments.empty();

    if (has_scalar && has_pieces) {
      result.errors.push_back(std::format(
          "Generator '{}': `heat_rate` (scalar) and `heat_rate_segments` / "
          "`pmax_segments` (piecewise) are mutually exclusive — set exactly "
          "one.",
          gen.name));
    }

    if (has_pieces) {
      if (gen.pmax_segments.size() != gen.heat_rate_segments.size()) {
        result.errors.push_back(
            std::format("Generator '{}': `pmax_segments` (size {}) and "
                        "`heat_rate_segments` "
                        "(size {}) must have equal length.",
                        gen.name,
                        gen.pmax_segments.size(),
                        gen.heat_rate_segments.size()));
      }

      // Strictly increasing pmax breakpoints — required for the
      // piecewise [P̄_{k-1}, P̄ₖ] decomposition to be well-defined.
      for (std::size_t k = 1; k < gen.pmax_segments.size(); ++k) {
        if (!(gen.pmax_segments[k] > gen.pmax_segments[k - 1])) {
          result.errors.push_back(std::format(
              "Generator '{}': pmax_segments must be strictly increasing — "
              "pmax_segments[{}] = {} is not > pmax_segments[{}] = {}.",
              gen.name,
              k,
              gen.pmax_segments[k],
              k - 1,
              gen.pmax_segments[k - 1]));
          break;
        }
      }

      // Strictly increasing heat-rate slopes — convexity precondition.
      for (std::size_t k = 1; k < gen.heat_rate_segments.size(); ++k) {
        if (!(gen.heat_rate_segments[k] > gen.heat_rate_segments[k - 1])) {
          result.errors.push_back(std::format(
              "Generator '{}': heat_rate_segments must be strictly increasing "
              "for the piecewise cost to be convex — heat_rate_segments[{}] = "
              "{} is not > heat_rate_segments[{}] = {}.",
              gen.name,
              k,
              gen.heat_rate_segments[k],
              k - 1,
              gen.heat_rate_segments[k - 1]));
          break;
        }
      }
    }
  }

  // Commitment.pmax_segments / heat_rate_segments were removed on
  // 2026-05-20.  The piecewise-curve validation above (on Generator)
  // is the only path now.

  // ── P2 fuel/heat-rate pairing (added 2026-05-20) ────────────────────
  //
  // The LP-side coefficient is
  //   slope_cost_per_mwh = stage_fuel_price * heat_rate + block_gcost
  // (see `source/generator_lp.cpp:138-153`).  When either factor is
  // missing the code silently falls back to `block_gcost`, which is
  // almost never the user's intent:
  //
  //   * fuel set, no heat_rate (and no heat_rate_segments)
  //       → fuel_price coefficient drops to 0 → fuel cost ignored
  //   * heat_rate (or heat_rate_segments) set, no fuel
  //       → no per-fuel pricing applied → heat_rate ignored
  //
  // Both cases are flagged as warnings (not errors).  The LP still
  // solves, but the resulting dispatch ignores half of the user's
  // declared cost structure — better to surface the disagreement at
  // the validation gate than to track it down post-solve.
  for (const auto& gen : sys.generator_array) {
    const bool has_fuel = gen.fuel.has_value();
    const bool has_heat_rate =
        gen.heat_rate.has_value() || !gen.heat_rate_segments.empty();
    if (has_fuel && !has_heat_rate) {
      result.warnings.push_back(std::format(
          "Generator '{}': fuel='{}' set but no heat_rate or "
          "heat_rate_segments — fuel price will be ignored at the LP "
          "(per-MWh cost falls back to gcost only).  Set heat_rate or "
          "heat_rate_segments to consume the fuel price.",
          gen.name,
          format_single_id(gen.fuel.value())));
    } else if (has_heat_rate && !has_fuel) {
      result.warnings.push_back(std::format(
          "Generator '{}': heat_rate set but no fuel — heat_rate will "
          "be ignored at the LP (per-MWh cost falls back to gcost only). "
          "Set a fuel reference to consume the heat_rate.",
          gen.name));
    }
  }

  // Commitment.fuel / pmax_segments / heat_rate_segments were removed
  // on 2026-05-20.  The dispatch-cost fuel + heat-rate pairing now
  // lives entirely on Generator, validated above.
}

// ── Positivity helpers ────────────────────────────────────────────────
//
// Water-right, line, and waterway magnitudes must be physically
// positive (zero allowed when explicitly called out; negative is
// always a schema bug).  These helpers walk the FieldSched variant
// (scalar / vector / file) and report any negative scalar value
// found.  FileSched entries are skipped — they resolve at
// load-time and the file loader is responsible for its own checks.

enum class Positivity : std::uint8_t
{
  strict,  ///< value must be > 0
  non_negative,  ///< value must be >= 0
};

[[nodiscard]] constexpr bool violates(double v, Positivity p) noexcept
{
  return p == Positivity::strict ? !(v > 0.0) : !(v >= 0.0);
}

[[nodiscard]] constexpr std::string_view positivity_name(Positivity p) noexcept
{
  return p == Positivity::strict ? "> 0" : ">= 0";
}

template<typename Vec>
void check_vector_positive(ValidationResult& result,
                           const Vec& values,
                           Positivity p,
                           std::string_view owner_kind,
                           std::string_view owner_name,
                           std::string_view field_name)
{
  if constexpr (std::is_arithmetic_v<
                    std::remove_cvref_t<decltype(values.front())>>)
  {
    for (const auto& [i, v_raw] : enumerate(values)) {
      const auto v = static_cast<double>(v_raw);
      if (violates(v, p)) {
        result.errors.push_back(std::format("{} '{}': {}[{}] = {} must be {}",
                                            owner_kind,
                                            owner_name,
                                            field_name,
                                            i,
                                            v,
                                            positivity_name(p)));
        return;  // one error per field is enough
      }
    }
  } else {
    // Nested vector (TBReal / STBReal).  Recurse one level.
    for (const auto& sub : values) {
      check_vector_positive(result, sub, p, owner_kind, owner_name, field_name);
    }
  }
}

/// Check a FieldSched<Real, ...> variant for positivity.
/// FileSched (string) entries are skipped — those resolve at load-time.
template<typename FieldSched>
void check_field_positive(ValidationResult& result,
                          const FieldSched& fs,
                          Positivity p,
                          std::string_view owner_kind,
                          std::string_view owner_name,
                          std::string_view field_name)
{
  std::visit(
      [&](const auto& val)
      {
        using T = std::remove_cvref_t<decltype(val)>;
        if constexpr (std::is_same_v<T, Real>) {
          if (violates(static_cast<double>(val), p)) {
            result.errors.push_back(std::format("{} '{}': {} = {} must be {}",
                                                owner_kind,
                                                owner_name,
                                                field_name,
                                                val,
                                                positivity_name(p)));
          }
        } else if constexpr (std::is_same_v<T, FileSched>) {
          // Filename reference — defer validation to load-time.
        } else {
          check_vector_positive(
              result, val, p, owner_kind, owner_name, field_name);
        }
      },
      fs);
}

/// Check an optional FieldSched field.  Skips when unset.
template<typename OptFieldSched>
void check_opt_field_positive(ValidationResult& result,
                              const OptFieldSched& opt,
                              Positivity p,
                              std::string_view owner_kind,
                              std::string_view owner_name,
                              std::string_view field_name)
{
  if (!opt.has_value()) {
    return;
  }
  check_field_positive(result, *opt, p, owner_kind, owner_name, field_name);
}

/// Check positivity of flow-right / volume-right / line / waterway fields.
void check_positivity(ValidationResult& result, const System& sys)
{
  // FlowRight: fmin, target, fmax must be >= 0 (consumptive flow).
  for (const auto& fr : sys.flow_right_array) {
    check_opt_field_positive(result,
                             fr.fmin,
                             Positivity::non_negative,
                             "FlowRight",
                             fr.name,
                             "fmin");
    check_opt_field_positive(result,
                             fr.target,
                             Positivity::non_negative,
                             "FlowRight",
                             fr.name,
                             "target");
    check_opt_field_positive(result,
                             fr.fmax,
                             Positivity::non_negative,
                             "FlowRight",
                             fr.name,
                             "fmax");
    // Non-consumptive rights return the served flow to `junction_b`;
    // without it there is nowhere to credit the flow, so the right would
    // silently behave as consumptive.  Require the pairing explicitly.
    if (fr.consumptive.has_value() && !*fr.consumptive
        && !fr.junction_b.has_value())
    {
      result.errors.push_back(
          std::format("FlowRight '{}': consumptive=false requires 'junction_b' "
                      "to return the served flow to the river",
                      fr.name));
    }
  }

  // VolumeRight: storage and demand magnitudes must be >= 0.
  for (const auto& vr : sys.volume_right_array) {
    check_opt_field_positive(result,
                             vr.emin,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emin");
    check_opt_field_positive(result,
                             vr.emax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emax");
    check_opt_field_positive(result,
                             vr.demand,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "demand");
    check_opt_field_positive(result,
                             vr.fmax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "fmax");
    if (vr.eini.has_value() && *vr.eini < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': eini = {} must be >= 0", vr.name, *vr.eini));
    }
    if (vr.efin.has_value() && *vr.efin < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': efin = {} must be >= 0", vr.name, *vr.efin));
    }
  }

  // Line: transfer capacities must be >= 0 (direction is encoded
  // separately in bus_a/bus_b).
  for (const auto& line : sys.line_array) {
    check_opt_field_positive(result,
                             line.tmax_ab,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ab");
    check_opt_field_positive(result,
                             line.tmax_ba,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ba");
  }

  // Waterway (hydro flow): fmin and fmax must be >= 0.  Negative flow
  // would invert the direction, which should be expressed by swapping
  // junction_a/junction_b instead.  fmax == 0 is legal: it means the
  // flow is pinned at 0 (matches PLP's VertMax=0 → qv_k ∈ [0,0] on
  // the vertimiento waterway — see plpcnfce.dat + leecnfce.f:342-343).
  for (const auto& ww : sys.waterway_array) {
    check_opt_field_positive(
        result, ww.fmin, Positivity::non_negative, "Waterway", ww.name, "fmin");
    check_opt_field_positive(
        result, ww.fmax, Positivity::non_negative, "Waterway", ww.name, "fmax");
  }
}

// ── Piecewise-linear feasibility (Reservoir seepage / discharge_limit) ──
//
// Both `ReservoirSeepage` and `ReservoirDischargeLimit` produce a
// stage-level LP constraint of the form
//
//     row(efin) − slope · efin  ≤  intercept
//
// where `(slope, intercept)` is selected by piecewise-linear segment
// against the start-of-stage volume.  At the LOWEST volume boundary
// (`efin = emin`) the constraint reduces to
//
//     row(efin)  ≤  intercept + slope · emin
//
// For `row(efin) ≥ 0` to be a feasible LP, the right-hand side must
// be non-negative — which means the FIRST segment (lowest volume
// breakpoint) must satisfy  `intercept + slope · emin ≥ 0`.  When
// the input data violates this invariant the LP solves cleanly only
// while the reservoir stays above the first segment's lower bound;
// the moment a backward cut pulls efin to emin (which happens
// regularly under SDDP iter-1+), the resolve goes primal-infeasible
// and the trajectory diverges between off (which retains stale
// state on the live backend) and compress (which reverts on
// reconstruct).  Catching the invariant at validation time produces
// a clear "fix the input data" message instead of opaque solver
// fallbacks downstream.
//
// Implementation: walks `system.reservoir_seepage_array` and
// `system.reservoir_discharge_limit_array`, looks up the owning
// reservoir's `emin` (when scalar — vector/file schedules are
// resolved at load time and skipped here), then evaluates the
// first segment's `intercept + slope · emin` and emits a warning
// when the result is negative.

namespace
{
// Try to extract a scalar value from an `OptTRealFieldSched` /
// `OptTBRealFieldSched`-shaped field.  Returns nullopt for
// vector / file schedules — those are validated per-stage at load
// time and can't be sanity-checked here.
template<typename OptField>
[[nodiscard]] std::optional<Real> try_scalar_value(const OptField& field)
{
  if (!field.has_value()) {
    return std::nullopt;
  }
  return std::visit(
      [](const auto& v) -> std::optional<Real>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return v;
        } else {
          return std::nullopt;
        }
      },
      *field);
}

// Look up a reservoir struct by SingleId (Uid or Name).
[[nodiscard]] const Reservoir* find_reservoir(
    const std::vector<Reservoir>& reservoirs, const SingleId& sid)
{
  return std::visit(
      [&](const auto& v) -> const Reservoir*
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Uid>) {
          for (const auto& r : reservoirs) {
            if (r.uid == v) {
              return &r;
            }
          }
        } else if constexpr (std::is_same_v<T, Name>) {
          for (const auto& r : reservoirs) {
            if (r.name == v) {
              return &r;
            }
          }
        }
        return nullptr;
      },
      sid);
}

// Look up a waterway struct by SingleId (Uid or Name).
[[nodiscard]] const Waterway* find_waterway(
    const std::vector<Waterway>& waterways, const SingleId& sid)
{
  return std::visit(
      [&](const auto& v) -> const Waterway*
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Uid>) {
          for (const auto& w : waterways) {
            if (w.uid == v) {
              return &w;
            }
          }
        } else if constexpr (std::is_same_v<T, Name>) {
          for (const auto& w : waterways) {
            if (w.name == v) {
              return &w;
            }
          }
        }
        return nullptr;
      },
      sid);
}

// Compute the active volume range [V_low, V_high] for segment k of a
// segments vector sorted ascending by `.volume`, clipped to the
// reservoir's [emin, emax] envelope.  N is the segment count.
struct SegmentRange
{
  Real v_low {0.0};
  Real v_high {0.0};
};

template<typename SegmentT>
[[nodiscard]] SegmentRange segment_range(const std::vector<SegmentT>& segments,
                                         std::size_t k,
                                         Real emin,
                                         Real emax)
{
  const auto seg_lo = static_cast<Real>(segments[k].volume);
  const auto v_low = std::max(seg_lo, emin);
  const auto v_high = (k + 1 < segments.size())
      ? static_cast<Real>(segments[k + 1].volume)
      : emax;
  return {.v_low = v_low, .v_high = v_high};
}

// Linear function value at endpoints (no interior extrema for linear).
struct LinearRange
{
  Real f_low {0.0};
  Real f_high {0.0};
  [[nodiscard]] Real fmin() const { return std::min(f_low, f_high); }
  [[nodiscard]] Real fmax() const { return std::max(f_low, f_high); }
};

[[nodiscard]] LinearRange evaluate_linear(Real slope,
                                          Real constant,
                                          const SegmentRange& r)
{
  return {
      .f_low = constant + (slope * r.v_low),
      .f_high = constant + (slope * r.v_high),
  };
}

/// Extract one Real per stage from a 1D `OptTRealFieldSched`-shaped
/// field.  Scalar broadcasts to every stage; vector indexes by stage
/// (truncated/padded to `num_stages` with `default_value`); file-
/// schedule returns `nullopt` (truly deferred to load-time).
template<typename OptField>
[[nodiscard]] std::optional<std::vector<Real>> per_stage_values(
    const OptField& field, std::size_t num_stages, Real default_value)
{
  if (!field.has_value()) {
    return std::vector<Real>(num_stages, default_value);
  }
  return std::visit(
      [&](const auto& v) -> std::optional<std::vector<Real>>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return std::vector<Real>(num_stages, v);
        } else if constexpr (std::is_same_v<T, std::vector<Real>>) {
          std::vector<Real> out(num_stages, default_value);
          for (std::size_t s = 0; s < num_stages && s < v.size(); ++s) {
            out[s] = v[s];
          }
          return out;
        } else {
          // FileSched (string) — truly deferred.
          return std::nullopt;
        }
      },
      *field);
}

/// 2D variant for `OptTBRealFieldSched` (per-stage, per-block).
/// Reduces each stage's block vector to a single scalar via
/// `reducer` (use `std::ranges::min` for upper-bound fields where the
/// strictest stage-level value is the smallest, `std::ranges::max`
/// for lower-bound fields where the strictest is the largest).
template<typename OptField, typename Reducer>
[[nodiscard]] std::optional<std::vector<Real>> per_stage_values_2d(
    const OptField& field,
    std::size_t num_stages,
    Real default_value,
    Reducer reducer)
{
  if (!field.has_value()) {
    return std::vector<Real>(num_stages, default_value);
  }
  return std::visit(
      [&](const auto& v) -> std::optional<std::vector<Real>>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return std::vector<Real>(num_stages, v);
        } else if constexpr (std::is_same_v<T,
                                            std::vector<std::vector<Real>>>) {
          std::vector<Real> out(num_stages, default_value);
          for (std::size_t s = 0; s < num_stages && s < v.size(); ++s) {
            if (!v[s].empty()) {
              out[s] = reducer(v[s]);
            }
          }
          return out;
        } else {
          return std::nullopt;
        }
      },
      *field);
}

/// Per-stage scan summary for one piecewise segment in one direction
/// (below the lower bound or above the upper bound).  Aggregated
/// across all stages so the validator emits exactly ONE warning per
/// (element, segment, direction) instead of one per stage.
struct StageViolation
{
  std::size_t count {0};  ///< Number of stages where the bound is violated.
  Real worst_diff {0.0};  ///< Largest |bound − f| across the failing stages.
  std::size_t worst_stage {0};  ///< Stage index achieving `worst_diff`.
  LinearRange worst_fr {};  ///< Linear range at the worst stage.
  SegmentRange worst_range {};  ///< Active [V_low, V_high] at the worst stage.
  Real worst_bound {0.0};  ///< Bound value at the worst stage.
};

/// Walk every stage and accumulate the worst lower-bound violation
/// (`fr.fmin() < lower_bounds[s]`) and worst upper-bound violation
/// (`fr.fmax() > upper_bounds[s]`) for the given segment.  Returns a
/// pair `{below, above}`; either may have `count == 0` for "no
/// violation in that direction".
template<typename Segment>
[[nodiscard]] std::pair<StageViolation, StageViolation> scan_segment_per_stage(
    const std::vector<Segment>& segments,
    std::size_t k,
    Real slope,
    Real constant,
    std::span<const Real> emins,
    std::span<const Real> emaxs,
    std::span<const Real> lower_bounds,
    std::span<const Real> upper_bounds)
{
  StageViolation below;
  StageViolation above;
  const std::size_t num_stages = emins.size();
  for (std::size_t s = 0; s < num_stages; ++s) {
    const auto range = segment_range(segments, k, emins[s], emaxs[s]);
    if (range.v_high < range.v_low) {
      continue;  // segment outside this stage's [emin, emax] envelope
    }
    const auto fr = evaluate_linear(slope, constant, range);

    if (fr.fmin() < lower_bounds[s]) {
      ++below.count;
      const Real diff = lower_bounds[s] - fr.fmin();
      if (diff > below.worst_diff) {
        below.worst_diff = diff;
        below.worst_stage = s;
        below.worst_fr = fr;
        below.worst_range = range;
        below.worst_bound = lower_bounds[s];
      }
    }
    if (fr.fmax() > upper_bounds[s]) {
      ++above.count;
      const Real diff = fr.fmax() - upper_bounds[s];
      if (diff > above.worst_diff) {
        above.worst_diff = diff;
        above.worst_stage = s;
        above.worst_fr = fr;
        above.worst_range = range;
        above.worst_bound = upper_bounds[s];
      }
    }
  }
  return {below, above};
}

/// Per-stage seepage envelopes.  Returns `nullopt` for any field that
/// is file-schedule (truly deferred to load-time validation).
struct SeepageEnvelopes
{
  std::vector<Real> emins;  ///< Reservoir per-stage emin.
  std::vector<Real> emaxs;  ///< Reservoir per-stage emax.
  std::vector<Real>
      fmins;  ///< Strictest per-stage waterway floor (max-reduced).
  std::vector<Real>
      fmaxs;  ///< Strictest per-stage waterway ceiling (min-reduced).
};

[[nodiscard]] std::optional<SeepageEnvelopes> resolve_seepage_envelopes(
    const Reservoir& res, const Waterway& ww, std::size_t num_stages)
{
  using std::ranges::max;
  using std::ranges::min;

  // Reservoir.emin / emax are per-(stage, block) — reduce each stage's
  // block vector to the strictest stage-level value: max(emin) (highest
  // required floor), min(emax) (lowest allowed ceiling).
  auto emins =
      per_stage_values_2d(res.emin,
                          num_stages,
                          0.0,
                          [](const std::vector<Real>& v) { return max(v); });
  auto emaxs =
      per_stage_values_2d(res.emax,
                          num_stages,
                          std::numeric_limits<Real>::infinity(),
                          [](const std::vector<Real>& v) { return min(v); });
  // Reduce per-stage block vectors to the strictest stage-level
  // value: max(fmin) (highest required floor), min(fmax) (lowest
  // allowed ceiling).
  auto fmins =
      per_stage_values_2d(ww.fmin,
                          num_stages,
                          0.0,
                          [](const std::vector<Real>& v) { return max(v); });
  auto fmaxs =
      per_stage_values_2d(ww.fmax,
                          num_stages,
                          std::numeric_limits<Real>::infinity(),
                          [](const std::vector<Real>& v) { return min(v); });
  if (!emins.has_value() || !emaxs.has_value() || !fmins.has_value()
      || !fmaxs.has_value())
  {
    return std::nullopt;
  }
  return SeepageEnvelopes {
      .emins = std::move(*emins),
      .emaxs = std::move(*emaxs),
      .fmins = std::move(*fmins),
      .fmaxs = std::move(*fmaxs),
  };
}

/// Per-stage discharge-limit envelopes (only the reservoir
/// [emin, emax] matter; the LP constrains the discharge col bound to
/// `intercept + slope·V ≥ 0`).
struct DischargeLimitEnvelopes
{
  std::vector<Real> emins;
  std::vector<Real> emaxs;
};

[[nodiscard]] std::optional<DischargeLimitEnvelopes>
resolve_discharge_limit_envelopes(const Reservoir& res, std::size_t num_stages)
{
  // Reservoir.emin / emax are per-(stage, block) — reduce each stage's
  // block vector to the strictest stage-level value: max(emin) (highest
  // required floor), min(emax) (lowest allowed ceiling).
  using std::ranges::max;
  using std::ranges::min;
  auto emins =
      per_stage_values_2d(res.emin,
                          num_stages,
                          0.0,
                          [](const std::vector<Real>& v) { return max(v); });
  auto emaxs =
      per_stage_values_2d(res.emax,
                          num_stages,
                          std::numeric_limits<Real>::infinity(),
                          [](const std::vector<Real>& v) { return min(v); });
  if (!emins.has_value() || !emaxs.has_value()) {
    return std::nullopt;
  }
  return DischargeLimitEnvelopes {
      .emins = std::move(*emins),
      .emaxs = std::move(*emaxs),
  };
}

/// Format the standard "below fmin" warning for a seepage segment.
[[nodiscard]] std::string format_seepage_below_warning(
    const ReservoirSeepage& seep,
    const Reservoir& res,
    const Waterway& ww,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirSeepage '{}' (reservoir '{}', waterway '{}'): "
      "segment {} produces qfilt below waterway fmin in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → qfilt "
      "range [{:.6g}, {:.6g}] vs fmin={:.6g}, slope={:.6g}, "
      "constant={:.6g}); the LP will go primal-infeasible whenever "
      "efin lands in the segment's lower portion.  Adjust the "
      "segment data so that `constant + slope * V ≥ fmin` for all "
      "V in [V_low, V_high].",
      seep.name,
      res.name,
      ww.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      v.worst_bound,
      seep.segments[k].slope,
      seep.segments[k].constant);
}

/// Format the standard "above fmax" warning for a seepage segment.
[[nodiscard]] std::string format_seepage_above_warning(
    const ReservoirSeepage& seep,
    const Reservoir& res,
    const Waterway& ww,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirSeepage '{}' (reservoir '{}', waterway '{}'): "
      "segment {} produces qfilt above waterway fmax in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → qfilt "
      "range [{:.6g}, {:.6g}] vs fmax={:.6g}, slope={:.6g}, "
      "constant={:.6g}); the LP will go primal-infeasible whenever "
      "efin lands in the segment's upper portion.",
      seep.name,
      res.name,
      ww.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      v.worst_bound,
      seep.segments[k].slope,
      seep.segments[k].constant);
}

/// Format the standard "negative discharge bound" warning for a
/// discharge-limit segment.
[[nodiscard]] std::string format_discharge_limit_warning(
    const ReservoirDischargeLimit& ddl,
    const Reservoir& res,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirDischargeLimit '{}' (reservoir '{}'): segment {} "
      "produces a negative discharge upper bound in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → bound "
      "range [{:.6g}, {:.6g}], slope={:.6g}, intercept={:.6g}); "
      "the LP will go primal-infeasible whenever efin lands in "
      "the segment's lower portion.  Adjust the segment so that "
      "`intercept + slope * V >= 0` for all V in [V_low, V_high].",
      ddl.name,
      res.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      ddl.segments[k].slope,
      ddl.segments[k].intercept);
}

}  // namespace

void check_piecewise_feasibility(ValidationResult& result,
                                 const Planning& planning)
{
  const auto& sys = planning.system;
  const auto num_stages = planning.simulation.stage_array.size();
  // **ReservoirSeepage** — per-segment range feasibility.
  //
  // The LP constraint per (block, stage) is the equality
  //   `qfilt − slope_k · efin = constant_k`
  // → `qfilt = constant_k + slope_k · efin`.
  // The flow `qfilt` lives on the seepage's waterway and is bounded
  // `qfilt ∈ [fmin, fmax]`.  For each segment k, while it's the
  // active piecewise piece (i.e. `efin ∈ [V_low_k, V_high_k]`, where
  // V_low_0 is clamped to the reservoir's emin and V_high_{N-1} is
  // clamped to emax), we need:
  //
  //   fmin  ≤  constant_k + slope_k · efin  ≤  fmax,  ∀ efin ∈ [V_low_k,
  //   V_high_k]
  //
  // For schedule-form `emin/emax/fmin/fmax` (vector-per-stage), the
  // check fires per-stage and is summarised: ONE warning per
  // (element, segment, direction) listing how many stages fail and
  // pointing at the worst offender.  File-schedules (string paths)
  // are still deferred — we don't load them here.
  for (const auto& seep : sys.reservoir_seepage_array) {
    if (seep.segments.empty()) {
      continue;
    }
    const auto* res = find_reservoir(sys.reservoir_array, seep.reservoir);
    if (res == nullptr) {
      continue;  // referential integrity check elsewhere catches missing refs
    }
    const auto* ww = find_waterway(sys.waterway_array, seep.waterway);
    if (ww == nullptr) {
      continue;
    }
    const auto envs = resolve_seepage_envelopes(*res, *ww, num_stages);
    if (!envs.has_value()) {
      continue;  // some field is file-schedule — truly deferred
    }

    for (std::size_t k = 0; k < seep.segments.size(); ++k) {
      const auto& seg = seep.segments[k];
      const auto [below, above] = scan_segment_per_stage(seep.segments,
                                                         k,
                                                         seg.slope,
                                                         seg.constant,
                                                         envs->emins,
                                                         envs->emaxs,
                                                         envs->fmins,
                                                         envs->fmaxs);
      if (below.count > 0) {
        result.warnings.push_back(format_seepage_below_warning(
            seep, *res, *ww, k, num_stages, below));
      }
      if (above.count > 0) {
        result.warnings.push_back(format_seepage_above_warning(
            seep, *res, *ww, k, num_stages, above));
      }
    }
  }

  // **ReservoirDischargeLimit** — per-segment range feasibility.
  //
  // The LP row is `qeh − slope_k · efin ≤ intercept_k`, where qeh has
  // lower bound 0 (default for a free non-negative col).  For each
  // segment k active at efin ∈ [V_low_k, V_high_k] at stage s:
  //
  //   0 ≤ intercept_k + slope_k · efin   for all efin in the range.
  //
  // Per-stage scan with the same dedup semantics as the seepage
  // check above: ONE warning per (DDL, segment) summarising the
  // count of failing stages plus the worst case.  Only the lower
  // bound (≥ 0) matters; upper-bound is +∞ so the "above" branch of
  // `scan_segment_per_stage` never fires.
  const std::vector<Real> ddl_lower(num_stages, 0.0);
  const std::vector<Real> ddl_upper(num_stages,
                                    std::numeric_limits<Real>::infinity());
  for (const auto& ddl : sys.reservoir_discharge_limit_array) {
    if (ddl.segments.empty()) {
      continue;
    }
    const auto* res = find_reservoir(sys.reservoir_array, ddl.reservoir);
    if (res == nullptr) {
      continue;
    }
    const auto envs = resolve_discharge_limit_envelopes(*res, num_stages);
    if (!envs.has_value()) {
      continue;
    }

    for (std::size_t k = 0; k < ddl.segments.size(); ++k) {
      const auto& seg = ddl.segments[k];
      const auto [below, _above] = scan_segment_per_stage(ddl.segments,
                                                          k,
                                                          seg.slope,
                                                          seg.intercept,
                                                          envs->emins,
                                                          envs->emaxs,
                                                          ddl_lower,
                                                          ddl_upper);
      if (below.count > 0) {
        result.warnings.push_back(
            format_discharge_limit_warning(ddl, *res, k, num_stages, below));
      }
    }
  }
}

/// Validate range constraints on simulation parameters.
void check_ranges(ValidationResult& result, const Planning& planning)
{
  const auto& sim = planning.simulation;

  // Block duration > 0
  for (const auto& blk : sim.block_array) {
    if (blk.duration <= 0.0) {
      result.errors.push_back(std::format(
          "Block uid={}: duration ({}) must be > 0", blk.uid, blk.duration));
    }
  }

  // Stage count_block > 0
  for (const auto& stg : sim.stage_array) {
    if (stg.count_block == 0) {
      result.errors.push_back(
          std::format("Stage uid={}: count_block must be > 0", stg.uid));
    }
  }

  // Generator capacity non-negative (warning only)
  for (const auto& gen : planning.system.generator_array) {
    if (gen.capacity.has_value()) {
      const auto& cap = gen.capacity.value();
      if (std::holds_alternative<Real>(cap) && std::get<Real>(cap) < 0.0) {
        result.warnings.push_back(
            std::format("Generator '{}': capacity ({}) is negative",
                        gen.name,
                        std::get<Real>(cap)));
      }
    }
  }
}

/// Validate that every UID listed in `phase.apertures` resolves to an
/// entry in `simulation.aperture_array`.
///
/// Pre-fix history: silent fallback at runtime — phases with
/// dangling aperture UIDs would emit
/// `SDDP Aperture [...]: source_scenario X not found and no aperture
/// cache, skipping` for each broken reference, then proceed with the
/// remaining apertures.  Easy to misread as expected behaviour
/// (see the original juan/gtopt_iplp configuration where 2 of 16
/// per-phase aperture UIDs referenced uids 1 and 2 but the global
/// `aperture_array` defined uids 51..66 only).  Surfacing this as a
/// validation error catches the input-data bug at parse time, before
/// the SDDP run silently quietly drops cuts.
void check_aperture_references(ValidationResult& result,
                               const Planning& planning)
{
  const auto& sim = planning.simulation;

  // Build a set of aperture UIDs declared in the global array.
  // Empty aperture_array is fine — it disables apertures globally.
  if (sim.aperture_array.empty()) {
    return;
  }

  std::vector<int> known_uids;
  known_uids.reserve(sim.aperture_array.size());
  for (const auto& ap : sim.aperture_array) {
    known_uids.push_back(ap.uid);
  }
  std::ranges::sort(known_uids);

  for (const auto& ph : sim.phase_array) {
    if (ph.apertures.empty()) {
      continue;
    }
    for (const auto& uid : ph.apertures) {
      const auto u = uid;
      if (!std::ranges::binary_search(known_uids, u)) {
        result.errors.push_back(std::format(
            "Phase uid={}: aperture uid={} listed in `apertures` does not "
            "exist in `simulation.aperture_array` (declared uids range from "
            "{} to {}).  Either add the aperture definition or remove the "
            "uid from this phase's list.",
            ph.uid,
            u,
            known_uids.front(),
            known_uids.back()));
      }
    }
  }
}

/// Validate structural completeness.
void check_completeness(ValidationResult& result, const Planning& planning)
{
  if (planning.system.bus_array.empty()) {
    result.errors.emplace_back("System has no buses defined");
  }

  if (planning.simulation.block_array.empty()) {
    result.errors.emplace_back("Simulation has no blocks defined");
  }

  if (planning.simulation.stage_array.empty()) {
    result.errors.emplace_back("Simulation has no stages defined");
  }
}

/// Tolerance for probability sum comparison.
constexpr double prob_tolerance = 1e-6;

/// Check and optionally rescale scenario probabilities per scene.
void check_scenario_probabilities(ValidationResult& result, Planning& planning)
{
  const auto mode = planning.simulation.probability_rescale.value_or(
      ProbabilityRescaleMode::runtime);
  const bool do_rescale = (mode != ProbabilityRescaleMode::none);

  auto& scenarios = planning.simulation.scenario_array;
  const auto& scenes = planning.simulation.scene_array;

  if (scenarios.empty()) {
    return;
  }

  // When there are no explicit scenes, all scenarios form one implicit scene.
  // Check that the total probability sums to 1.0.
  if (scenes.empty()) {
    double total = 0.0;
    for (const auto& sc : scenarios) {
      total += sc.probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scenario probability_factor values sum to {:.6f} (expected 1.0)",
          total));

      if (do_rescale && total > 0.0) {
        for (auto& sc : scenarios) {
          const double p = sc.probability_factor.value_or(1.0);
          sc.probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled {} scenario probability_factor values to sum 1.0",
            scenarios.size()));
      }
    }
    return;
  }

  // Per-scene: check that scenario probabilities within each scene sum to 1.0.
  // Skip scenes with a single scenario — they are trivially normalised and
  // their probability_factor represents the global weight used for
  // cross-scene aggregation (e.g. SDDP with one scenario per scene).
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;

    if (first + count > scenarios.size()) {
      result.errors.push_back(std::format(
          "Scene '{}': first_scenario ({}) + count_scenario ({}) exceeds "
          "scenario_array size ({})",
          scene.name.value_or("?"),
          first,
          count,
          scenarios.size()));
      continue;
    }

    // Single-scenario scenes need no intra-scene normalisation.
    if (count <= 1) {
      continue;
    }

    double total = 0.0;
    for (std::size_t i = first; i < first + count; ++i) {
      total += scenarios[i].probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scene '{}': scenario probability_factor values sum to {:.6f} "
          "(expected 1.0)",
          scene.name.value_or("?"),
          total));

      if (do_rescale && total > 0.0) {
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled scenario probabilities in scene '{}' to sum 1.0",
            scene.name.value_or("?")));
      }
    }
  }

  // Check that scene-level probability totals sum to 1.0 across all scenes.
  double scene_total = 0.0;
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;
    if (first + count > scenarios.size()) {
      continue;  // already reported above
    }
    for (std::size_t i = first; i < first + count; ++i) {
      scene_total += scenarios[i].probability_factor.value_or(1.0);
    }
  }

  if (std::abs(scene_total - 1.0) > prob_tolerance) {
    result.warnings.push_back(
        std::format("Total scene probability sums to {:.6f} across {} scenes "
                    "(expected 1.0)",
                    scene_total,
                    scenes.size()));

    if (do_rescale && scene_total > 0.0) {
      for (const auto& scene : scenes) {
        const auto first = scene.first_scenario;
        const auto count = (scene.count_scenario == std::dynamic_extent)
            ? (scenarios.size() - first)
            : scene.count_scenario;
        if (first + count > scenarios.size()) {
          continue;
        }
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / scene_total;
        }
      }
      result.warnings.push_back(
          std::format("Rescaled all scenario probabilities across {} scenes to "
                      "sum 1.0",
                      scenes.size()));
    }
  }
}

}  // namespace

[[nodiscard]] ValidationResult validate_planning(Planning& planning)
{
  ValidationResult result;

  check_referential_integrity(result, planning.system);
  check_ranges(result, planning);
  check_positivity(result, planning.system);
  check_heat_rate(result, planning.system);
  check_piecewise_feasibility(result, planning);
  check_aperture_references(result, planning);
  check_completeness(result, planning);
  check_scenario_probabilities(result, planning);

  // Gates for Optimal Transmission Switching (issue #509).  Count the
  // active LineCommitment rows once; both the method gate (rejects
  // SDDP / cascade) and the Kirchhoff-mode gate (rejects cycle_basis)
  // use it.
  int active_lc_count = 0;
  for (const auto& lc : planning.system.line_commitment_array) {
    // Treat unset / non-zero active as "active".  The OptActive
    // variant carries either a bool, a per-stage vector, or a name
    // — for the gate purpose we only need to detect rows explicitly
    // marked inactive at file scope.
    const bool is_inactive = lc.active.has_value()
        && std::holds_alternative<Int>(*lc.active)
        && std::get<Int>(*lc.active) == 0;
    if (!is_inactive) {
      ++active_lc_count;
    }
  }

  // Method gate for OTS.  OTS introduces per-line binary u_l decisions
  // that make Benders cuts unsound on SDDP / cascade subproblems
  // (cost-to-go nonconvexity — see issue body §"Why monolithic only,
  // not SDDP" + Zou-Ahmed-Sun 2019).  Reject upfront so the
  // misconfiguration surfaces at JSON load time rather than failing
  // mid-SDDP with an opaque error.  A future SDDiP-style relaxation
  // (Lagrangian cuts) would lift this restriction.
  const auto method = planning.options.method.value_or(MethodType::monolithic);
  if (method != MethodType::monolithic && active_lc_count > 0) {
    const std::string_view method_name = (method == MethodType::sddp)
        ? std::string_view {"sddp"}
        : std::string_view {"cascade"};
    result.errors.push_back(std::format(
        "Optimal Transmission Switching (OTS) is incompatible with the "
        "'{}' planning method: {} active LineCommitment row(s) found, "
        "but Benders cuts on a mixed-integer subproblem are unsound "
        "(Zou-Ahmed-Sun 2019).  Switch to method='monolithic' or "
        "deactivate the LineCommitment rows (issue #509).",
        method_name,
        active_lc_count));
  }

  // Issue #509 v0.6 ``cycle_basis`` gate REMOVED in v1: the cycle-form
  // big-M disjunctive rewrite is now implemented in
  // ``source/kirchhoff_cycle_basis.cpp::add_kvl_rows`` (per-cycle
  // ``M_C = 2θ_max · |C| · row_scale + Σ |φ_e| · row_scale``).  Both
  // Kirchhoff modes (``node_angle`` and ``cycle_basis``) now support
  // ``LineCommitment``; transport mode (``use_kirchhoff = false``)
  // continues to work under capacity gating alone.

  // Log all findings
  for (const auto& warn : result.warnings) {
    spdlog::warn("Validation: {}", warn);
  }
  for (const auto& err : result.errors) {
    spdlog::error("Validation: {}", err);
  }

  if (result.ok()) {
    spdlog::info("Planning validation passed");
  } else {
    spdlog::error("Planning validation failed with {} error(s)",
                  result.errors.size());
  }

  return result;
}

}  // namespace gtopt
