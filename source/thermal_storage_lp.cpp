/**
 * @file      thermal_storage_lp.cpp
 * @brief     Implementation of ThermalStorageLP — CSP / district-heat TES
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``source/battery_lp.cpp`` re-targeted to the thermal
 * carrier (MWh_th).  All LP semantics flow through ``StorageLP<>``
 * unchanged — only the element / class-name / id types differ.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/thermal_node_lp.hpp>
#include <gtopt/thermal_storage_lp.hpp>

namespace gtopt
{

ThermalStorageLP::ThermalStorageLP(const ThermalStorage& pts,
                                   const InputContext& ic)
    : StorageBase(pts, ic, Element::class_name)
    , input_efficiency(
          ic, Element::class_name, id(), std::move(object().input_efficiency))
    , output_efficiency(
          ic, Element::class_name, id(), std::move(object().output_efficiency))
{
}

bool ThermalStorageLP::add_to_lp(SystemContext& sc,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 LinearProblem& lp)
{
  static constexpr const auto& cname = Element::class_name;
  static constexpr auto ampl_name = Element::class_name.snake_case();
  static constexpr double flow_conversion_rate = 1.0;

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  // Register filter metadata for sum(...) predicates.  Mirrors Battery:
  // ``thermal_node`` is optional in the data struct (the carrier-side
  // balance coupling is added later by ThermalNodeLP, not here), so
  // only the ``type`` tag is registered.
  if (const auto& t = thermal_storage().type) {
    AmplElementMetadata metadata;
    metadata.emplace_back(TypeKey, *t);
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  auto&& [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  const auto input_efficiency_at = [&](BlockUid b)
  { return input_efficiency.optval(stage.uid(), b).value_or(1.0); };
  const auto output_efficiency_at = [&](BlockUid b)
  { return output_efficiency.optval(stage.uid(), b).value_or(1.0); };

  const auto& blocks = stage.blocks();

  const auto es = sc.options().variable_scale_map().lookup(
      "ThermalStorage", "energy", uid());

  BIndexHolder<ColIndex> finps;
  BIndexHolder<ColIndex> fouts;
  map_reserve(finps, blocks.size());
  map_reserve(fouts, blocks.size());

  double fs = 1.0;
  for (auto&& block : blocks) {
    const auto buid = block.uid();
    finps[buid] = lp.add_col(SparseCol {
        .class_name = Element::class_name.full_name(),
        .variable_name = FinpName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fouts[buid] = lp.add_col(SparseCol {
        .class_name = Element::class_name.full_name(),
        .variable_name = FoutName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fs = lp.get_col_scale(finps[buid]);
  }
  const StorageOptions opts {
      .use_state_variable =
          thermal_storage().use_state_variable.value_or(false),
      .daily_cycle = thermal_storage().daily_cycle.value_or(false),
      .class_name = Element::class_name.full_name(),
      .variable_uid = uid(),
      .energy_scale = es,
      .flow_scale = fs,
  };
  if (!StorageBase::add_to_lp(cname,
                              ampl_name,
                              sc,
                              scenario,
                              stage,
                              lp,
                              flow_conversion_rate,
                              finps,
                              input_efficiency_at,
                              fouts,
                              output_efficiency_at,
                              stage_capacity,
                              capacity_col,
                              {},
                              {},
                              opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for thermal_storage {}",
                    uid());
    return false;
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  finp_cols[st_key] = std::move(finps);
  fout_cols[st_key] = std::move(fouts);

  // Stamp into the ThermalNode balance row if a thermal_node ref is
  // set.  Sign convention (mirrors BusLP):
  //   * finp (charging the TES) pulls heat from the node    → −1
  //   * fout (discharging the TES) injects heat to the node → +1
  if (const auto& tn_ref = thermal_storage().thermal_node; tn_ref.has_value()) {
    const auto& tn_lp =
        sc.element<ThermalNodeLP>(ThermalNodeLPSId {tn_ref.value()});
    if (tn_lp.is_active(stage)) {
      const auto& brows = tn_lp.balance_rows_at(scenario, stage);
      const auto& finps_at = finp_cols.at(st_key);
      const auto& fouts_at = fout_cols.at(st_key);
      for (auto&& block : blocks) {
        const auto buid = block.uid();
        auto& brow = lp.row_at(brows.at(buid));
        brow[finps_at.at(buid)] = -1.0;
        brow[fouts_at.at(buid)] = +1.0;
      }
    }
  }

  sc.add_ampl_variable(
      ampl_name, uid(), ChargeName, scenario, stage, finp_cols.at(st_key));
  sc.add_ampl_variable(
      ampl_name, uid(), DischargeName, scenario, stage, fout_cols.at(st_key));

  return true;
}

bool ThermalStorageLP::add_to_output(OutputContext& out) const
{
  static constexpr const auto& cname = Element::class_name;

  out.add_col_sol(cname, FinpName, id(), finp_cols);
  out.add_col_cost(cname, FinpName, id(), finp_cols);
  out.add_col_sol(cname, FoutName, id(), fout_cols);
  out.add_col_cost(cname, FoutName, id(), fout_cols);

  return StorageBase::add_to_output(out, cname)
      && CapacityBase::add_to_output(out);
}

}  // namespace gtopt
