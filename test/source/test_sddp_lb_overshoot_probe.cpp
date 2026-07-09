// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_lb_overshoot_probe.cpp
 * @brief     Probe for the juan SDDP LB-overshoot regression at small scale
 * @date      2026-05-01
 *
 * Synthetic reproducer for the bug observed on
 * `support/juan/gtopt_iplp` where the SDDP lower bound compounds
 * ~10× per iteration (iter 0 LB=$1.4M → iter 1 LB=$1.1B vs UB=$153M)
 * — reported as "LB > UB violates SDDP theory; cuts likely overshoot
 * the optimum".
 *
 * Strategy: scale up the existing 2-phase 2-scenario hydrothermal
 * fixture from `sddp_helpers.hpp` until the LB-overshoot signature
 * appears, then we have a sub-minute reproducer to iterate fixes
 * against.  The known-good baseline ("SDDPMethod - 2-phase with
 * apertures converges") passes on master; this file probes
 * progressively larger configurations.
 *
 * Each probe runs SDDP for up to `max_iterations` iterations and
 * reports per-iter UB/LB so the reader can spot the compound-ratio
 * pattern.  The CHECK is loud-but-non-fatal so a single
 * configuration's overshoot does not abort the rest of the probe.
 */

#include <chrono>
#include <print>

#include <doctest/doctest.h>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"  // IWYU pragma: keep — fixture helpers used in build_nphase_mscenario_planning

using namespace gtopt;

namespace
{

/// Replicate the 2-phase fixture for `n_phases` consecutive stages.
/// Each stage gets `blocks_per_phase` blocks of 1 h.  Reservoir
/// dynamics carry across phases via the standard state-variable
/// mechanism.
///
/// `n_scenes`: when > 1, partitions the scenarios into N scenes
/// (one scenario per scene).  Distinct scenes drive the cut-sharing
/// codepaths in `sddp_cut_sharing.cpp` — heterogeneous data per
/// scene means non-`none` cut_sharing modes are KNOWN INVALID and
/// expected to produce LB > UB.  When `n_scenes <= 1` the
/// `scene_array` is left empty (default single-scene behaviour).
auto build_nphase_mscenario_planning(int n_phases,
                                     int n_scenarios,
                                     int n_scenes = 1,
                                     int blocks_per_phase = 4,
                                     bool unequal_probabilities = false)
    -> Planning
{
  using gtopt::test_fixtures::make_single_stage_phases;
  using gtopt::test_fixtures::make_uniform_blocks;
  using gtopt::test_fixtures::make_uniform_stages;

  const int total_blocks = n_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(n_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(n_phases));

  Array<Scenario> scenario_array;
  scenario_array.reserve(static_cast<std::size_t>(n_scenarios));
  // With unequal probabilities the three "INVALID" cut_sharing modes
  // produce arithmetically different cuts than `none`; with equal
  // probabilities they degenerate to identical cuts and the
  // pathology stays hidden.  Mirrors `make_2scene_3phase_hydro_planning`'s
  // 0.7/0.3 split used in the existing bounds-sanity regression test.
  for (int s = 1; s <= n_scenarios; ++s) {
    double prob = 1.0 / static_cast<double>(n_scenarios);
    if (unequal_probabilities && n_scenarios == 2) {
      prob = (s == 1) ? 0.7 : 0.3;
    } else if (unequal_probabilities && n_scenarios > 2) {
      // Geometric weighting: scenario s gets weight ∝ (n+1-s) so the
      // first scenarios dominate.  Re-normalised to sum=1.
      double sum = 0.0;
      for (int k = 1; k <= n_scenarios; ++k) {
        sum += static_cast<double>(n_scenarios + 1 - k);
      }
      prob = static_cast<double>(n_scenarios + 1 - s) / sum;
    }
    scenario_array.push_back(Scenario {
        .uid = Uid {s},
        .probability_factor = OptReal {prob},
    });
  }

  Array<Scene> scene_array;
  if (n_scenes > 1) {
    scene_array.reserve(static_cast<std::size_t>(n_scenes));
    // Partition scenarios across scenes.  When n_scenes ==
    // n_scenarios we produce one scene per scenario (the
    // heterogeneous-scene configuration that triggers the
    // cut_sharing pathology).  When n_scenes < n_scenarios the
    // remainder lands on the last scene.
    const auto per_scene =
        static_cast<Size>(std::max(1, n_scenarios / std::max(1, n_scenes)));
    for (int sc = 0; sc < n_scenes; ++sc) {
      const auto first = static_cast<Size>(sc) * per_scene;
      Size count = per_scene;
      if (sc == n_scenes - 1) {
        count = static_cast<Size>(n_scenarios) - first;
      }
      scene_array.push_back(Scene {
          .uid = Uid {sc + 1},
          .name = std::string {"scene"} + std::to_string(sc + 1),
          .active = true,
          .first_scenario = first,
          .count_scenario = count,
      });
    }
  }

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = std::move(scenario_array),
      .phase_array = std::move(phase_array),
      .scene_array = std::move(scene_array),
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 20.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 30.0,
      },
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 50.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  // SHARED Flow across all scenarios (matches the layout used by
  // `make_2scene_3phase_hydro_planning` in
  // `test_sddp_bounds_sanity.cpp`, which is the existing regression
  // test that documents the cut_sharing pathology).  Heterogeneity
  // among scenes / scenarios comes from `probability_factor` only,
  // not from per-scenario inflow data.  Per-scenario flow arrays
  // (one Flow per scenario) made the cut_sharing modes degenerate
  // to the same answer at this small scale and hid the regression.
  Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 5.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1000.0};  // juan default
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  // Mimic juan's per-Reservoir variable_scales=10 (the col_scale=10 +
  // ruiz interaction the user fingered as the bug surface).
  options.variable_scales = std::vector<VariableScale> {
      VariableScale {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = Uid {1},
          .scale = 10.0,
      },
  };

  System system = {
      .name = "lb_overshoot_probe",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = std::move(flow_array),
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Configuration for one probe run.
struct ProbeConfig
{
  int n_phases {2};
  int n_scenarios {2};
  int n_scenes {1};
  int max_iterations {15};
  bool with_apertures {true};
  bool unequal_probabilities {false};
  CutSharingMode cut_sharing {CutSharingMode::none};
};

/// One probe run: print per-iter UB/LB and return the worst (max)
/// LB/UB ratio seen across all iterations.  Ratio > 1 ⇒ overshoot.
double run_probe(const ProbeConfig& cfg)
{
  auto planning = build_nphase_mscenario_planning(cfg.n_phases,
                                                  cfg.n_scenarios,
                                                  cfg.n_scenes,
                                                  /*blocks_per_phase=*/4,
                                                  cfg.unequal_probabilities);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = cfg.max_iterations;
  sddp_opts.convergence_tol = 1e-3;
  sddp_opts.enable_api = false;
  sddp_opts.cut_sharing = cfg.cut_sharing;
  if (cfg.with_apertures) {
    sddp_opts.apertures = std::nullopt;  // use per-phase apertures
  } else {
    sddp_opts.apertures = std::vector<Uid> {};  // disable
  }

  const auto t0 = std::chrono::steady_clock::now();
  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  const auto dt =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  REQUIRE(results.has_value());

  std::println(
      "─── probe: phases={} scenarios={} scenes={} probs={} "
      "apertures={} cut_sharing={} max_iter={} ({:.2f}s) ───",
      cfg.n_phases,
      cfg.n_scenarios,
      cfg.n_scenes,
      cfg.unequal_probabilities ? "uneq" : "uniform",
      cfg.with_apertures ? "yes" : "no",
      enum_name(cfg.cut_sharing),
      cfg.max_iterations,
      dt);

  double worst_ratio = 0.0;
  int idx = 0;
  for (const auto& r : *results) {
    const double ratio =
        (r.upper_bound > 0.0) ? r.lower_bound / r.upper_bound : 0.0;
    worst_ratio = std::max(worst_ratio, ratio);
    std::string_view tag;
    if (ratio > 1.0 + 1e-6) {
      tag = "  ← OVERSHOOT";
    } else if (r.converged) {
      tag = "  [CONVERGED]";
    }
    std::println(
        "  iter {:2d}: UB={:14.4f}  LB={:14.4f}  "
        "gap={:+.4f}  ratio LB/UB={:.4f}{}",
        idx,
        r.upper_bound,
        r.lower_bound,
        r.gap,
        ratio,
        tag);
    ++idx;
  }
  return worst_ratio;
}

}  // namespace

TEST_CASE("SDDPMethod - LB-overshoot probe @ tiny scales")  // NOLINT
{
  // Baseline: known-good 2-phase 2-scenario fixture should converge
  // without LB-overshoot.  If THIS subcase fails, the bug is at
  // the smallest scale and our existing test_sddp_method.cpp
  // baseline would have caught it.
  SUBCASE("2 phases × 2 scenarios — known good")
  {
    const auto worst = run_probe(ProbeConfig {
        .n_phases = 2,
        .n_scenarios = 2,
        .max_iterations = 10,
    });
    CHECK(worst <= 1.0 + 1e-6);
  }

  SUBCASE("3 phases × 2 scenarios")
  {
    const auto worst = run_probe(ProbeConfig {
        .n_phases = 3,
        .n_scenarios = 2,
        .max_iterations = 15,
    });
    CHECK_MESSAGE(worst <= 1.0 + 1e-6,
                  "LB-overshoot at 3 phases / 2 scenarios — bug "
                  "appears earlier than juan scale");
  }

  SUBCASE("5 phases × 2 scenarios")
  {
    const auto worst = run_probe(ProbeConfig {
        .n_phases = 5,
        .n_scenarios = 2,
        .max_iterations = 20,
    });
    CHECK_MESSAGE(worst <= 1.0 + 1e-6,
                  "LB-overshoot at 5 phases / 2 scenarios");
  }
}

// Multi-scene + cut_sharing probe.  juan/gtopt_iplp runs with 16
// scenes (one per scenario family).  The invalid broadcast modes
// (accumulate/broadcast_mean/max) were REMOVED 2026-07-08; what
// remains to probe here is `multicut` under NON-uniform scene
// probabilities — the theorem-M3 uncertified configuration
// (`docs/formulation/sddp-cut-validity.md` §8).  This subcase set
// verifies (a) that the heterogeneous-scene configuration completes
// at all, and (b) documents the multicut/non-uniform LB behaviour
// WARN-only (LB > UB there is a process mismatch and/or M3 pricing,
// not a regression).
TEST_CASE("SDDPMethod - LB-overshoot probe @ multi-scene")  // NOLINT
{
  // ── Heterogeneous scenes (each scene = one distinct scenario) ────
  // Same scenario discharge profiles as the single-scene probes
  // above (5 dam³/h vs 10 dam³/h); only the scene partitioning
  // changes.  cut_sharing=none is the safe baseline.
  SUBCASE("3p × 2sc × 2sn uneq-prob 0.7/0.3 cut_sharing=none — baseline")
  {
    const auto worst = run_probe(ProbeConfig {
        .n_phases = 3,
        .n_scenarios = 2,
        .n_scenes = 2,
        .max_iterations = 15,
        .unequal_probabilities = true,
        .cut_sharing = CutSharingMode::none,
    });
    CHECK_MESSAGE(worst <= 1.0 + 1e-6,
                  "LB-overshoot under cut_sharing=none — would indicate a "
                  "non-cut-sharing bug at multi-scene scale");
  }

  // multicut × non-uniform probabilities (0.7/0.3): theorem-M3
  // uncertified — the uniform 1/N varphi pricing is the
  // resampled-process recursion only for uniform probabilities, so
  // LB > UB here is possible and NOT a regression (and even at
  // uniform probabilities the persistent-path UB and the
  // resampled-process LB are incomparable, Corollary M2).  WARN-only
  // per `docs/formulation/sddp-cut-validity.md` §8; the strict
  // certification lives in `test_sddp_cut_oracle.cpp`.
  SUBCASE("3p × 2sc × 2sn uneq-prob 0.7/0.3 cut_sharing=multicut (M3)")
  {
    const auto worst = run_probe(ProbeConfig {
        .n_phases = 3,
        .n_scenarios = 2,
        .n_scenes = 2,
        .max_iterations = 15,
        .unequal_probabilities = true,
        .cut_sharing = CutSharingMode::multicut,
    });
    INFO("cut_sharing=multicut worst LB/UB=" << worst);
    WARN_MESSAGE(worst <= 1.0 + 1e-6,
                 "multicut LB exceeded the persistent-path UB — expected "
                 "under non-uniform probabilities (theorem M3) and/or the "
                 "resampled-vs-persistent process mismatch (Corollary M2)");
  }
}
