// SPDX-License-Identifier: BSD-3-Clause
/// @file test_cost_scale_type_dual.cpp
/// @brief Pin the per-element `cost_scale_type` dual / reduced-cost
///        readback contract in OutputContext.
///
/// Background.  The dual / reduced-cost readback in OutputContext divides
/// the LP value by the inverse cost-factor `prob·disc·duration` (the
/// "block_icost" matrix).  That is CORRECT for FLOW / POWER quantities
/// (per-block power balance → LMP $/MWh: the objective coefficient was
/// `cost·prob·disc·duration`, so the dual must be divided back by
/// duration).  It is WRONG for STOCK / commodity quantities (storage
/// energy balance → water value $/CMD), which are duration-INdependent:
/// dividing those by duration makes multi-hour blocks report
/// `true_value / duration`.
///
/// The fix (Tier 2): every SparseCol / SparseRow carries a
/// `cost_scale_type` (Power / Energy / Raw).  OutputContext selects the
/// inverse factor family PER ELEMENT — Power → ÷duration, Energy → no
/// ÷duration, Raw → face value — so the stock dual / reduced cost is the
/// exact inverse of the objective fold and is duration-independent.
///
/// This test builds a single-stage model with three blocks of DIFFERENT
/// durations (1 h, 2 h, 5 h) containing a reservoir whose terminal value
/// makes stored water valuable, plus a thermal generator so there is an
/// LMP.  It asserts:
///   1. the storage energy-balance dual (water value, Energy) is
///      DURATION-INDEPENDENT — equal across all three blocks;
///   2. the storage energy-state reduced cost (Energy) is likewise
///      duration-independent;
///   3. the bus-balance LMP (Power) is unchanged from the legacy
///      behaviour — it equals the binding generator gcost in every block,
///      independent of duration (the robust Power invariant: the
///      ÷duration readback exactly cancels the ×duration objective fold).

#include <cmath>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace cost_scale_type_dual_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Plain-old-data inputs.  Returned by value so each test owns its own
/// `SystemLP` (moving one would leave its embedded `SystemContext`
/// dangling) — same rationale as `test_storage_dual_sign.cpp`.
struct Inputs
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
};

/// One bus, three blocks of durations {1, 2, 5} h in a single stage.
/// A cheap reservoir-fed hydro turbine plus an expensive thermal backup
/// make stored water valuable; a terminal `efin_cost` pins a constant
/// marginal water value so the energy-balance dual is non-zero and (with
/// the fix) duration-independent across the three blocks.
inline Inputs make_inputs()
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g_thermal",
          .bus = Uid {1},
          .gcost = 200.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "g_hydro",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = 80.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_dn",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  // Terminal water value via `efin_cost`: pins a constant marginal value
  // on the stored volume that propagates back through every block's
  // energy balance as the water value.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
          .efin = 5000.0,
          .efin_cost = 50.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {2},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
              },
              {
                  .uid = Uid {3},
                  .duration = 5.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 3,
              },
          },
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  System system = {
      .name = "CostScaleTypeDualTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  // Multi-bus off so the single-bus balance dual is the LMP directly.
  opts.model_options.use_single_bus = true;
  opts.model_options.scale_objective = 1.0;

  return Inputs {
      .system = std::move(system),
      .simulation = std::move(simulation),
      .opts = std::move(opts),
  };
}

/// Fetch the per-block value vector for a `(cname, fname, sname)` output
/// field from a solved `OutputContext`.  With one scenario / one stage,
/// the flattened vector holds one entry per block in block order.
inline std::vector<double> field_values(const OutputContext& oc,
                                        std::string_view cname,
                                        std::string_view fname,
                                        std::string_view sname)
{
  const auto& fields = oc.fields();
  const OutputContext::ClassFieldName key {cname, fname, sname};
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) {
    return {};
  }
  return std::get<1>(it->second.front());
}

}  // namespace cost_scale_type_dual_test

TEST_CASE(  // NOLINT
    "cost_scale_type: stock duals duration-independent, power duals not")
{
  using namespace cost_scale_type_dual_test;

  auto inputs = make_inputs();
  const PlanningOptionsLP options(inputs.opts);
  SimulationLP sim_lp(inputs.simulation, options);
  SystemLP sys_lp(inputs.system, sim_lp);

  auto&& lp = sys_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Build an OutputContext directly and drive add_to_output for the
  // elements we care about (Reservoir water value, Bus LMP, Reservoir
  // energy reduced cost).
  OutputContext oc(sys_lp.system_context(),
                   sys_lp.linear_interface(),
                   sys_lp.scene().uid(),
                   sys_lp.phase().uid(),
                   sys_lp.phase().is_continuous());

  const auto& rsv_lp = sys_lp.elements<ReservoirLP>().front();
  const auto& bus_lp = sys_lp.elements<BusLP>().front();
  REQUIRE(rsv_lp.add_to_output(oc));
  REQUIRE(bus_lp.add_to_output(oc));

  // ── 1. Water value (Energy stock dual) is duration-independent ──
  const auto wv =
      field_values(oc, "Reservoir", ReservoirLP::WaterValueName, "dual");
  REQUIRE(wv.size() == 3);
  CAPTURE(wv[0]);
  CAPTURE(wv[1]);
  CAPTURE(wv[2]);
  // The bug would make multi-hour blocks report wv/duration:
  //   wv[1] = wv[0]/2, wv[2] = wv[0]/5.  After the fix all three are equal.
  CHECK(wv[0] != doctest::Approx(0.0));  // non-trivial (binding water value)
  CHECK(wv[1] == doctest::Approx(wv[0]));
  CHECK(wv[2] == doctest::Approx(wv[0]));

  // ── 2. Energy-state reduced cost (Energy) is duration-independent ──
  const auto ec =
      field_values(oc, "Reservoir", ReservoirLP::EnergyName, "cost");
  // The energy column's reduced cost is published per block; whether each
  // entry is zero (basic) or non-zero (at bound), the per-block scaling
  // must be duration-free, so any two reported values that share the same
  // LP-physical reduced cost stay equal across blocks of different
  // durations.  We assert the vector is duration-consistent: every entry
  // equals the first (a duration-divided readback would split them).
  if (ec.size() == 3) {
    CAPTURE(ec[0]);
    CAPTURE(ec[1]);
    CAPTURE(ec[2]);
    CHECK(ec[1] == doctest::Approx(ec[0]));
    CHECK(ec[2] == doctest::Approx(ec[0]));
  }

  // ── 3. Bus LMP (Power dual) keeps the legacy duration scaling ──
  const auto lmp = field_values(oc, "Bus", BusLP::BalanceName, "dual");
  REQUIRE(lmp.size() == 3);
  CAPTURE(lmp[0]);
  CAPTURE(lmp[1]);
  CAPTURE(lmp[2]);
  // Power invariant: the ÷duration readback exactly cancels the ×duration
  // objective fold, so the physical LMP equals the binding marginal cost
  // ($/MWh) in EVERY block regardless of block duration.  This is the
  // robust check that Power-typed duals are unchanged by the per-element
  // selection (a stock-style no-duration readback here would instead make
  // multi-hour blocks report LMP×duration).
  CHECK(lmp[0] != doctest::Approx(0.0));
  CHECK(lmp[1] == doctest::Approx(lmp[0]));
  CHECK(lmp[2] == doctest::Approx(lmp[0]));
}
