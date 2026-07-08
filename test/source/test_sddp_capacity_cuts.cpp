// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_capacity_cuts.cpp
 * @brief     Capacity-space cut extraction (`extract_capacity_cuts`) —
 *            the investment-master API of the SDDiP campaign
 *            (deliverable 3 working subset; design in
 *            docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md
 *            §7).
 * @date      2026-07-08
 *
 * Fixture: the 2-scene 3-phase hydro planning plus a CONTINUOUS
 * candidate expansion unit (base capacity 0 + `expcap`/`expmod`/
 * `annual_capcost`, c0 convention — pure LP, no MIP backend needed).  The
 * expansion machinery registers `capainst`/`capacost` as SDDP state variables,
 * so every backward cut already carries ∂V/∂capacity subgradients;
 * the extraction API projects the phase-0 cuts onto exactly those
 * coordinates and reports the dropped non-capacity states (here the
 * reservoir `efin`).
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_capacity_cuts.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

[[nodiscard]] Planning make_expansion_planning()
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  // Candidate mid-merit unit (c0 convention: base capacity 0 + expcap):
  // 2 modules × 50 MW at $1/MW-hour (8760 $/MW-year), cheaper than the
  // thermal unit so building it is attractive.  Continuous expmod →
  // pure LP.
  planning.system.generator_array.push_back(Generator {
      .uid = Uid {3},
      .name = "expansion_gen",
      .bus = Uid {1},
      .gcost = 20.0,
      .capacity = 0.0,
      .expcap = 50.0,
      .expmod = 2.0,
      .annual_capcost = 8760.0,
  });
  return planning;
}

[[nodiscard]] SDDPOptions make_opts(int max_iterations)
{
  SDDPOptions opts;
  opts.max_iterations = max_iterations;
  opts.convergence_tol = 1.0e-9;
  opts.stationary_tol = 0.0;
  opts.cut_sharing = CutSharingMode::none;
  opts.apertures = std::vector<Uid> {};  // pure Benders backward pass
  opts.enable_api = false;
  return opts;
}

}  // namespace

TEST_CASE(  // NOLINT
    "extract_capacity_cuts — phase-0 cuts project onto the expansion "
    "capainst/capacost coordinates")
{
  auto planning = make_expansion_planning();
  PlanningLP plp(std::move(planning));

  auto opts = make_opts(/*max_iterations=*/4);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto cuts = sddp.stored_cuts();
  const auto& sim = plp.simulation();

  // Ground truth: number of phase-0 optimality cuts in the store.
  const auto phase0_uid = sim.uid_of(PhaseIndex {0});
  const auto n_phase0 = static_cast<std::size_t>(std::ranges::count_if(
      cuts,
      [&](const StoredCut& sc)
      {
        return sc.type == CutType::Optimality && sc.phase_uid == phase0_uid;
      }));
  REQUIRE(n_phase0 >= 2);

  const auto extracted = extract_capacity_cuts(sim, cuts);
  CHECK(extracted.size() == n_phase0);

  for (const auto& cut : extracted) {
    CHECK(std::isfinite(cut.rhs));
    CHECK(cut.phase_uid == phase0_uid);

    // The candidate unit's capainst must survive the projection —
    // it is the coordinate the investment master prices.
    const bool has_candidate_capainst = std::ranges::any_of(
        cut.coefficients,
        [](const CapacityCutCoefficient& c)
        {
          return c.col_name == "capainst" && c.uid == Uid {3}
          && c.class_name == "Generator";
        });
    CHECK(has_candidate_capainst);

    // Every kept coordinate is a capacity accounting state.
    for (const auto& c : cut.coefficients) {
      CHECK((c.col_name == "capainst" || c.col_name == "capacost"));
      CHECK(std::isfinite(c.coeff));
    }

    // The reservoir `efin` coordinate must have been projected away
    // and reported (mixed-state caveat — design doc §7.3).
    CHECK(cut.dropped_state_coefficients >= 1);
  }
}

TEST_CASE(  // NOLINT
    "extract_capacity_cuts — phase selector and empty input")
{
  auto planning = make_expansion_planning();
  PlanningLP plp(std::move(planning));

  auto opts = make_opts(/*max_iterations=*/3);
  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto cuts = sddp.stored_cuts();
  const auto& sim = plp.simulation();

  // Phase-1 cuts exist too (they bound the phase-2 tail) and are
  // addressable through the selector.
  const auto ph1 = extract_capacity_cuts(sim, cuts, PhaseIndex {1});
  CHECK_FALSE(ph1.empty());
  for (const auto& cut : ph1) {
    CHECK(cut.phase_uid == sim.uid_of(PhaseIndex {1}));
  }

  // Empty input → empty output.
  const std::vector<StoredCut> none;
  CHECK(extract_capacity_cuts(sim, none).empty());
}
