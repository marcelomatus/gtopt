/**
 * @file      user_constraint_lp.cpp
 * @brief     LP application of user-defined constraints (pre-flattening)
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
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/reservoir_lp.hpp>
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
      && std::all_of(element_id.begin(),
                     element_id.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; }))
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

/**
 * @brief Try to look up the LP `ColIndex` for one element reference.
 *
 * Returns `std::nullopt` when the element is not found, the block is not
 * active in the requested (scenario, stage), or the attribute is unknown.
 */
[[nodiscard]] std::optional<ColIndex> resolve_single_col(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref)
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
          return it->second;
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
          return it->second;
        }
      } else if (ref.attribute == "fail") {
        const auto& cols = dem.fail_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return it->second;
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
          return it->second;
        }
      } else if (ref.attribute == "flown") {
        const auto& cols = ln.flown_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return it->second;
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
          return it->second;
        }
      } else if (ref.attribute == "discharge") {
        const auto& cols = bat.fout_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return it->second;
        }
      } else if (ref.attribute == "energy") {
        const auto& cols = bat.energy_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return it->second;
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
          return it->second;
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
          return it->second;
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
          return it->second;
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
          return it->second;
        }
      } else if (ref.attribute == "charge") {
        const auto& dem = sc.get_element(conv.demand_sid());
        const auto& cols = dem.load_cols_at(scenario, stage);
        if (const auto it = cols.find(buid); it != cols.end()) {
          return it->second;
        }
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
                      SparseRow& row)
{
  // Helper lambda: add one ElementRef to the row
  auto add_one = [&](const std::string& eid)
  {
    ElementRef ref;
    ref.element_type = sum_ref.element_type;
    ref.element_id = eid;
    ref.attribute = sum_ref.attribute;

    if (auto col = resolve_single_col(sc, scenario, stage, block, ref)) {
      row[*col] += base_coeff;
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
      for (const auto& res : sc.elements<ReservoirLP>()) {
        add_one(std::to_string(static_cast<int>(res.uid())));
      }
    } else if (sum_ref.element_type
               == "waterway") {  // NOLINT(bugprone-branch-clone)
      for (const auto& ww : sc.elements<WaterwayLP>()) {
        add_one(std::to_string(static_cast<int>(ww.uid())));
      }
    } else if (sum_ref.element_type
               == "turbine") {  // NOLINT(bugprone-branch-clone)
      for (const auto& t : sc.elements<TurbineLP>()) {
        add_one(std::to_string(static_cast<int>(t.uid())));
      }
    } else if (sum_ref.element_type
               == "converter") {  // NOLINT(bugprone-branch-clone)
      for (const auto& c : sc.elements<ConverterLP>()) {
        add_one(std::to_string(static_cast<int>(c.uid())));
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

// ── Per-constraint row building
// ───────────────────────────────────────────────

void add_constraint_rows(const ConstraintExpr& expr,
                         const std::string& row_name_base,
                         const SystemContext& sc,
                         const PhaseLP& phase,
                         const SceneLP& scene,
                         LinearProblem& lp)
{
  const auto& domain = expr.domain;

  for (auto&& stage : phase.stages()) {
    if (!in_range(domain.stages, static_cast<int>(stage.uid()))) {
      continue;
    }

    for (auto&& scenario : scene.scenarios()) {
      if (!in_range(domain.scenarios, static_cast<int>(scenario.uid()))) {
        continue;
      }

      for (auto&& block : stage.blocks()) {
        if (!in_range(domain.blocks, static_cast<int>(block.uid()))) {
          continue;
        }

        SparseRow row;
        row.name = std::format("{}_s{}_t{}_b{}",
                               row_name_base,
                               static_cast<int>(scenario.uid()),
                               static_cast<int>(stage.uid()),
                               static_cast<int>(block.uid()));

        // Build coefficients from expression terms
        bool has_vars = false;
        for (const auto& term : expr.terms) {
          if (term.element) {
            if (auto col = resolve_single_col(
                    sc, scenario, stage, block, *term.element))
            {
              row[*col] += term.coefficient;
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
                             row);
            if (row.size() > before) {
              has_vars = true;
            }
          }
          // Pure constant terms (no element) shift only the RHS, which is
          // already encoded in expr.rhs / lower_bound / upper_bound.
        }

        if (!has_vars) {
          SPDLOG_DEBUG(
              std::format("user_constraint '{}': no LP columns resolved "
                          "for block {} — skipping",
                          row_name_base,
                          static_cast<int>(block.uid())));
          continue;
        }

        apply_constraint_bounds(row, expr);
        [[maybe_unused]] const auto row_idx = lp.add_row(std::move(row));
      }
    }
  }
}

}  // anonymous namespace

// ── Public function
// ───────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-use-internal-linkage): declared in user_constraint_lp.hpp
void add_user_constraints_to_lp(const std::vector<UserConstraint>& constraints,
                                SystemContext& sc,
                                const PhaseLP& phase,
                                const SceneLP& scene,
                                LinearProblem& lp)
{
  for (const auto& uc : constraints) {
    if (!uc.active.value_or(true)) {
      SPDLOG_DEBUG(
          std::format("user_constraint '{}': inactive — skipping", uc.name));
      continue;
    }

    if (uc.expression.empty()) {
      SPDLOG_WARN(std::format(
          "user_constraint '{}': empty expression — skipping", uc.name));
      continue;
    }

    try {
      const auto expr = ConstraintParser::parse(uc.name, uc.expression);
      add_constraint_rows(expr, uc.name, sc, phase, scene, lp);
    } catch (const std::exception& ex) {
      SPDLOG_ERROR(std::format(
          "user_constraint '{}': parse/apply error: {}", uc.name, ex.what()));
    }
  }
}

}  // namespace gtopt
