/**
 * @file      user_constraint_lp.cpp
 * @brief     LP element implementation for user-defined constraints
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/constraint_expr.hpp>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/storage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/user_constraint.hpp>
#include <gtopt/user_constraint_lp.hpp>
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
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), val);
    const bool ok = ec == std::errc {} && ptr == digits.data() + digits.size();
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
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
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto [ptr, ec] = std::from_chars(
        element_id.data(), element_id.data() + element_id.size(), val);
    const bool ok =
        ec == std::errc {} && ptr == element_id.data() + element_id.size();
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (ok) {
      return val;
    }
  }

  // Otherwise treat as a name
  return Name {element_id};
}

// ── Per-element column resolution ────────────────────────────────────────────

/// A resolved LP column together with its physical-to-LP scale factor.
///
/// The @c scale field satisfies:  physical_value = LP_value × scale.
///
/// When assembling a user constraint  `coeff × physical_var [op] RHS`  the
/// LP-level coefficient is  `coeff × scale`  so that the constraint remains
/// dimensionally correct regardless of internal LP scaling choices (e.g.
/// reservoir volume in Gm³, theta in milli-radians, …).
struct ResolvedCol
{
  ColIndex col;
  double scale {1.0};
};

/**
 * @brief Try to look up the LP `ColIndex` for one element reference.
 *
 * Returns `std::nullopt` when the element is not found, the block is not
 * active in the requested (scenario, stage), or the attribute is unknown.
 *
 * The returned @c ResolvedCol::scale converts the LP variable to physical
 * units so that the caller can build correctly-scaled constraint rows.
 *
 * When a column has a non-unit scale stored in SparseCol::scale (set at
 * variable creation time), the scale is retrieved via
 * `lp.get_col_scale(col)` — providing a uniform mechanism that works for
 * all current and future scaled variables without hardcoding per-element
 * logic.
 */
