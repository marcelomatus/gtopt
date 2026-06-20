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

#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/ammonia_node_lp.hpp>
#include <gtopt/ammonia_storage_lp.hpp>
#include <gtopt/ampl_dispatch_registry.hpp>
#include <gtopt/ampl_variable.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/carrier_converter_lp.hpp>
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
#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/hydrogen_storage_lp.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/thermal_node_lp.hpp>
#include <gtopt/thermal_storage_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/user_constraint_lp.hpp>
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
                                       BlockUid b)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_price(s, b);
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

// Turbine — per-stage schedules (production_factor, efficiency, capacity).
// Exposed via PAMPL so user constraints can read the conversion factors
// and flow cap as constants in `turbine('X').<field>` expressions.
std::optional<double> tur_param_production_factor(const SystemContext& sc,
                                                  Uid uid,
                                                  StageUid s,
                                                  BlockUid b)
{
  return sc.get_element(ObjectSingleId<TurbineLP> {uid})
      .param_production_factor(s, b);
}
std::optional<double> tur_param_efficiency(const SystemContext& sc,
                                           Uid uid,
                                           StageUid s,
                                           BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<TurbineLP> {uid}).param_efficiency(s);
}
std::optional<double> tur_param_capacity(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid b)
{
  return sc.get_element(ObjectSingleId<TurbineLP> {uid}).param_capacity(s, b);
}

// Waterway — per-(stage, block) fmin/fmax + per-stage capacity, lossfactor,
// fcost.  Exposed via PAMPL so user constraints can reference the channel's
// bound and cost schedules in ``waterway('X').<field>`` expressions.
std::optional<double> ww_param_fmin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<WaterwayLP> {uid}).param_fmin(s, b);
}
std::optional<double> ww_param_fmax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<WaterwayLP> {uid}).param_fmax(s, b);
}
std::optional<double> ww_param_capacity(const SystemContext& sc,
                                        Uid uid,
                                        StageUid s,
                                        BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<WaterwayLP> {uid}).param_capacity(s);
}
std::optional<double> ww_param_lossfactor(const SystemContext& sc,
                                          Uid uid,
                                          StageUid s,
                                          BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<WaterwayLP> {uid}).param_lossfactor(s);
}
std::optional<double> ww_param_fcost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<WaterwayLP> {uid}).param_fcost(s);
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

// UserConstraint
std::optional<double> uc_param_rhs(const SystemContext& sc,
                                   Uid uid,
                                   StageUid s,
                                   BlockUid b)
{
  return sc.get_element(ObjectSingleId<UserConstraintLP> {uid}).param_rhs(s, b);
}

// Fuel — max_offtake row (PR #487 / #492).  These are the
// stage-level cap parameters used by ``FuelLP::add_to_lp`` when
// building the per-(scenario, stage) row that bounds total
// heat-rate-weighted dispatch on every generator referencing the
// fuel.  Exposed via PAMPL so user constraints can read the cap
// (e.g. for compliance-side audit constraints).
std::optional<double> fuel_param_max_offtake(const SystemContext& sc,
                                             Uid uid,
                                             StageUid s,
                                             BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_max_offtake(s);
}
std::optional<double> fuel_param_max_offtake_cost(const SystemContext& sc,
                                                  Uid uid,
                                                  StageUid s,
                                                  BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_max_offtake_cost(s);
}
std::optional<double> fuel_param_min_offtake(const SystemContext& sc,
                                             Uid uid,
                                             StageUid s,
                                             BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_min_offtake(s);
}
std::optional<double> fuel_param_min_offtake_cost(const SystemContext& sc,
                                                  Uid uid,
                                                  StageUid s,
                                                  BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<FuelLP> {uid}).param_min_offtake_cost(s);
}

// LngTerminal — delivery (m³/stage).  Per-stage scheduled LNG
// arrival; the LP per-block inflow rate is ``delivery /
// stage_duration``.  Exposed via PAMPL so user constraints can
// reference the scheduled delivery in compliance / aggregation
// rules.
std::optional<double> lng_param_delivery(const SystemContext& sc,
                                         Uid uid,
                                         StageUid s,
                                         BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<LngTerminalLP> {uid}).param_delivery(s);
}

