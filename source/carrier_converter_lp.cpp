/**
 * @file      carrier_converter_lp.cpp
 * @brief     Implementation of CarrierConverterLP (multi-carrier converter)
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <stdexcept>

#include <gtopt/ammonia_node_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/carrier_converter_lp.hpp>
#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/thermal_node_lp.hpp>

namespace gtopt
{

namespace
{

/// Resolves the balance_rows of a node identified by ``(carrier, sid)``
/// against the matching XxxNodeLP collection in the SystemContext.
/// Returns a const reference — every carrier-node LP exposes its
/// per-(scenario, stage) row map via the same shape.
[[nodiscard]] const BIndexHolder<RowIndex>& balance_rows_for(
    const SystemContext& sc,
    Carrier carrier,
    const SingleId& sid,
    const ScenarioLP& scenario,
    const StageLP& stage)
{
  switch (carrier) {
    case Carrier::Electric:
      return sc.element<BusLP>(BusLPSId {sid}).balance_rows_at(scenario, stage);
    case Carrier::Water:
      return sc.element<JunctionLP>(JunctionLPSId {sid})
          .balance_rows_at(scenario, stage);
    case Carrier::Thermal:
      return sc.element<ThermalNodeLP>(ThermalNodeLPSId {sid})
          .balance_rows_at(scenario, stage);
    case Carrier::Hydrogen:
      return sc.element<HydrogenNodeLP>(HydrogenNodeLPSId {sid})
          .balance_rows_at(scenario, stage);
    case Carrier::Ammonia:
      return sc.element<AmmoniaNodeLP>(AmmoniaNodeLPSId {sid})
          .balance_rows_at(scenario, stage);
  }
  throw std::invalid_argument {std::format(
      "CarrierConverterLP: unknown carrier {}", static_cast<int>(carrier))};
}

}  // namespace

CarrierConverterLP::CarrierConverterLP(const CarrierConverter& pcc,
                                       const InputContext& ic)
    : CapacityBase(pcc, ic, Element::class_name)
    , efficiency(ic, Element::class_name, id(), std::move(object().efficiency))
    , ocost(ic, Element::class_name, id(), std::move(object().ocost))
{
}

bool CarrierConverterLP::add_to_lp(SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  if (const auto& t = carrier_converter().type) {
    AmplElementMetadata metadata;
    metadata.emplace_back(TypeKey, *t);
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  auto&& [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);

  const auto& blocks = stage.blocks();

  const auto efficiency_at = [&](BlockUid b)
  { return efficiency.optval(stage.uid(), b).value_or(1.0); };
  const auto ocost_at = [&](BlockUid b)
  { return ocost.optval(stage.uid(), b).value_or(0.0); };

  const auto& cc = carrier_converter();
  const bool has_from = cc.from_node.has_value();
  const bool has_to = cc.to_node.has_value();

  BIndexHolder<ColIndex> inputs;
  map_reserve(inputs, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const double cost_b =
        CostHelper::block_ecost(scenario, stage, block, ocost_at(buid));
    inputs[buid] = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = stage_capacity,
        .cost = cost_b,
        .class_name = Element::class_name.full_name(),
        .variable_name = InputName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  input_cols[st_key] = std::move(inputs);
  const auto& inputs_at = input_cols.at(st_key);

  // Stamp into the from-node balance row: −1 × input (withdrawal).
  if (has_from) {
    const auto& brows_from = balance_rows_for(
        sc, cc.from_carrier, cc.from_node.value(), scenario, stage);
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      auto& brow = lp.row_at(brows_from.at(buid));
      brow[inputs_at.at(buid)] = -1.0;
    }
  }

  // Stamp into the to-node balance row: +efficiency × input (injection).
  if (has_to) {
    const auto& brows_to = balance_rows_for(
        sc, cc.to_carrier, cc.to_node.value(), scenario, stage);
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      auto& brow = lp.row_at(brows_to.at(buid));
      brow[inputs_at.at(buid)] = +efficiency_at(buid);
    }
  }

  sc.add_ampl_variable(
      ampl_name, uid(), InputName, scenario, stage, input_cols.at(st_key));

  return true;
}

bool CarrierConverterLP::add_to_output(OutputContext& out) const
{
  static constexpr const auto& cname = Element::class_name;
  out.add_col_sol(cname, InputName, id(), input_cols);
  out.add_col_cost(cname, InputName, id(), input_cols);
  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
