/**
 * @file      ampl_dispatch_registry.cpp
 * @brief     AMPL parameter + iterator dispatch table population
 * @date      2026-05-19
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Every shim below is a thin
 * `sc.get_element(ObjectSingleId<LP>{uid}).param_X(s, b)`
 * forwarder for one (LP class, attribute) pair.  The shim is registered
 * against the corresponding user-facing PAMPL spelling via
 * `SimulationLP::register_ampl_param`; the user-constraint resolver in
 * `element_column_resolver.cpp` reaches it through a single
 * `find_ampl_param` probe.
 *
 * Per-stage `param_X(s)` accessors discard the block argument; the
 * resolver passes `(s, b)` uniformly so the call site stays branch-free.
 *
 * Adding a new (class, attribute) is one new shim + one
 * `register_ampl_param` line — no resolver edit.
 */

#include <gtopt/ampl_dispatch_registry.hpp>
#include <gtopt/ampl_variable.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/emission_source_lp.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

namespace
{

// Generator
std::optional<double> gen_param_pmax(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid}).param_pmax(s, b);
}
std::optional<double> gen_param_pmin(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid}).param_pmin(s, b);
}
std::optional<double> gen_param_gcost(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid}).param_gcost(s, b);
}
std::optional<double> gen_param_lossfactor(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid})
      .param_lossfactor(s, b);
}
std::optional<double> gen_param_heat_rate(const SystemContext& sc,
                                          Uid uid,
                                          StageUid s,
                                          BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid})
      .param_heat_rate(s, b);
}
std::optional<double> gen_param_emission_rate(const SystemContext& sc,
                                              Uid uid,
                                              StageUid s,
                                              BlockUid b)
{
  return sc.get_element(ObjectSingleId<GeneratorLP> {uid})
      .param_emission_rate(s, b);
}

// Fuel
std::optional<double> fuel_param_price(const SystemContext& sc,
                                       Uid uid,
                                       StageUid s,
                                       BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_price(s);
}
std::optional<double> fuel_param_heat_content(const SystemContext& sc,
                                              Uid uid,
                                              StageUid s,
                                              BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_heat_content(s);
}
std::optional<double> fuel_param_combustion_emission_factor(
    const SystemContext& sc, Uid uid, StageUid s, BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid})
      .param_combustion_emission_factor(s);
}
std::optional<double> fuel_param_upstream_emission_factor(
    const SystemContext& sc, Uid uid, StageUid s, BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid})
      .param_upstream_emission_factor(s);
}

// Demand
std::optional<double> dem_param_lmax(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<DemandLP> {uid}).param_lmax(s, b);
}
std::optional<double> dem_param_fcost(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<DemandLP> {uid}).param_fcost(s, b);
}
std::optional<double> dem_param_lossfactor(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid b)
{
  return sc.get_element(ObjectSingleId<DemandLP> {uid}).param_lossfactor(s, b);
}

// Line
std::optional<double> line_param_tmax_ab(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid b)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_tmax_ab(s, b);
}
std::optional<double> line_param_tmax_ba(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid b)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_tmax_ba(s, b);
}
std::optional<double> line_param_tcost(const SystemContext& sc,
                                       Uid uid,
                                       StageUid s,
                                       BlockUid b)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_tcost(s, b);
}
std::optional<double> line_param_reactance(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_reactance(s);
}
std::optional<double> line_param_voltage(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_voltage(s);
}
std::optional<double> line_param_tap_ratio(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_tap_ratio(s);
}
std::optional<double> line_param_phase_shift_deg(const SystemContext& sc,
                                                 Uid uid,
                                                 StageUid s,
                                                 BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_phase_shift_deg(s);
}
std::optional<double> line_param_lossfactor(const SystemContext& sc,
                                            Uid uid,
                                            StageUid s,
                                            BlockUid b)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_lossfactor(s, b);
}
std::optional<double> line_param_resistance(const SystemContext& sc,
                                            Uid uid,
                                            StageUid s,
                                            BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LineLP> {uid}).param_resistance(s);
}