[[nodiscard]] std::optional<ResolvedCol> resolve_single_col(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref,
    const LinearProblem& lp)
{
  const auto single_id = parse_element_id(ref.element_id);
  const BlockUid buid = block.uid();

  try {
    // ── generator ────────────────────────────────────────────────────────
    if (ref.element_type == "generator") {
      const auto& gen = sc.get_element(ObjectSingleId<GeneratorLP> {single_id});
      if (ref.attribute == "generation") {
        const auto& cols = gen.generation_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── demand ───────────────────────────────────────────────────────────
    if (ref.element_type == "demand") {
      const auto& dem = sc.get_element(ObjectSingleId<DemandLP> {single_id});
      if (ref.attribute == "load") {
        const auto& cols = dem.load_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      } else if (ref.attribute == "fail") {
        const auto& cols = dem.fail_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── line ─────────────────────────────────────────────────────────────
    if (ref.element_type == "line") {
      const auto& ln = sc.get_element(ObjectSingleId<LineLP> {single_id});
      // "flow" is an alias for the forward power flow variable "flowp"
      if (ref.attribute == "flow" || ref.attribute == "flowp") {
        const auto& cols = ln.flowp_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      } else if (ref.attribute == "flown") {
        const auto& cols = ln.flown_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      } else if (ref.attribute == "lossp") {
        const auto& cols = ln.lossp_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      } else if (ref.attribute == "lossn") {
        const auto& cols = ln.lossn_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── battery ──────────────────────────────────────────────────────────
    if (ref.element_type == "battery") {
      const auto& bat = sc.get_element(ObjectSingleId<BatteryLP> {single_id});
      if (ref.attribute == "charge") {
        const auto& cols = bat.finp_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      } else if (ref.attribute == "discharge") {
        const auto& cols = bat.fout_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      } else if (ref.attribute == "energy") {
        const auto& cols = bat.energy_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      } else if (ref.attribute == "spill" || ref.attribute == "drain") {
        const auto& cols = bat.drain_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      }
      return std::nullopt;
    }

    // ── reservoir ────────────────────────────────────────────────────────
    if (ref.element_type == "reservoir") {
      const auto& res = sc.get_element(ObjectSingleId<ReservoirLP> {single_id});
      if (ref.attribute == "volume" || ref.attribute == "energy") {
        const auto& cols = res.energy_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      } else if (ref.attribute == "spill" || ref.attribute == "drain") {
        const auto& cols = res.drain_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      } else if (ref.attribute == "extraction") {
        const auto& cols = res.extraction_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
              .scale = lp.get_col_scale(it->second),
          };
        }
      }
      return std::nullopt;
    }

    // ── waterway ─────────────────────────────────────────────────────────
    if (ref.element_type == "waterway") {
      const auto& ww = sc.get_element(ObjectSingleId<WaterwayLP> {single_id});
      if (ref.attribute == "flow") {
        const auto& cols = ww.flow_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── turbine: delegate to the associated generator ────────────────────
    if (ref.element_type == "turbine") {
      const auto& turb = sc.get_element(ObjectSingleId<TurbineLP> {single_id});
      if (ref.attribute == "generation") {
        const auto& gen = sc.get_element(turb.generator_sid());
        const auto& cols = gen.generation_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── converter: charge → demand.load, discharge → generator.generation
    if (ref.element_type == "converter") {
      const auto& conv =
          sc.get_element(ObjectSingleId<ConverterLP> {single_id});
      if (ref.attribute == "discharge") {
        const auto& gen = sc.get_element(conv.generator_sid());
        const auto& cols = gen.generation_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      } else if (ref.attribute == "charge") {
        const auto& dem = sc.get_element(conv.demand_sid());
        const auto& cols = dem.load_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── bus: voltage angle θ ─────────────────────────────────────────────
    if (ref.element_type == "bus") {
      const auto& bus_lp = sc.get_element(ObjectSingleId<BusLP> {single_id});
      if (ref.attribute == "theta" || ref.attribute == "angle") {
        if (auto col = bus_lp.lookup_theta_col(scenario, stage, buid)) {
          // theta scale is stored in SparseCol::scale at column creation.
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
        return std::nullopt;
      }
      return std::nullopt;
    }

    // ── junction: drain variable ─────────────────────────────────────────
    if (ref.element_type == "junction") {
      const auto& jun = sc.get_element(ObjectSingleId<JunctionLP> {single_id});
      if (ref.attribute == "drain") {
        const auto& cols = jun.drain_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── flow: discharge variable ─────────────────────────────────────────
    if (ref.element_type == "flow") {
      const auto& flw = sc.get_element(ObjectSingleId<FlowLP> {single_id});
      if (ref.attribute == "flow" || ref.attribute == "discharge") {
        const auto& cols = flw.flow_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── seepage: seepage flow variable ──────────────────────────────
    if (ref.element_type == "seepage") {
      const auto& fil =
          sc.get_element(ObjectSingleId<ReservoirSeepageLP> {single_id});
      if (ref.attribute == "flow" || ref.attribute == "seepage") {
        const auto& cols = fil.seepage_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return ResolvedCol {
              .col = it->second,
          };
        }
      }
      return std::nullopt;
    }

    // ── reserve_provision: up/dn reserve provision column ────────────────
    if (ref.element_type == "reserve_provision") {
      const auto& rp =
          sc.get_element(ObjectSingleId<ReserveProvisionLP> {single_id});
      if (ref.attribute == "up" || ref.attribute == "uprovision"
          || ref.attribute == "up_provision")
      {
        if (auto col = rp.lookup_up_provision_col(scenario, stage, buid)) {
          return ResolvedCol {
              .col = *col,
          };
        }
        return std::nullopt;
      }
      if (ref.attribute == "dn" || ref.attribute == "down"
          || ref.attribute == "dprovision" || ref.attribute == "dn_provision")
      {
        if (auto col = rp.lookup_dn_provision_col(scenario, stage, buid)) {
          return ResolvedCol {
              .col = *col,
          };
        }
        return std::nullopt;
      }
      return std::nullopt;
    }

    // ── reserve_zone: up/dn requirement column ───────────────────────────
    if (ref.element_type == "reserve_zone") {
      const auto& rz =
          sc.get_element(ObjectSingleId<ReserveZoneLP> {single_id});
      if (ref.attribute == "up" || ref.attribute == "urequirement"
          || ref.attribute == "up_requirement")
      {
        if (auto col = rz.lookup_urequirement_col(scenario, stage, buid)) {
          return ResolvedCol {
              .col = *col,
          };
        }
        return std::nullopt;
      }
      if (ref.attribute == "dn" || ref.attribute == "down"
          || ref.attribute == "drequirement"
          || ref.attribute == "dn_requirement")
      {
        if (auto col = rz.lookup_drequirement_col(scenario, stage, buid)) {
          return ResolvedCol {
              .col = *col,
          };
        }
        return std::nullopt;
      }
      return std::nullopt;
    }

  } catch (const std::exception& ex) {
    SPDLOG_WARN(std::format("user_constraint: cannot resolve {}.{}('{}'): {}",
                            ref.element_type,
                            ref.attribute,
                            ref.element_id,
                            ex.what()));
    return std::nullopt;
  }

  SPDLOG_WARN(std::format("user_constraint: unknown element type '{}'",
                          ref.element_type));
  return std::nullopt;
}

// ── Sum-reference resolution ─────────────────────────────────────────────────

/**
 * @brief Collect (coefficient, ColIndex) pairs for a `SumElementRef`.
 *
 * When `sum_ref.all_elements` is true, iterates over every element in the
 * collection of the matching type.  Otherwise iterates over the explicit ID
 * list.  The base_coeff is multiplied into each term's coefficient.
 */
void collect_sum_cols(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      const BlockLP& block,
                      const SumElementRef& sum_ref,
                      double base_coeff,
                      SparseRow& row,
                      const LinearProblem& lp)
{
  // Helper lambda: add one ElementRef to the row
  auto add_one = [&](const std::string& eid)
  {
    ElementRef ref;
    ref.element_type = sum_ref.element_type;
    ref.element_id = eid;
    ref.attribute = sum_ref.attribute;

    if (auto resolved = resolve_single_col(sc, scenario, stage, block, ref, lp))
    {
      row[resolved->col] += base_coeff * resolved->scale;
    }
  };

  if (sum_ref.all_elements) {
    // Iterate over every element of the named type
    // NOLINT(bugprone-branch-clone): each branch targets a different C++ type
    // (GeneratorLP, DemandLP, etc.)
    if (sum_ref.element_type == "generator") {  // NOLINT(bugprone-branch-clone)
      for (const auto& gen : sc.elements<GeneratorLP>()) {
        if (sum_ref.type_filter
            && gen.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(std::to_string(static_cast<int>(gen.uid())));
      }
    } else if (sum_ref.element_type == "demand") {
      for (const auto& dem : sc.elements<DemandLP>()) {
        if (sum_ref.type_filter
            && dem.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(std::to_string(static_cast<int>(dem.uid())));
      }
    } else if (sum_ref.element_type == "line") {
      for (const auto& ln : sc.elements<LineLP>()) {
        if (sum_ref.type_filter
            && ln.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(std::to_string(static_cast<int>(ln.uid())));
      }
    } else if (sum_ref.element_type == "battery") {
      for (const auto& bat : sc.elements<BatteryLP>()) {
        if (sum_ref.type_filter
            && bat.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(std::to_string(static_cast<int>(bat.uid())));
      }
    } else if (sum_ref.element_type == "reservoir") {
      // NOLINT(bugprone-branch-clone): reservoir/waterway/turbine/converter
      // don't have a `type` field
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& res : sc.elements<ReservoirLP>()) {
        add_one(std::to_string(static_cast<int>(res.uid())));
      }
    } else if (sum_ref.element_type
               == "waterway") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& ww : sc.elements<WaterwayLP>()) {
        add_one(std::to_string(static_cast<int>(ww.uid())));
      }
    } else if (sum_ref.element_type
               == "turbine") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& t : sc.elements<TurbineLP>()) {
        add_one(std::to_string(static_cast<int>(t.uid())));
      }
    } else if (sum_ref.element_type
               == "converter") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& c : sc.elements<ConverterLP>()) {
        add_one(std::to_string(static_cast<int>(c.uid())));
      }
    } else if (sum_ref.element_type
               == "junction") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& jun : sc.elements<JunctionLP>()) {
        add_one(std::to_string(static_cast<int>(jun.uid())));
      }
    } else if (sum_ref.element_type
               == "flow") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& flw : sc.elements<FlowLP>()) {
        add_one(std::to_string(static_cast<int>(flw.uid())));
      }
    } else if (sum_ref.element_type
               == "seepage") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& fil : sc.elements<ReservoirSeepageLP>()) {
        add_one(std::to_string(static_cast<int>(fil.uid())));
      }
    } else if (sum_ref.element_type
               == "reserve_provision") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& rp : sc.elements<ReserveProvisionLP>()) {
        add_one(std::to_string(static_cast<int>(rp.uid())));
      }
    } else if (sum_ref.element_type
               == "reserve_zone") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& rz : sc.elements<ReserveZoneLP>()) {
        add_one(std::to_string(static_cast<int>(rz.uid())));
      }
    }
  } else {
    for (const auto& eid : sum_ref.element_ids) {
      add_one(eid);
    }
  }
}

// ── Domain check ─────────────────────────────────────────────────────────────

[[nodiscard]] bool in_range(const IndexRange& range, int uid)
{
  if (range.is_all) {
    return true;
  }
  return std::ranges::contains(range.values, uid);
}

// ── SparseRow bounds from ConstraintExpr ─────────────────────────────────────

void apply_constraint_bounds(SparseRow& row, const ConstraintExpr& expr)
{
  switch (expr.constraint_type) {
    case ConstraintType::LESS_EQUAL:
      row.less_equal(expr.rhs);
      break;
    case ConstraintType::GREATER_EQUAL:
      row.greater_equal(expr.rhs);
      break;
    case ConstraintType::EQUAL:
      row.equal(expr.rhs);
      break;
    case ConstraintType::RANGE:
      row.bound(expr.lower_bound.value_or(-SparseRow::CoinDblMax),
                expr.upper_bound.value_or(SparseRow::CoinDblMax));
      break;
  }
}

}  // anonymous namespace

