/**
 * @file      element_column_resolver.cpp
 * @brief     Resolve LP column indices for user-constraint element references
 * @date      Mon Mar 24 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/constraint_expr.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/names_registry.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <gtopt/waterway_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ── Element-ID parsing ───────────────────────────────────────────────────────

/**
 * @brief Convert a constraint-expression element-id string to a `SingleId`.
 *
 * Three accepted forms:
 *  - `"uid:N"` — UID N (as written by the constraint parser for bare integers)
 *  - `"N"`     — pure decimal integer → UID N
 *  - anything else → Name lookup
 */
[[nodiscard]] SingleId parse_element_id(const std::string& element_id)
{
  // "uid:N" form produced by the constraint parser for bare-integer references
  if (element_id.starts_with("uid:")) {
    const std::string_view digits = std::string_view {element_id}.substr(4);
    Uid val {};
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), val);
    const bool ok = ec == std::errc {} && ptr == digits.data() + digits.size();
    if (ok) {
      return val;
    }
    // fall through to name if parse fails
    return Name {element_id};
  }

  // Bare integer string (purely decimal, no letters)
  if (!element_id.empty()
      && std::ranges::all_of(
          element_id, [](unsigned char c) { return std::isdigit(c) != 0; }))
  {
    Uid val {};
    const auto [ptr, ec] = std::from_chars(
        element_id.data(), element_id.data() + element_id.size(), val);
    const bool ok =
        ec == std::errc {} && ptr == element_id.data() + element_id.size();
    if (ok) {
      return val;
    }
  }

  // Otherwise treat as a name
  return Name {element_id};
}

}  // anonymous namespace

// ── Per-element column resolution ────────────────────────────────────────────

[[nodiscard]] std::optional<ResolvedCol> resolve_single_col(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref,
    const LinearProblem& lp)
{
  // Singleton-class scalars (`options.*`, `system.*`) carry no element
  // id and resolve to a constant numeric, never to an LP column — leave
  // them to `resolve_single_param`'s scalar branch.
  if (ref.element_id.empty()) {
    return std::nullopt;
  }

  const auto single_id = parse_element_id(ref.element_id);
  const BlockUid buid = block.uid();

  // 1. Convert element_id (Uid or Name) into a concrete Uid.  Names
  //    are looked up via the AMPL element-name registry populated by
  //    each element's `add_to_lp` on first invocation.
  const auto uid_opt = [&]() -> std::optional<Uid>
  {
    if (std::holds_alternative<Uid>(single_id)) {
      return std::get<Uid>(single_id);
    }
    if (std::holds_alternative<Name>(single_id)) {
      return sc.lookup_ampl_element_uid(ref.element_type,
                                        std::get<Name>(single_id));
    }
    return std::nullopt;
  }();

  if (!uid_opt) {
    SPDLOG_WARN("user_constraint: unknown {} name '{}'",
                ref.element_type,
                ref.element_id);
    return std::nullopt;
  }

  // 2. Generic AMPL variable registry lookup — primary path.  All
  //    migrated elements populate this map in their `add_to_lp`, so
  //    we can look up the column without per-element-type dispatch.
  if (const auto col = sc.find_ampl_col(ref.element_type,
                                        *uid_opt,
                                        ref.attribute,
                                        scenario.uid(),
                                        stage.uid(),
                                        buid))
  {
    // Optional additive offset: physical = LP * scale + offset.
    // Returns 0.0 for every element that didn't register offsets —
    // the common case.  Demand's Option C registration is the only
    // current user; see `demand_lp.cpp`.
    return ResolvedCol {
        .col = *col,
        .scale = lp.get_col_scale(*col),
        .offset = sc.find_ampl_offset(ref.element_type,
                                      *uid_opt,
                                      ref.attribute,
                                      scenario.uid(),
                                      stage.uid(),
                                      buid),
    };
  }

  // 3. Fallback: `bus.theta` columns are created lazily by
  //    `LineLP::add_kirchhoff_rows` through `theta_cols_at` and are
  //    therefore not yet known to the registry at `BusLP::add_to_lp`
  //    time.  Keep the bespoke lookup so PAMPL expressions can still
  //    reference theta once the lines have populated the map.
  if (ref.element_type == "bus" && ref.attribute == BusLP::ThetaName) {
    try {
      const auto& bus_lp = sc.get_element(ObjectSingleId<BusLP> {single_id});
      if (auto col = bus_lp.lookup_theta_col(scenario, stage, buid)) {
        return ResolvedCol {
            .col = *col,
            .scale = lp.get_col_scale(*col),
        };
      }
    } catch (const std::exception& ex) {
      SPDLOG_WARN("user_constraint: cannot resolve {}.{}('{}'): {}",
                  ref.element_type,
                  ref.attribute,
                  ref.element_id,
                  ex.what());
    }
    return std::nullopt;
  }

  return std::nullopt;
}