// Battery
std::optional<double> bat_param_emin(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<BatteryLP> {uid}).param_emin(s, b);
}
std::optional<double> bat_param_emax(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<BatteryLP> {uid}).param_emax(s, b);
}
std::optional<double> bat_param_ecost(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<BatteryLP> {uid}).param_ecost(s, b);
}
std::optional<double> bat_param_input_efficiency(const SystemContext& sc,
                                                 Uid uid,
                                                 StageUid s,
                                                 BlockUid b)
{
  return sc.get_element(ObjectSingleId<BatteryLP> {uid})
      .param_input_efficiency(s, b);
}
std::optional<double> bat_param_output_efficiency(const SystemContext& sc,
                                                  Uid uid,
                                                  StageUid s,
                                                  BlockUid b)
{
  return sc.get_element(ObjectSingleId<BatteryLP> {uid})
      .param_output_efficiency(s, b);
}

// Reservoir
std::optional<double> res_param_emin(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<ReservoirLP> {uid}).param_emin(s, b);
}
std::optional<double> res_param_emax(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<ReservoirLP> {uid}).param_emax(s, b);
}
std::optional<double> res_param_ecost(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<ReservoirLP> {uid}).param_ecost(s, b);
}
std::optional<double> res_param_capacity(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<ReservoirLP> {uid}).param_capacity(s);
}

// FlowRight
std::optional<double> fr_param_fmin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<FlowRightLP> {uid}).param_fmin(s, b);
}
std::optional<double> fr_param_fmax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<FlowRightLP> {uid}).param_fmax(s, b);
}
std::optional<double> fr_param_target(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<FlowRightLP> {uid}).param_target(s, b);
}
std::optional<double> fr_param_fcost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<FlowRightLP> {uid}).param_fcost(s, b);
}
std::optional<double> fr_param_uvalue(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid b)
{
  return sc.get_element(ObjectSingleId<FlowRightLP> {uid}).param_uvalue(s, b);
}

// VolumeRight
std::optional<double> vr_param_fmax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid}).param_fmax(s, b);
}
std::optional<double> vr_param_emin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid}).param_emin(s, b);
}
std::optional<double> vr_param_emax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid}).param_emax(s, b);
}
std::optional<double> vr_param_demand(const SystemContext& sc,
                                      Uid uid,
                                      StageUid s,
                                      BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid}).param_demand(s);
}
std::optional<double> vr_param_saving_rate(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid b)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid})
      .param_saving_rate(s, b);
}
std::optional<double> vr_param_fail_cost(const SystemContext& sc,
                                         Uid uid,
                                         StageUid /*s*/,
                                         BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<VolumeRightLP> {uid}).param_fail_cost();
}

// EmissionZone
std::optional<double> ez_param_cap(const SystemContext& sc,
                                   Uid uid,
                                   StageUid s,
                                   BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<EmissionZoneLP> {uid}).param_cap(s);
}
std::optional<double> ez_param_cap_cost(const SystemContext& sc,
                                        Uid uid,
                                        StageUid s,
                                        BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<EmissionZoneLP> {uid}).param_cap_cost(s);
}
std::optional<double> ez_param_price(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<EmissionZoneLP> {uid}).param_price(s);
}

// EmissionSource
std::optional<double> es_param_rate(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<EmissionSourceLP> {uid}).param_rate(s);
}
std::optional<double> es_param_upstream_rate(const SystemContext& sc,
                                             Uid uid,
                                             StageUid s,
                                             BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<EmissionSourceLP> {uid})
      .param_upstream_rate(s);
}

/// Class iterator: walks `sc.elements<LP>()` and emits every element's
/// uid to the supplied captureless callback.  One instantiation per LP
/// class that participates in `sum(class(all)...)`.
template<class LP>
void iter_class(const SystemContext& sc, void* state, AmplIterCallback cb)
{
  for (const auto& el : sc.elements<LP>()) {
    cb(state, el.uid());
  }
}

}  // namespace