// ── UserConstraintLP implementation ─────────────────────────────────────────

UserConstraintLP::UserConstraintLP(const UserConstraint& uc, InputContext& ic)
    : ObjectLP<UserConstraint>(uc, ic, ClassName)
    , m_scale_type_(
          parse_constraint_scale_type(uc.constraint_type.value_or("power")))
{
  if (!uc.expression.empty()) {
    try {
      m_expr_ = ConstraintParser::parse(uc.name, uc.expression);
    } catch (const std::exception& ex) {
      SPDLOG_ERROR(std::format(
          "user_constraint '{}': expression parse error: {} — "
          "check expression syntax and refer to the user-constraint "
          "documentation; constraint will be silently skipped",
          uc.name,
          ex.what()));
      // m_expr_ stays nullopt; add_to_lp will skip this constraint silently
    }
  }
}

bool UserConstraintLP::add_to_lp(const SystemContext& sc,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 LinearProblem& lp)
{
  if (!m_expr_.has_value()) {
    return true;
  }

  if (!is_active(stage)) {
    return true;
  }

  const auto& expr = *m_expr_;
  const auto& domain = expr.domain;

  // Check domain filters for this (scenario, stage)
  if (!in_range(domain.scenarios, static_cast<int>(scenario.uid()))) {
    return true;
  }
  if (!in_range(domain.stages, static_cast<int>(stage.uid()))) {
    return true;
  }

  const auto& uc = user_constraint();
  BIndexHolder<RowIndex> block_rows;
  map_reserve(block_rows, stage.blocks().size());

  for (const auto& block : stage.blocks()) {
    if (!in_range(domain.blocks, static_cast<int>(block.uid()))) {
      continue;
    }

    SparseRow row;
    row.name = std::format("{}_s{}_t{}_b{}",
                           uc.name,
                           static_cast<int>(scenario.uid()),
                           static_cast<int>(stage.uid()),
                           static_cast<int>(block.uid()));

    bool has_vars = false;
    for (const auto& term : expr.terms) {
      if (term.element) {
        if (auto resolved = resolve_single_col(
                sc, scenario, stage, block, *term.element, lp))
        {
          row[resolved->col] += term.coefficient * resolved->scale;
          has_vars = true;
        }
      } else if (term.sum_ref) {
        const std::size_t before = row.size();
        collect_sum_cols(sc,
                         scenario,
                         stage,
                         block,
                         *term.sum_ref,
                         term.coefficient,
                         row,
                         lp);
        if (row.size() > before) {
          has_vars = true;
        }
      }
    }

    if (!has_vars) {
      SPDLOG_DEBUG(
          std::format("user_constraint '{}': no LP columns resolved "
                      "for block {} — skipping",
                      uc.name,
                      static_cast<int>(block.uid())));
      continue;
    }

    apply_constraint_bounds(row, expr);
    const auto row_idx = lp.add_row(std::move(row));
    block_rows[block.uid()] = row_idx;
  }

  if (!block_rows.empty()) {
    m_rows_[{scenario.uid(), stage.uid()}] = std::move(block_rows);
  }

  return true;
}

bool UserConstraintLP::add_to_output(OutputContext& out) const
{
  if (m_rows_.empty()) {
    return true;
  }

  const Id pid {uid(), user_constraint().name};

  if (m_scale_type_ == ConstraintScaleType::Raw) {
    // Raw / unitless constraints: scale by discount factor only
    // (scale_obj / discount[t]), no probability and no block duration.
    out.add_row_dual_raw(ClassName.full_name(), "constraint", pid, m_rows_);
  } else {
    // Power and Energy constraints: standard block_cost_factors scaling
    // (scale_obj / (prob × discount × Δt)).
    // "power"  → dual in $/MW;  "energy" → dual in $/MWh.
    out.add_row_dual(ClassName.full_name(), "constraint", pid, m_rows_);
  }

  return true;
}

}  // namespace gtopt