// ── Compound-aware row emission ──────────────────────────────────────────────

/// Resolve and stamp a single (class, uid, attribute) reference into
/// `row` with coefficient `coef`.  Handles both single-col registrations
/// (the common case) and **multi-col sum** registrations used by virtual
/// aggregators like `line.flowp` under `piecewise_direct` line-loss mode
/// (no aggregator LP col — `flowp` resolves to `Σ flowp_seg_k`, so we
/// stamp `coef` on every segment col).  See
/// `AmplVariable::block_cols_sum` for the sum-of-cols data layout.
///
/// Returns `{emitted, offset_shift}`.  `offset_shift` is `coef × offset`
/// when the resolved column carries a non-zero AMPL offset (e.g.
/// demand's Option C `neg_fail = load − lmax`); the caller folds the
/// shift onto the row's RHS via the existing `param_shift` accumulator.
[[nodiscard]] ResolveColResult stamp_ref(const SystemContext& sc,
                                         const ScenarioLP& scenario,
                                         const StageLP& stage,
                                         const BlockLP& block,
                                         const ElementRef& ref,
                                         double coef,
                                         SparseRow& row,
                                         const LinearProblem& lp)
{
  // Resolve element_id (uid|name) once; multi-col path needs the uid.
  const auto single_id = parse_element_id(ref.element_id);
  const auto uid_opt = [&]() -> std::optional<Uid>
  {
    if (std::holds_alternative<Uid>(single_id)) {
      return std::get<Uid>(single_id);
    }
    if (std::holds_alternative<Name>(single_id)) {
      return sc.lookup_ampl_element_uid(ref.element_type,
                                        std::get<Name>(single_id));
    }
    return std::nullopt;
  }();

  if (uid_opt) {
    // Try multi-col (sum-of-cols) first.  When registered, the
    // aggregator is virtual and the attribute expands to a sum of LP
    // cols — stamp each with the leg coefficient.  Sum-of-cols
    // registrations never carry offsets in current code; if they ever
    // do, they'd be a per-leg shift summed here.
    const auto cols = sc.find_ampl_cols(ref.element_type,
                                        *uid_opt,
                                        ref.attribute,
                                        scenario.uid(),
                                        stage.uid(),
                                        block.uid());
    if (!cols.empty()) {
      for (const auto& col : cols) {
        row[col] += coef;
      }
      return {.emitted = true, .offset_shift = 0.0};
    }
  }

  // Fall back to the single-col path (uses the lp scale + optional
  // offset).  When the column has a non-zero offset, the caller folds
  // `coef × offset` onto the row's RHS (existing param_shift machinery).
  if (auto resolved = resolve_single_col(sc, scenario, stage, block, ref, lp)) {
    row[resolved->col] += coef;
    return {
        .emitted = true,
        .offset_shift = coef * resolved->offset,
    };
  }
  return {.emitted = false, .offset_shift = 0.0};
}