void register_ampl_param_dispatchers(SimulationLP& sim)
{
  constexpr auto generator_cls = Generator::class_name.snake_case();
  sim.register_ampl_param(generator_cls, "pmax", &gen_param_pmax);
  sim.register_ampl_param(generator_cls, "pmin", &gen_param_pmin);
  sim.register_ampl_param(generator_cls, "gcost", &gen_param_gcost);
  sim.register_ampl_param(generator_cls, "lossfactor", &gen_param_lossfactor);
  sim.register_ampl_param(generator_cls, "heat_rate", &gen_param_heat_rate);
  sim.register_ampl_param(
      generator_cls, "emission_rate", &gen_param_emission_rate);

  constexpr auto fuel_cls = Fuel::class_name.snake_case();
  sim.register_ampl_param(fuel_cls, "price", &fuel_param_price);
  sim.register_ampl_param(fuel_cls, "heat_content", &fuel_param_heat_content);
  sim.register_ampl_param(fuel_cls,
                          "combustion_emission_factor",
                          &fuel_param_combustion_emission_factor);
  sim.register_ampl_param(fuel_cls,
                          "upstream_emission_factor",
                          &fuel_param_upstream_emission_factor);

  constexpr auto demand_cls = Demand::class_name.snake_case();
  sim.register_ampl_param(demand_cls, "lmax", &dem_param_lmax);
  sim.register_ampl_param(demand_cls, "fcost", &dem_param_fcost);
  sim.register_ampl_param(demand_cls, "lossfactor", &dem_param_lossfactor);

  constexpr auto line_cls = Line::class_name.snake_case();
  sim.register_ampl_param(line_cls, "tmax_ab", &line_param_tmax_ab);
  sim.register_ampl_param(line_cls, "tmax_ba", &line_param_tmax_ba);
  sim.register_ampl_param(line_cls, "tcost", &line_param_tcost);
  sim.register_ampl_param(line_cls, "reactance", &line_param_reactance);
  sim.register_ampl_param(line_cls, "voltage", &line_param_voltage);
  sim.register_ampl_param(line_cls, "tap_ratio", &line_param_tap_ratio);
  sim.register_ampl_param(
      line_cls, "phase_shift_deg", &line_param_phase_shift_deg);
  sim.register_ampl_param(line_cls, "lossfactor", &line_param_lossfactor);
  sim.register_ampl_param(line_cls, "resistance", &line_param_resistance);

  constexpr auto battery_cls = Battery::class_name.snake_case();
  sim.register_ampl_param(battery_cls, "emin", &bat_param_emin);
  sim.register_ampl_param(battery_cls, "emax", &bat_param_emax);
  sim.register_ampl_param(battery_cls, "ecost", &bat_param_ecost);
  sim.register_ampl_param(
      battery_cls, "input_efficiency", &bat_param_input_efficiency);
  sim.register_ampl_param(
      battery_cls, "output_efficiency", &bat_param_output_efficiency);

  constexpr auto reservoir_cls = Reservoir::class_name.snake_case();
  sim.register_ampl_param(reservoir_cls, "emin", &res_param_emin);
  sim.register_ampl_param(reservoir_cls, "emax", &res_param_emax);
  sim.register_ampl_param(reservoir_cls, "ecost", &res_param_ecost);
  sim.register_ampl_param(reservoir_cls, "capacity", &res_param_capacity);

  constexpr auto flow_right_cls = FlowRight::class_name.snake_case();
  sim.register_ampl_param(flow_right_cls, "fmin", &fr_param_fmin);
  sim.register_ampl_param(flow_right_cls, "fmax", &fr_param_fmax);
  sim.register_ampl_param(flow_right_cls, "target", &fr_param_target);
  sim.register_ampl_param(flow_right_cls, "fcost", &fr_param_fcost);
  sim.register_ampl_param(flow_right_cls, "uvalue", &fr_param_uvalue);

  constexpr auto volume_right_cls = VolumeRight::class_name.snake_case();
  sim.register_ampl_param(volume_right_cls, "fmax", &vr_param_fmax);
  sim.register_ampl_param(volume_right_cls, "emin", &vr_param_emin);
  sim.register_ampl_param(volume_right_cls, "emax", &vr_param_emax);
  sim.register_ampl_param(volume_right_cls, "demand", &vr_param_demand);
  sim.register_ampl_param(
      volume_right_cls, "saving_rate", &vr_param_saving_rate);
  sim.register_ampl_param(volume_right_cls, "fail_cost", &vr_param_fail_cost);

  constexpr auto emission_zone_cls = EmissionZone::class_name.snake_case();
  sim.register_ampl_param(emission_zone_cls, "cap", &ez_param_cap);
  sim.register_ampl_param(emission_zone_cls, "cap_cost", &ez_param_cap_cost);
  sim.register_ampl_param(emission_zone_cls, "price", &ez_param_price);

  constexpr auto emission_source_cls = EmissionSource::class_name.snake_case();
  sim.register_ampl_param(emission_source_cls, "rate", &es_param_rate);
  sim.register_ampl_param(
      emission_source_cls, "upstream_rate", &es_param_upstream_rate);
}

