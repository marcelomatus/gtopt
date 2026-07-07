// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the index_holder utilities.  Currently focused on
// `find_or_empty_inner` — the lookup helper that the conditional-
// outer-key assignment pattern (LineLP / JunctionLP / WaterwayLP)
// relies on to read safely from STBIndexHolders that may have
// missing (scenario, stage) entries.

#include <doctest/doctest.h>
#include <gtopt/index_holder.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

namespace
{

/// Build a minimal SimulationLP + ScenarioLP + StageLP triple.
///
/// `find_or_empty_inner` only reads the `uid()` of the scenario and
/// the stage, so the fixture can stay tiny: a single scenario, a
/// single 1-block stage.  The helper is fully decoupled from
/// SystemLP / PlanningLP construction.
struct StbFixture
{
  PlanningOptionsLP options {};
  Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {1}, .probability_factor = 1.0}},
  };
  SimulationLP sim {simulation, options};

  // Pull the first scenario / first stage refs from the SimulationLP
  // so their uid() accessors match the seed UIDs above.
  const ScenarioLP& scenario {sim.scenarios().front()};
  const StageLP& stage {sim.stages().front()};
};

}  // namespace

TEST_CASE(  // NOLINT
    "find_or_empty_inner returns the stored inner map when the (s, t) "
    "outer key is present")
{
  StbFixture f;

  STBIndexHolder<ColIndex> holder;
  BIndexHolder<ColIndex> inner;
  inner.emplace(make_uid<Block>(1), ColIndex {42});
  holder.emplace(std::make_tuple(f.scenario.uid(), f.stage.uid()),
                 std::move(inner));

  const auto& got = find_or_empty_inner(holder, f.scenario, f.stage);
  REQUIRE(got.size() == 1);
  CHECK(got.at(make_uid<Block>(1)) == ColIndex {42});
}

TEST_CASE(  // NOLINT
    "find_or_empty_inner returns an empty inner map when the (s, t) "
    "outer key is absent (does NOT throw)")
{
  // This is the regression-prone case: pre-2026-05-15 the LP-element
  // accessors `flow_cols_at` / `drain_cols_at` did `holder.at({...})`
  // and threw `std::out_of_range` on missing keys.  The cascade
  // L0 → L1 transition would then trip an uncaught throw because L1's
  // LP rebuild legitimately has some (scenario, stage) cells without
  // a drain or flow column.  `find_or_empty_inner` centralises the
  // graceful fallback so every consumer (turbine_lp, pump_lp,
  // reservoir_*_lp, storage_lp, output emitters) sees an empty inner
  // map and short-circuits naturally.
  StbFixture f;

  STBIndexHolder<ColIndex> holder;  // outer key absent

  // Must NOT throw — that was the failure mode pre-fix.
  CHECK_NOTHROW([[maybe_unused]] auto& r =
                    find_or_empty_inner(holder, f.scenario, f.stage));

  const auto& got = find_or_empty_inner(holder, f.scenario, f.stage);
  CHECK(got.empty());
}

TEST_CASE(  // NOLINT
    "find_or_empty_inner returns the same static empty per Inner type")
{
  StbFixture f;

  // Two distinct holders of the SAME inner type return references to
  // the SAME static-storage empty when both keys are missing.
  STBIndexHolder<ColIndex> a;
  STBIndexHolder<ColIndex> b;

  const auto& empty_a = find_or_empty_inner(a, f.scenario, f.stage);
  const auto& empty_b = find_or_empty_inner(b, f.scenario, f.stage);
  CHECK(&empty_a == &empty_b);
  CHECK(empty_a.empty());

  // Holders of a DIFFERENT inner type instantiate a different
  // static — the addresses must NOT coincide.
  STBIndexHolder<std::vector<ColIndex>> c;
  const auto& empty_c = find_or_empty_inner(c, f.scenario, f.stage);
  CHECK(empty_c.empty());
  // Distinct types → distinct statics: pointer equality across types
  // is ill-formed under the C++ standard, but we can at least confirm
  // the vector-of-cols static behaves independently of the col-only
  // static by mutating one and observing the other is unaffected.
  // (Both are const, so the mutability check just confirms type-level
  // isolation.)
  static_assert(!std::is_same_v<std::remove_cvref_t<decltype(empty_a)>,
                                std::remove_cvref_t<decltype(empty_c)>>);
}

TEST_CASE(  // NOLINT
    "find_or_empty_inner returns the stored map even when the inner "
    "BIndexHolder is empty (zero-size insertion still distinguishable "
    "from outer-key absence)")
{
  // The conditional-insert pattern in LP elements is:
  //   if (!inner.empty()) holder[st_key] = std::move(inner);
  // So in practice we never insert empty inners.  But the helper
  // must still distinguish "key present with empty inner" from "key
  // absent" — both yield an empty map by reference but only the
  // present-key case touches user-owned storage.
  StbFixture f;

  STBIndexHolder<ColIndex> holder;
  holder.emplace(std::make_tuple(f.scenario.uid(), f.stage.uid()),
                 BIndexHolder<ColIndex> {});  // explicit empty insert

  const auto& got = find_or_empty_inner(holder, f.scenario, f.stage);
  CHECK(got.empty());
  // The returned reference must point at the holder's own inner, not
  // the static fallback.  Address equality proves we didn't fall
  // through to the static.
  const auto it = holder.find(std::make_tuple(f.scenario.uid(), f.stage.uid()));
  REQUIRE(it != holder.end());
  CHECK(&got == &it->second);
}