ResolveColResult resolve_col_to_row(const SystemContext& sc,
                                    const ScenarioLP& scenario,
                                    const StageLP& stage,
                                    const BlockLP& block,
                                    const ElementRef& ref,
                                    double base_coeff,
                                    SparseRow& row,
                                    const LinearProblem& lp)
{
  // 1. Compound path: class-level recipe of (coefficient, source_attribute).
  if (const auto* legs = sc.find_ampl_compound(ref.element_type, ref.attribute))
  {
    ResolveColResult out;
    for (const auto& leg : *legs) {
      ElementRef leg_ref = ref;
      leg_ref.attribute = std::string {leg.source_attribute};
      const auto leg_res = stamp_ref(sc,
                                     scenario,
                                     stage,
                                     block,
                                     leg_ref,
                                     base_coeff * leg.coefficient,
                                     row,
                                     lp);
      if (leg_res.emitted) {
        out.emitted = true;
        out.offset_shift += leg_res.offset_shift;
      }
    }
    return out;
  }

  // 2. Single attribute path (handles both single-col and multi-col).
  return stamp_ref(sc, scenario, stage, block, ref, base_coeff, row, lp);
}

// ── Per-element parameter resolution ────────────────────────────────────────

[[nodiscard]] std::optional<double> resolve_single_param(
    const SystemContext& sc,
    [[maybe_unused]] const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref)
{
  // ── singleton class scalar (options.*, system.*, stage.*) ───────────
  // No element id, no element-level variation.  Most of these are
  // immutable for the SimulationLP lifetime and live in the scalar
  // registry, but `stage.*` reads metadata of the *active* stage and
  // therefore has to be resolved against the StageLP argument.
  if (ref.element_id.empty()) {
    if (ref.element_type == StageLP::ClassName) {
      if (ref.attribute == StageLP::MonthName) {
        const auto m = stage.month();
        if (m.has_value()) {
          return static_cast<double>(std::to_underlying(*m));
        }
        return std::nullopt;
      }
      if (ref.attribute == StageLP::UidName) {
        return static_cast<double>(stage.uid());
      }
      if (ref.attribute == StageLP::DurationName) {
        return stage.duration();
      }
      SPDLOG_WARN("user_constraint: unknown stage attribute '{}'",
                  ref.attribute);
      return std::nullopt;
    }
    if (auto val = sc.find_ampl_scalar(ref.element_type, ref.attribute)) {
      return val;
    }
    SPDLOG_WARN("user_constraint: unknown scalar {}.{}",
                ref.element_type,
                ref.attribute);
    return std::nullopt;
  }

  const auto single_id = parse_element_id(ref.element_id);
  const auto suid = stage.uid();
  const auto buid = block.uid();

  // Normalise the attribute name via the runtime naming-dialects
  // registry.  Class-scoped lookup first (entries under
  // `class_aliases[]` — e.g. `flow_right.discharge → target`,
  // `flow_right.use_value → uvalue` — both legacy gtopt aliases
  // that collide with canonicals on other classes); falls back to
  // the global table for class-blind aliases like `marginal_cost
  // → gcost`, `tmax → tmax_ab`, etc.  See
  // docs/analysis/naming-conventions.md §10.4.
  auto attr = std::string_view {ref.attribute};
  if (const auto canonical =
          NamesRegistry::instance().canonical_for(ref.element_type, attr);
      canonical.has_value())
  {
    attr = *canonical;
  }

  try {
    // ── generator ────────────────────────────────────────────────────────
    if (ref.element_type == "generator") {
      const auto& gen = sc.get_element(ObjectSingleId<GeneratorLP> {single_id});
      if (attr == "pmax") {
        return gen.param_pmax(suid, buid);
      }
      if (attr == "pmin") {
        return gen.param_pmin(suid, buid);
      }
      if (attr == "gcost") {
        return gen.param_gcost(suid, buid);
      }
      if (attr == "lossfactor") {
        return gen.param_lossfactor(suid, buid);
      }
      // Added 2026-05-17 alongside the Fuel entity (d13da9e8):
      //   * heat_rate       — per-(stage, block) <fuel_unit>/MWh
      //   * emission_rate — per-(stage, block) tCO₂/MWh
      // Per-segment heat rates (`heat_rate_segments`, `pmax_segments`)
      // are arrays without a meaningful scalar PAMPL projection — not
      // exposed; reference via the Fuel side instead.
      if (attr == "heat_rate") {
        return gen.param_heat_rate(suid, buid);
      }
      if (attr == "emission_rate") {
        return gen.param_emission_rate(suid, buid);
      }
      return std::nullopt;
    }

    // ── fuel (passive parameter carrier, no LP cols) ────────────────────
    // PLEXOS-named schedules referenced by Generator.fuel /
    // Commitment.fuel.  Resolves only through `resolve_single_param`
    // (no LP column path) since `FuelLP::add_to_lp` is a no-op.
    if (ref.element_type == "fuel") {
      const auto& fuel = sc.get_element(ObjectSingleId<FuelLP> {single_id});
      if (attr == "price") {
        return fuel.param_price(suid);
      }
      if (attr == "heat_content") {
        return fuel.param_heat_content(suid);
      }
      if (attr == "combustion_emission_factor") {
        return fuel.param_combustion_emission_factor(suid);
      }
      if (attr == "upstream_emission_factor") {
        return fuel.param_upstream_emission_factor(suid);
      }
      return std::nullopt;
    }

    // ── demand ───────────────────────────────────────────────────────────
    if (ref.element_type == "demand") {
      const auto& dem = sc.get_element(ObjectSingleId<DemandLP> {single_id});
      if (attr == "lmax") {
        return dem.param_lmax(suid, buid);
      }
      if (attr == "fcost") {
        return dem.param_fcost(suid, buid);
      }
      if (attr == "lossfactor") {
        return dem.param_lossfactor(suid, buid);
      }
      return std::nullopt;
    }

    // ── line ─────────────────────────────────────────────────────────────
    if (ref.element_type == "line") {
      const auto& ln = sc.get_element(ObjectSingleId<LineLP> {single_id});
      // `tmax` (legacy) is now an alias for `tmax_ab` via the registry —
      // canonicalisation above turns `tmax` into `tmax_ab`, so a single
      // compare suffices here.
      if (attr == "tmax_ab") {
        return ln.param_tmax_ab(suid, buid);
      }
      if (attr == "tmax_ba") {
        return ln.param_tmax_ba(suid, buid);
      }
      if (attr == "tcost") {
        return ln.param_tcost(suid, buid);
      }
      if (attr == "reactance") {
        return ln.param_reactance(suid);
      }
      return std::nullopt;
    }

    // ── battery ──────────────────────────────────────────────────────────
    if (ref.element_type == "battery") {
      const auto& bat = sc.get_element(ObjectSingleId<BatteryLP> {single_id});
      if (attr == "emin") {
        return bat.param_emin(suid, buid);
      }
      if (attr == "emax") {
        return bat.param_emax(suid, buid);
      }
      if (attr == "ecost") {
        return bat.param_ecost(suid, buid);
      }
      if (attr == "input_efficiency") {
        return bat.param_input_efficiency(suid, buid);
      }
      if (attr == "output_efficiency") {
        return bat.param_output_efficiency(suid, buid);
      }
      return std::nullopt;
    }

    // ── reservoir ────────────────────────────────────────────────────────
    if (ref.element_type == "reservoir") {
      const auto& res = sc.get_element(ObjectSingleId<ReservoirLP> {single_id});
      if (attr == "emin") {
        return res.param_emin(suid, buid);
      }
      if (attr == "emax") {
        return res.param_emax(suid, buid);
      }
      if (attr == "ecost") {
        return res.param_ecost(suid, buid);
      }
      if (attr == "capacity") {
        return res.param_capacity(suid);
      }
      return std::nullopt;
    }

    // ── flow_right ───────────────────────────────────────────────────────
    // Legacy aliases `discharge → target` and `use_value → uvalue`
    // are registered under `class_aliases[]` in
    // share/gtopt/naming_dialects.json (class-scoped because
    // `discharge` is *canonical* on `flow`).  The class-aware
    // canonicalize step above turns either legacy form into the
    // canonical name, so a single compare per attribute suffices.
    if (ref.element_type == "flow_right") {
      const auto& frt = sc.get_element(ObjectSingleId<FlowRightLP> {single_id});
      if (attr == "fmin") {
        return frt.param_fmin(suid, buid);
      }
      if (attr == "fmax") {
        return frt.param_fmax(suid, buid);
      }
      if (attr == "target") {
        return frt.param_target(suid, buid);
      }
      if (attr == "fcost") {
        return frt.param_fcost(suid, buid);
      }
      if (attr == "uvalue") {
        return frt.param_uvalue(suid, buid);
      }
      return std::nullopt;
    }

    // ── volume_right ─────────────────────────────────────────────────────
    if (ref.element_type == "volume_right") {
      const auto& vrt =
          sc.get_element(ObjectSingleId<VolumeRightLP> {single_id});
      if (attr == "fmax") {
        return vrt.param_fmax(suid, buid);
      }
      if (attr == "emin") {
        return vrt.param_emin(suid, buid);
      }
      if (attr == "emax") {
        return vrt.param_emax(suid, buid);
      }
      if (attr == "demand") {
        return vrt.param_demand(suid);
      }
      if (attr == "saving_rate") {
        return vrt.param_saving_rate(suid, buid);
      }
      if (attr == "fail_cost") {
        return vrt.param_fail_cost();
      }
      return std::nullopt;
    }

  } catch (const std::exception& ex) {
    SPDLOG_WARN(
        std::format("user_constraint: cannot resolve param {}.{}('{}'): {}",
                    ref.element_type,
                    ref.attribute,
                    ref.element_id,
                    ex.what()));
    return std::nullopt;
  }

  return std::nullopt;
}