void register_ampl_iterator_dispatchers(SimulationLP& sim)
{
  sim.register_ampl_iter(Generator::class_name.snake_case(),
                         &iter_class<GeneratorLP>);
  sim.register_ampl_iter(Demand::class_name.snake_case(),
                         &iter_class<DemandLP>);
  sim.register_ampl_iter(Line::class_name.snake_case(), &iter_class<LineLP>);
  sim.register_ampl_iter(Battery::class_name.snake_case(),
                         &iter_class<BatteryLP>);
  sim.register_ampl_iter(Reservoir::class_name.snake_case(),
                         &iter_class<ReservoirLP>);
  sim.register_ampl_iter(Waterway::class_name.snake_case(),
                         &iter_class<WaterwayLP>);
  sim.register_ampl_iter(Turbine::class_name.snake_case(),
                         &iter_class<TurbineLP>);
  sim.register_ampl_iter(Converter::class_name.snake_case(),
                         &iter_class<ConverterLP>);
  sim.register_ampl_iter(Junction::class_name.snake_case(),
                         &iter_class<JunctionLP>);
  sim.register_ampl_iter(Flow::class_name.snake_case(), &iter_class<FlowLP>);
  sim.register_ampl_iter(FlowRight::class_name.snake_case(),
                         &iter_class<FlowRightLP>);
  sim.register_ampl_iter(VolumeRight::class_name.snake_case(),
                         &iter_class<VolumeRightLP>);
  sim.register_ampl_iter(ReservoirSeepageLP::SeepageName,
                         &iter_class<ReservoirSeepageLP>);
  sim.register_ampl_iter(ReserveProvision::class_name.snake_case(),
                         &iter_class<ReserveProvisionLP>);
  sim.register_ampl_iter(ReserveZone::class_name.snake_case(),
                         &iter_class<ReserveZoneLP>);
  sim.register_ampl_iter(Bus::class_name.snake_case(), &iter_class<BusLP>);
  sim.register_ampl_iter(LngTerminal::class_name.snake_case(),
                         &iter_class<LngTerminalLP>);
  sim.register_ampl_iter(Fuel::class_name.snake_case(), &iter_class<FuelLP>);
  sim.register_ampl_iter(EmissionZone::class_name.snake_case(),
                         &iter_class<EmissionZoneLP>);
  sim.register_ampl_iter(EmissionSource::class_name.snake_case(),
                         &iter_class<EmissionSourceLP>);
  // Commitment family: Commitment exposes status/startup/shutdown PAMPL
  // variables, SimpleCommitment exposes status — both have to be iterable
  // for `sum(commitment(all).status)` / `sum(simple_commitment(all).status)`.
  sim.register_ampl_iter(Commitment::class_name.snake_case(),
                         &iter_class<CommitmentLP>);
  sim.register_ampl_iter(SimpleCommitment::class_name.snake_case(),
                         &iter_class<SimpleCommitmentLP>);
  // Inertia: zone-level requirement column + per-generator provision
  // columns; both register AMPL variables on every (scenario, stage).
  sim.register_ampl_iter(InertiaZone::class_name.snake_case(),
                         &iter_class<InertiaZoneLP>);
  sim.register_ampl_iter(InertiaProvision::class_name.snake_case(),
                         &iter_class<InertiaProvisionLP>);
  // Free continuous decision variable referenced via
  // `decision_variable("X").value` from PAMPL constraints.
  sim.register_ampl_iter(DecisionVariable::class_name.snake_case(),
                         &iter_class<DecisionVariableLP>);
}

}  // namespace gtopt