// Storage peers (Battery / Reservoir already covered above) —
// ThermalStorage / HydrogenStorage / AmmoniaStorage all derive
// emin/emax/ecost from the StorageLP base + add
// input_efficiency / output_efficiency on the Battery pattern.
std::optional<double> ts_param_emin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<ThermalStorageLP> {uid})
      .param_emin(s, b);
}
std::optional<double> ts_param_emax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<ThermalStorageLP> {uid})
      .param_emax(s, b);
}
std::optional<double> ts_param_ecost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<ThermalStorageLP> {uid})
      .param_ecost(s, b);
}
std::optional<double> ts_param_input_efficiency(const SystemContext& sc,
                                                Uid uid,
                                                StageUid s,
                                                BlockUid b)
{
  return sc.get_element(ObjectSingleId<ThermalStorageLP> {uid})
      .param_input_efficiency(s, b);
}
std::optional<double> ts_param_output_efficiency(const SystemContext& sc,
                                                 Uid uid,
                                                 StageUid s,
                                                 BlockUid b)
{
  return sc.get_element(ObjectSingleId<ThermalStorageLP> {uid})
      .param_output_efficiency(s, b);
}

std::optional<double> hs_param_emin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<HydrogenStorageLP> {uid})
      .param_emin(s, b);
}
std::optional<double> hs_param_emax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<HydrogenStorageLP> {uid})
      .param_emax(s, b);
}
std::optional<double> hs_param_ecost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<HydrogenStorageLP> {uid})
      .param_ecost(s, b);
}
std::optional<double> hs_param_input_efficiency(const SystemContext& sc,
                                                Uid uid,
                                                StageUid s,
                                                BlockUid b)
{
  return sc.get_element(ObjectSingleId<HydrogenStorageLP> {uid})
      .param_input_efficiency(s, b);
}
std::optional<double> hs_param_output_efficiency(const SystemContext& sc,
                                                 Uid uid,
                                                 StageUid s,
                                                 BlockUid b)
{
  return sc.get_element(ObjectSingleId<HydrogenStorageLP> {uid})
      .param_output_efficiency(s, b);
}

std::optional<double> as_param_emin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<AmmoniaStorageLP> {uid})
      .param_emin(s, b);
}
std::optional<double> as_param_emax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<AmmoniaStorageLP> {uid})
      .param_emax(s, b);
}
std::optional<double> as_param_ecost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<AmmoniaStorageLP> {uid})
      .param_ecost(s, b);
}
std::optional<double> as_param_input_efficiency(const SystemContext& sc,
                                                Uid uid,
                                                StageUid s,
                                                BlockUid b)
{
  return sc.get_element(ObjectSingleId<AmmoniaStorageLP> {uid})
      .param_input_efficiency(s, b);
}
std::optional<double> as_param_output_efficiency(const SystemContext& sc,
                                                 Uid uid,
                                                 StageUid s,
                                                 BlockUid b)
{
  return sc.get_element(ObjectSingleId<AmmoniaStorageLP> {uid})
      .param_output_efficiency(s, b);
}

// CarrierConverter — multi-carrier converter (PR #485).
std::optional<double> cc_param_efficiency(const SystemContext& sc,
                                          Uid uid,
                                          StageUid s,
                                          BlockUid b)
{
  return sc.get_element(ObjectSingleId<CarrierConverterLP> {uid})
      .param_efficiency(s, b);
}
std::optional<double> cc_param_ocost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<CarrierConverterLP> {uid})
      .param_ocost(s, b);
}

