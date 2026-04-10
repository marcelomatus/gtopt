/**
 * @file      element_column_resolver.cpp
 * @brief     Resolve LP column indices for user-constraint element references
 * @date      Mon Mar 24 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <charconv>
#include <format>
#include <ranges>

#include <gtopt/as_label.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/constraint_expr.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/element_column_resolver.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/single_id.hpp>
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
  const auto single_id = parse_element_id(ref.element_id);
  const BlockUid buid = block.uid();

  // Local helper: collapse the common "find+check+build ResolvedCol" pattern
  // used for every per-block column map below.  Returns nullopt when `buid`
  // is not present (no bounds-throwing .at(), no duplicated lookups).
  const auto resolve_from_map =
      [&](const auto& cols) -> std::optional<ResolvedCol>
  {
    if (const auto it = cols.find(buid); it != cols.end()) {
      return ResolvedCol {
          .col = it->second,
          .scale = lp.get_col_scale(it->second),
      };
    }
    return std::nullopt;
  };

  try {
    // ── generator ────────────────────────────────────────────────────────
    if (ref.element_type == "generator") {
      const auto& gen = sc.get_element(ObjectSingleId<GeneratorLP> {single_id});
      if (ref.attribute == GeneratorLP::GenerationName) {
        return resolve_from_map(gen.generation_cols_at(scenario, stage));
      }
      if (ref.attribute == CapacityObjectBase::CapainstName
          || ref.attribute == GeneratorLP::CapacityName)
      {
        // Stage-level capacity installation variable (expansion column).
        // The same column is returned for every block in the stage.
        if (auto col = gen.capacity_col_at(stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      return std::nullopt;
    }

    // ── demand ───────────────────────────────────────────────────────────
    if (ref.element_type == "demand") {
      const auto& dem = sc.get_element(ObjectSingleId<DemandLP> {single_id});
      if (ref.attribute == DemandLP::LoadName) {
        return resolve_from_map(dem.load_cols_at(scenario, stage));
      }
      if (ref.attribute == DemandLP::FailName) {
        return resolve_from_map(dem.fail_cols_at(scenario, stage));
      }
      if (ref.attribute == CapacityObjectBase::CapainstName
          || ref.attribute == DemandLP::CapacityName)
      {
        // Stage-level capacity installation variable.
        if (auto col = dem.capacity_col_at(stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      return std::nullopt;
    }

    // ── line ─────────────────────────────────────────────────────────────
    if (ref.element_type == "line") {
      const auto& ln = sc.get_element(ObjectSingleId<LineLP> {single_id});
      // Note: "flow" is not supported for lines — it is semantically
      // (flowp - flown), not an alias.  Users must write
      //   `line("X").flowp - line("X").flown`
      // explicitly if they want net power flow.
      if (ref.attribute == LineLP::FlowpName) {
        return resolve_from_map(ln.flowp_cols_at(scenario, stage));
      }
      if (ref.attribute == LineLP::FlownName) {
        return resolve_from_map(ln.flown_cols_at(scenario, stage));
      }
      if (ref.attribute == LineLP::LosspName) {
        return resolve_from_map(ln.lossp_cols_at(scenario, stage));
      }
      if (ref.attribute == LineLP::LossnName) {
        return resolve_from_map(ln.lossn_cols_at(scenario, stage));
      }
      if (ref.attribute == CapacityObjectBase::CapainstName) {
        // Stage-level capacity installation variable.
        if (auto col = ln.capacity_col_at(stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      return std::nullopt;
    }

    // ── battery ──────────────────────────────────────────────────────────
    if (ref.element_type == "battery") {
      const auto& bat = sc.get_element(ObjectSingleId<BatteryLP> {single_id});
      if (ref.attribute == BatteryLP::ChargeName) {
        return resolve_from_map(bat.finp_cols_at(scenario, stage));
      }
      if (ref.attribute == BatteryLP::DischargeName) {
        return resolve_from_map(bat.fout_cols_at(scenario, stage));
      }
      if (ref.attribute == BatteryLP::EnergyName) {
        return resolve_from_map(bat.energy_cols_at(scenario, stage));
      }
      if (ref.attribute == BatteryLP::SpillName
          || ref.attribute == BatteryLP::DrainName)
      {
        return resolve_from_map(bat.drain_cols_at(scenario, stage));
      }
      if (ref.attribute == BatteryLP::EiniName) {
        // Stage-level initial energy column (state variable).
        // Same column for all blocks in the stage.
        const auto col = bat.eini_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == BatteryLP::EfinName) {
        // Stage-level final energy column.
        // Same column for all blocks in the stage.
        const auto col = bat.efin_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == BatteryLP::SoftEminName) {
        // Stage-level soft minimum energy slack column.
        // Returns nullopt when soft_emin is inactive for this stage.
        if (auto col = bat.soft_emin_col_at(scenario, stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      if (ref.attribute == CapacityObjectBase::CapainstName
          || ref.attribute == BatteryLP::CapacityName)
      {
        // Stage-level capacity installation variable.
        if (auto col = bat.capacity_col_at(stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      return std::nullopt;
    }

    // ── reservoir ────────────────────────────────────────────────────────
    if (ref.element_type == "reservoir") {
      const auto& res = sc.get_element(ObjectSingleId<ReservoirLP> {single_id});
      if (ref.attribute == ReservoirLP::VolumeName
          || ref.attribute == ReservoirLP::EnergyName)
      {
        return resolve_from_map(res.energy_cols_at(scenario, stage));
      }
      if (ref.attribute == ReservoirLP::SpillName
          || ref.attribute == ReservoirLP::DrainName)
      {
        return resolve_from_map(res.drain_cols_at(scenario, stage));
      }
      if (ref.attribute == ReservoirLP::ExtractionName) {
        return resolve_from_map(res.extraction_cols_at(scenario, stage));
      }
      if (ref.attribute == ReservoirLP::EiniName) {
        // Stage-level initial volume column (state variable).
        const auto col = res.eini_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == ReservoirLP::EfinName) {
        // Stage-level final volume column.
        const auto col = res.efin_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == ReservoirLP::SoftEminName) {
        // Stage-level soft minimum volume slack column.
        if (auto col = res.soft_emin_col_at(scenario, stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
          };
        }
      }
      return std::nullopt;
    }

    // ── waterway ─────────────────────────────────────────────────────────
    if (ref.element_type == "waterway") {
      const auto& ww = sc.get_element(ObjectSingleId<WaterwayLP> {single_id});
      if (ref.attribute == WaterwayLP::FlowName) {
        return resolve_from_map(ww.flow_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── turbine: delegate to the associated generator ────────────────────
    if (ref.element_type == "turbine") {
      const auto& turb = sc.get_element(ObjectSingleId<TurbineLP> {single_id});
      if (ref.attribute == GeneratorLP::GenerationName) {
        const auto& gen = sc.get_element(turb.generator_sid());
        return resolve_from_map(gen.generation_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── converter: charge → demand.load, discharge → generator.generation
    if (ref.element_type == "converter") {
      const auto& conv =
          sc.get_element(ObjectSingleId<ConverterLP> {single_id});
      if (ref.attribute == BatteryLP::DischargeName) {
        const auto& gen = sc.get_element(conv.generator_sid());
        return resolve_from_map(gen.generation_cols_at(scenario, stage));
      }
      if (ref.attribute == BatteryLP::ChargeName) {
        const auto& dem = sc.get_element(conv.demand_sid());
        return resolve_from_map(dem.load_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── bus: voltage angle θ ─────────────────────────────────────────────
    if (ref.element_type == "bus") {
      const auto& bus_lp = sc.get_element(ObjectSingleId<BusLP> {single_id});
      if (ref.attribute == BusLP::ThetaName
          || ref.attribute == BusLP::AngleName)
      {
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
      if (ref.attribute == JunctionLP::DrainName) {
        return resolve_from_map(jun.drain_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── flow: discharge variable ─────────────────────────────────────────
    if (ref.element_type == "flow") {
      const auto& flw = sc.get_element(ObjectSingleId<FlowLP> {single_id});
      if (ref.attribute == FlowLP::FlowName
          || ref.attribute == FlowLP::DischargeName)
      {
        return resolve_from_map(flw.flow_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── flow_right: water-rights flow variable ────────────────────────
    if (ref.element_type == "flow_right") {
      const auto& frt = sc.get_element(ObjectSingleId<FlowRightLP> {single_id});
      if (ref.attribute == FlowRightLP::FlowName) {
        return resolve_from_map(frt.flow_cols_at(scenario, stage));
      }
      if (ref.attribute == FlowRightLP::FailName) {
        return resolve_from_map(frt.fail_cols_at(scenario, stage));
      }
      return std::nullopt;
    }

    // ── volume_right: water-rights extraction flow variable ──────────
    if (ref.element_type == "volume_right") {
      const auto& vrt =
          sc.get_element(ObjectSingleId<VolumeRightLP> {single_id});
      if (ref.attribute == VolumeRightLP::ExtractionName
          || ref.attribute == VolumeRightLP::FlowName
          || ref.attribute == VolumeRightLP::FoutName)
      {
        return resolve_from_map(vrt.extraction_cols_at(scenario, stage));
      }
      if (ref.attribute == VolumeRightLP::SavingName) {
        return resolve_from_map(vrt.saving_cols_at(scenario, stage));
      }
      if (ref.attribute == VolumeRightLP::EnergyName
          || ref.attribute == VolumeRightLP::VolumeName)
      {
        // Per-block accumulated rights volume column.
        // "volume" is the domain-natural name for water rights;
        // "energy" is the StorageLP base-class name — both resolve
        // to the same LP column, consistent with reservoir and battery.
        return resolve_from_map(vrt.energy_cols_at(scenario, stage));
      }
      if (ref.attribute == VolumeRightLP::EiniName) {
        // Stage-level initial rights volume column (state variable).
        // Enables PAMPL constraints to reference or set the initial
        // accumulated volume at the start of a stage — critical for
        // month-based reset of Maule/Laja volume rights.
        const auto col = vrt.eini_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == VolumeRightLP::EfinName) {
        // Stage-level final rights volume column.
        const auto col = vrt.efin_col_at(scenario, stage);
        return ResolvedCol {
            .col = col,
            .scale = lp.get_col_scale(col),
        };
      }
      if (ref.attribute == VolumeRightLP::SoftEminName) {
        // Stage-level soft minimum volume slack column.
        if (auto col = vrt.soft_emin_col_at(scenario, stage)) {
          return ResolvedCol {
              .col = *col,
              .scale = lp.get_col_scale(*col),
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
        return resolve_from_map(cols);
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
              .scale = lp.get_col_scale(*col),
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
              .scale = lp.get_col_scale(*col),
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
              .scale = lp.get_col_scale(*col),
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
              .scale = lp.get_col_scale(*col),
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

// ── Per-element parameter resolution ────────────────────────────────────────

[[nodiscard]] std::optional<double> resolve_single_param(
    const SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    const ElementRef& ref)
{
  const auto single_id = parse_element_id(ref.element_id);
  const auto suid = stage.uid();
  const auto buid = block.uid();

  try {
    // ── generator ────────────────────────────────────────────────────────
    if (ref.element_type == "generator") {
      const auto& gen = sc.get_element(ObjectSingleId<GeneratorLP> {single_id});
      if (ref.attribute == "pmax") {
        return gen.param_pmax(suid, buid);
      }
      if (ref.attribute == "pmin") {
        return gen.param_pmin(suid, buid);
      }
      if (ref.attribute == "gcost") {
        return gen.param_gcost(suid);
      }
      if (ref.attribute == "lossfactor") {
        return gen.param_lossfactor(suid);
      }
      return std::nullopt;
    }

    // ── demand ───────────────────────────────────────────────────────────
    if (ref.element_type == "demand") {
      const auto& dem = sc.get_element(ObjectSingleId<DemandLP> {single_id});
      if (ref.attribute == "lmax") {
        return dem.param_lmax(suid, buid);
      }
      if (ref.attribute == "fcost") {
        return dem.param_fcost(suid);
      }
      if (ref.attribute == "lossfactor") {
        return dem.param_lossfactor(suid);
      }
      return std::nullopt;
    }

    // ── line ─────────────────────────────────────────────────────────────
    if (ref.element_type == "line") {
      const auto& ln = sc.get_element(ObjectSingleId<LineLP> {single_id});
      if (ref.attribute == "tmax" || ref.attribute == "tmax_ab") {
        return ln.param_tmax_ab(suid, buid);
      }
      if (ref.attribute == "tmax_ba") {
        return ln.param_tmax_ba(suid, buid);
      }
      if (ref.attribute == "tcost") {
        return ln.param_tcost(suid);
      }
      if (ref.attribute == "reactance") {
        return ln.param_reactance(suid);
      }
      return std::nullopt;
    }

    // ── battery ──────────────────────────────────────────────────────────
    if (ref.element_type == "battery") {
      const auto& bat = sc.get_element(ObjectSingleId<BatteryLP> {single_id});
      if (ref.attribute == "emin") {
        return bat.param_emin(suid);
      }
      if (ref.attribute == "emax") {
        return bat.param_emax(suid);
      }
      if (ref.attribute == "ecost") {
        return bat.param_ecost(suid);
      }
      if (ref.attribute == "input_efficiency") {
        return bat.param_input_efficiency(suid);
      }
      if (ref.attribute == "output_efficiency") {
        return bat.param_output_efficiency(suid);
      }
      return std::nullopt;
    }

    // ── reservoir ────────────────────────────────────────────────────────
    if (ref.element_type == "reservoir") {
      const auto& res = sc.get_element(ObjectSingleId<ReservoirLP> {single_id});
      if (ref.attribute == "emin") {
        return res.param_emin(suid);
      }
      if (ref.attribute == "emax") {
        return res.param_emax(suid);
      }
      if (ref.attribute == "ecost") {
        return res.param_ecost(suid);
      }
      if (ref.attribute == "capacity") {
        return res.param_capacity(suid);
      }
      return std::nullopt;
    }

    // ── flow_right ───────────────────────────────────────────────────────
    if (ref.element_type == "flow_right") {
      const auto& frt = sc.get_element(ObjectSingleId<FlowRightLP> {single_id});
      if (ref.attribute == "fmax") {
        return frt.param_fmax(suid, buid);
      }
      if (ref.attribute == "discharge") {
        return frt.param_discharge(scenario.uid(), suid, buid);
      }
      if (ref.attribute == "fail_cost") {
        return frt.param_fail_cost(suid, buid);
      }
      if (ref.attribute == "use_value") {
        return frt.param_use_value(suid, buid);
      }
      return std::nullopt;
    }

    // ── volume_right ─────────────────────────────────────────────────────
    if (ref.element_type == "volume_right") {
      const auto& vrt =
          sc.get_element(ObjectSingleId<VolumeRightLP> {single_id});
      if (ref.attribute == "fmax") {
        return vrt.param_fmax(suid, buid);
      }
      if (ref.attribute == "emin") {
        return vrt.param_emin(suid);
      }
      if (ref.attribute == "emax") {
        return vrt.param_emax(suid);
      }
      if (ref.attribute == "demand") {
        return vrt.param_demand(suid);
      }
      if (ref.attribute == "saving_rate") {
        return vrt.param_saving_rate(suid, buid);
      }
      if (ref.attribute == "fail_cost") {
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

// ── Sum-reference resolution ─────────────────────────────────────────────────

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
      row[resolved->col] += base_coeff;
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
        add_one(as_label(gen.uid()));
      }
    } else if (sum_ref.element_type == "demand") {
      for (const auto& dem : sc.elements<DemandLP>()) {
        if (sum_ref.type_filter
            && dem.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(as_label(dem.uid()));
      }
    } else if (sum_ref.element_type == "line") {
      for (const auto& ln : sc.elements<LineLP>()) {
        if (sum_ref.type_filter
            && ln.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(as_label(ln.uid()));
      }
    } else if (sum_ref.element_type == "battery") {
      for (const auto& bat : sc.elements<BatteryLP>()) {
        if (sum_ref.type_filter
            && bat.object().type.value_or("") != *sum_ref.type_filter)
        {
          continue;
        }
        add_one(as_label(bat.uid()));
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
        add_one(as_label(res.uid()));
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
        add_one(as_label(ww.uid()));
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
        add_one(as_label(t.uid()));
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
        add_one(as_label(c.uid()));
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
        add_one(as_label(jun.uid()));
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
        add_one(as_label(flw.uid()));
      }
    } else if (sum_ref.element_type
               == "flow_right") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& frt : sc.elements<FlowRightLP>()) {
        add_one(as_label(frt.uid()));
      }
    } else if (sum_ref.element_type
               == "volume_right") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& vrt : sc.elements<VolumeRightLP>()) {
        add_one(as_label(vrt.uid()));
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
        add_one(as_label(fil.uid()));
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
        add_one(as_label(rp.uid()));
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
        add_one(as_label(rz.uid()));
      }
    } else if (sum_ref.element_type
               == "bus") {  // NOLINT(bugprone-branch-clone)
      if (sum_ref.type_filter) {
        SPDLOG_WARN(std::format(
            "user_constraint sum({}): type_filter is not supported for "
            "element type '{}' — filter ignored",
            sum_ref.element_type,
            sum_ref.element_type));
      }
      for (const auto& bus : sc.elements<BusLP>()) {
        add_one(as_label(bus.uid()));
      }
    }
  } else {
    for (const auto& eid : sum_ref.element_ids) {
      add_one(eid);
    }
  }
}

}  // namespace gtopt