// ── Sum predicate evaluation ────────────────────────────────────────────────

namespace
{

/// Compare a metadata value against a predicate's RHS.  String-vs-number
/// mismatches always return false (the predicate is unsatisfied).
[[nodiscard]] bool eval_predicate(const SumPredicate& pred,
                                  const AmplMetadataValue& value)
{
  using Op = SumPredicate::Op;

  // Set membership: stringify both sides.
  if (pred.op == Op::In) {
    std::string s;
    if (std::holds_alternative<std::string>(value)) {
      s = std::get<std::string>(value);
    } else {
      s = std::format("{}", std::get<double>(value));
    }
    return std::ranges::find(pred.set_values, s) != pred.set_values.end();
  }

  // Numeric predicate.
  if (pred.number_value.has_value()) {
    if (!std::holds_alternative<double>(value)) {
      return false;
    }
    const double lhs = std::get<double>(value);
    const double rhs = *pred.number_value;
    switch (pred.op) {
      case Op::Eq:
        return lhs == rhs;
      case Op::Ne:
        return lhs != rhs;
      case Op::Lt:
        return lhs < rhs;
      case Op::Le:
        return lhs <= rhs;
      case Op::Gt:
        return lhs > rhs;
      case Op::Ge:
        return lhs >= rhs;
      default:
        return false;
    }
  }

  // String predicate.
  if (pred.string_value.has_value()) {
    if (!std::holds_alternative<std::string>(value)) {
      return false;
    }
    const auto& lhs = std::get<std::string>(value);
    const auto& rhs = *pred.string_value;
    switch (pred.op) {
      case Op::Eq:
        return lhs == rhs;
      case Op::Ne:
        return lhs != rhs;
      case Op::Lt:
        return lhs < rhs;
      case Op::Le:
        return lhs <= rhs;
      case Op::Gt:
        return lhs > rhs;
      case Op::Ge:
        return lhs >= rhs;
      default:
        return false;
    }
  }

  return false;
}

/// Return true iff the element identified by (class_name, element_uid)
/// satisfies every predicate in @p filters (AND semantics).  An element
/// with no registered metadata fails any non-empty filter list.
[[nodiscard]] bool element_passes_filters(
    const SystemContext& sc,
    std::string_view class_name,
    Uid element_uid,
    const std::vector<SumPredicate>& filters)
{
  if (filters.empty()) {
    return true;
  }
  const auto* metadata = sc.find_ampl_element_metadata(class_name, element_uid);
  if (metadata == nullptr) {
    return false;
  }
  for (const auto& pred : filters) {
    auto it = std::ranges::find_if(
        *metadata, [&](const auto& kv) { return kv.first == pred.attr; });
    if (it == metadata->end()) {
      return false;
    }
    if (!eval_predicate(pred, it->second)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ── Sum-reference resolution ─────────────────────────────────────────────────

double collect_sum_cols(const SystemContext& sc,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        const BlockLP& block,
                        const SumElementRef& sum_ref,
                        double base_coeff,
                        SparseRow& row,
                        const LinearProblem& lp)
{
  double offset_shift = 0.0;
  // Helper lambda: add one ElementRef to the row.  Uses the compound-aware
  // `resolve_col_to_row` so sums over a compound attribute (e.g.
  // `sum(line(all).flow)`) correctly expand each leg.  The per-leg
  // offset shift (Option C demand etc.) is accumulated into the
  // outer `offset_shift` so the caller can fold it onto the row RHS.
  auto add_one = [&](const std::string& eid)
  {
    ElementRef ref;
    ref.element_type = sum_ref.element_type;
    ref.element_id = eid;
    ref.attribute = sum_ref.attribute;

    const auto res = resolve_col_to_row(
        sc, scenario, stage, block, ref, base_coeff, row, lp);
    offset_shift += res.offset_shift;
  };

  if (sum_ref.all_elements) {
    // Common pattern: iterate a collection, apply AND-of-predicates via
    // the metadata registry, and emit `add_one` for each survivor.
    auto iterate = [&]<typename LP>(std::string_view class_name)
    {
      for (const auto& el : sc.elements<LP>()) {
        if (!element_passes_filters(sc, class_name, el.uid(), sum_ref.filters))
        {
          continue;
        }
        add_one(as_label(el.uid()));
      }
    };

    // NOLINTBEGIN(bugprone-branch-clone): each branch targets a different
    // C++ type (GeneratorLP, DemandLP, etc.).
    if (sum_ref.element_type == "generator") {
      iterate.template operator()<GeneratorLP>("generator");
    } else if (sum_ref.element_type == "demand") {
      iterate.template operator()<DemandLP>("demand");
    } else if (sum_ref.element_type == "line") {
      iterate.template operator()<LineLP>("line");
    } else if (sum_ref.element_type == "battery") {
      iterate.template operator()<BatteryLP>("battery");
    } else if (sum_ref.element_type == "reservoir") {
      iterate.template operator()<ReservoirLP>("reservoir");
    } else if (sum_ref.element_type == "waterway") {
      iterate.template operator()<WaterwayLP>("waterway");
    } else if (sum_ref.element_type == "turbine") {
      iterate.template operator()<TurbineLP>("turbine");
    } else if (sum_ref.element_type == "converter") {
      iterate.template operator()<ConverterLP>("converter");
    } else if (sum_ref.element_type == "junction") {
      iterate.template operator()<JunctionLP>("junction");
    } else if (sum_ref.element_type == "flow") {
      iterate.template operator()<FlowLP>("flow");
    } else if (sum_ref.element_type == "flow_right") {
      iterate.template operator()<FlowRightLP>("flow_right");
    } else if (sum_ref.element_type == "volume_right") {
      iterate.template operator()<VolumeRightLP>("volume_right");
    } else if (sum_ref.element_type == "seepage") {
      iterate.template operator()<ReservoirSeepageLP>("seepage");
    } else if (sum_ref.element_type == "reserve_provision") {
      iterate.template operator()<ReserveProvisionLP>("reserve_provision");
    } else if (sum_ref.element_type == "reserve_zone") {
      iterate.template operator()<ReserveZoneLP>("reserve_zone");
    } else if (sum_ref.element_type == "bus") {
      iterate.template operator()<BusLP>("bus");
    } else if (sum_ref.element_type == "lng_terminal") {
      iterate.template operator()<LngTerminalLP>("lng_terminal");
    }
    // NOLINTEND(bugprone-branch-clone)
  } else {
    for (const auto& eid : sum_ref.element_ids) {
      add_one(eid);
    }
  }
  return offset_shift;
}

}  // namespace gtopt