// AllowancePool — CO₂ cap-and-trade pool (PR #495 / #496).
// Inherits emin/emax/ecost from StorageLP base; exposes delivery
// (free allocation per stage) on its own accessor.
std::optional<double> ap_param_emin(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<AllowancePoolLP> {uid}).param_emin(s, b);
}
std::optional<double> ap_param_emax(const SystemContext& sc,
                                    Uid uid,
                                    StageUid s,
                                    BlockUid b)
{
  return sc.get_element(ObjectSingleId<AllowancePoolLP> {uid}).param_emax(s, b);
}
std::optional<double> ap_param_ecost(const SystemContext& sc,
                                     Uid uid,
                                     StageUid s,
                                     BlockUid b)
{
  return sc.get_element(ObjectSingleId<AllowancePoolLP> {uid})
      .param_ecost(s, b);
}
std::optional<double> ap_param_delivery(const SystemContext& sc,
                                        Uid uid,
                                        StageUid s,
                                        BlockUid /*b*/)
{
  return sc.get_element(ObjectSingleId<AllowancePoolLP> {uid})
      .param_delivery(s);
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

  constexpr auto turbine_cls = Turbine::class_name.snake_case();
  sim.register_ampl_param(
      turbine_cls, "production_factor", &tur_param_production_factor);
  sim.register_ampl_param(turbine_cls, "efficiency", &tur_param_efficiency);
  sim.register_ampl_param(turbine_cls, "capacity", &tur_param_capacity);

  constexpr auto waterway_cls = Waterway::class_name.snake_case();
  sim.register_ampl_param(waterway_cls, "fmin", &ww_param_fmin);
  sim.register_ampl_param(waterway_cls, "fmax", &ww_param_fmax);
  sim.register_ampl_param(waterway_cls, "capacity", &ww_param_capacity);
  sim.register_ampl_param(waterway_cls, "lossfactor", &ww_param_lossfactor);
  sim.register_ampl_param(waterway_cls, "fcost", &ww_param_fcost);

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

  // UserConstraint: per-(stage, block) RHS override accessible from
  // other constraints' expressions as `user_constraint("X").rhs`.
  constexpr auto user_constraint_cls = UserConstraint::class_name.snake_case();
  sim.register_ampl_param(user_constraint_cls, "rhs", &uc_param_rhs);

  // Fuel — max_offtake cap row (PR #487 / #492).
  sim.register_ampl_param(fuel_cls, "max_offtake", &fuel_param_max_offtake);
  sim.register_ampl_param(
      fuel_cls, "max_offtake_cost", &fuel_param_max_offtake_cost);
  // Fuel — min_offtake floor row (PLEXOS take-or-pay; pids 595-602).
  // Symmetric to the max-side params above; PAMPL exposes both so
  // compliance / audit UCs can reference the floor identically.
  sim.register_ampl_param(fuel_cls, "min_offtake", &fuel_param_min_offtake);
  sim.register_ampl_param(
      fuel_cls, "min_offtake_cost", &fuel_param_min_offtake_cost);

  // LngTerminal — scheduled LNG delivery per stage.
  constexpr auto lng_terminal_cls = LngTerminal::class_name.snake_case();
  sim.register_ampl_param(lng_terminal_cls, "delivery", &lng_param_delivery);

  // ThermalStorage — molten-salt TES (PR #483).
  constexpr auto thermal_storage_cls = ThermalStorage::class_name.snake_case();
  sim.register_ampl_param(thermal_storage_cls, "emin", &ts_param_emin);
  sim.register_ampl_param(thermal_storage_cls, "emax", &ts_param_emax);
  sim.register_ampl_param(thermal_storage_cls, "ecost", &ts_param_ecost);
  sim.register_ampl_param(
      thermal_storage_cls, "input_efficiency", &ts_param_input_efficiency);
  sim.register_ampl_param(
      thermal_storage_cls, "output_efficiency", &ts_param_output_efficiency);

  // HydrogenStorage — salt cavern / LH₂ / LOHC (PR #483).
  constexpr auto hydrogen_storage_cls =
      HydrogenStorage::class_name.snake_case();
  sim.register_ampl_param(hydrogen_storage_cls, "emin", &hs_param_emin);
  sim.register_ampl_param(hydrogen_storage_cls, "emax", &hs_param_emax);
  sim.register_ampl_param(hydrogen_storage_cls, "ecost", &hs_param_ecost);
  sim.register_ampl_param(
      hydrogen_storage_cls, "input_efficiency", &hs_param_input_efficiency);
  sim.register_ampl_param(
      hydrogen_storage_cls, "output_efficiency", &hs_param_output_efficiency);

  // AmmoniaStorage — refrigerated NH₃ tank (PR #483).
  constexpr auto ammonia_storage_cls = AmmoniaStorage::class_name.snake_case();
  sim.register_ampl_param(ammonia_storage_cls, "emin", &as_param_emin);
  sim.register_ampl_param(ammonia_storage_cls, "emax", &as_param_emax);
  sim.register_ampl_param(ammonia_storage_cls, "ecost", &as_param_ecost);
  sim.register_ampl_param(
      ammonia_storage_cls, "input_efficiency", &as_param_input_efficiency);
  sim.register_ampl_param(
      ammonia_storage_cls, "output_efficiency", &as_param_output_efficiency);

  // CarrierConverter — multi-carrier converter (PR #485).
  constexpr auto carrier_converter_cls =
      CarrierConverter::class_name.snake_case();
  sim.register_ampl_param(
      carrier_converter_cls, "efficiency", &cc_param_efficiency);
  sim.register_ampl_param(carrier_converter_cls, "ocost", &cc_param_ocost);

  // AllowancePool — CO₂ cap-and-trade pool (PR #495 / #496).
  constexpr auto allowance_pool_cls = AllowancePool::class_name.snake_case();
  sim.register_ampl_param(allowance_pool_cls, "emin", &ap_param_emin);
  sim.register_ampl_param(allowance_pool_cls, "emax", &ap_param_emax);
  sim.register_ampl_param(allowance_pool_cls, "ecost", &ap_param_ecost);
  sim.register_ampl_param(allowance_pool_cls, "delivery", &ap_param_delivery);
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
  // Unlocks ``sum(reservoir_discharge_limit(all).…)`` enumeration
  // patterns over the volume-dependent discharge caps.
  sim.register_ampl_iter(ReservoirDischargeLimit::class_name.snake_case(),
                         &iter_class<ReservoirDischargeLimitLP>);
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
  // UserConstraint: enables `sum(user_constraint(all).rhs)` and other
  // class-level iteration patterns over user constraints.
  sim.register_ampl_iter(UserConstraint::class_name.snake_case(),
                         &iter_class<UserConstraintLP>);

  // Multi-carrier element family (PR #483 / #484 / #485) — typed
  // balance nodes + their storage peers + the cross-carrier
  // converter.  Each gets ``sum(<class>(all).<attr>)`` support
  // via class-level iteration.
  sim.register_ampl_iter(ThermalNode::class_name.snake_case(),
                         &iter_class<ThermalNodeLP>);
  sim.register_ampl_iter(ThermalStorage::class_name.snake_case(),
                         &iter_class<ThermalStorageLP>);
  sim.register_ampl_iter(HydrogenNode::class_name.snake_case(),
                         &iter_class<HydrogenNodeLP>);
  sim.register_ampl_iter(HydrogenStorage::class_name.snake_case(),
                         &iter_class<HydrogenStorageLP>);
  sim.register_ampl_iter(AmmoniaNode::class_name.snake_case(),
                         &iter_class<AmmoniaNodeLP>);
  sim.register_ampl_iter(AmmoniaStorage::class_name.snake_case(),
                         &iter_class<AmmoniaStorageLP>);
  sim.register_ampl_iter(CarrierConverter::class_name.snake_case(),
                         &iter_class<CarrierConverterLP>);

  // AllowancePool — CO₂ cap-and-trade pool (PR #495 / #496).
  sim.register_ampl_iter(AllowancePool::class_name.snake_case(),
                         &iter_class<AllowancePoolLP>);
}

}  // namespace gtopt
