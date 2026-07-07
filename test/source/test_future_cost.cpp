// SPDX-License-Identifier: BSD-3-Clause
//
// Piece-2 step A: FutureCost (FCF / cost-to-go) element JSON binding.  Verifies
// the element parses end-to-end and the three scoping strings convert to their
// typed enums (BoundaryCutSharingMode / BoundaryCutsMode / BoundaryCutSoftCost)
// via FutureCostConstructor.  See docs/design/future_cost_and_user_model.md.

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/future_cost.hpp>
#include <gtopt/json/json_future_cost.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/system.hpp>

using namespace gtopt;

TEST_CASE("FutureCost JSON binding + enum conversion")  // NOLINT
{
  SUBCASE("full element parses and enum strings convert")
  {
    constexpr std::string_view js = R"({
      "uid": 1,
      "name": "fcf",
      "cuts_file": "boundary_cuts.csv",
      "scale_alpha": 100000.0,
      "mean_shift": true,
      "sharing": "multicut",
      "mode": "separated",
      "valuation": "max"
    })";
    const auto fc = daw::json::from_json<FutureCost>(js);
    CHECK(fc.uid == Uid {1});
    CHECK(fc.name == "fcf");
    CHECK((fc.cuts_file && *fc.cuts_file == "boundary_cuts.csv"));
    CHECK(fc.scale_alpha.value_or(-1.0) == doctest::Approx(100000.0));
    CHECK(fc.mean_shift.value_or(false));
    CHECK((fc.sharing && *fc.sharing == BoundaryCutSharingMode::multicut));
    CHECK((fc.mode && *fc.mode == BoundaryCutsMode::separated));
    CHECK((fc.valuation && *fc.valuation == BoundaryCutSoftCost::max));
  }

  SUBCASE("omitted optional fields stay nullopt")
  {
    constexpr std::string_view js = R"({"uid": 2, "name": "fcf2"})";
    const auto fc = daw::json::from_json<FutureCost>(js);
    CHECK(fc.uid == Uid {2});
    CHECK_FALSE(fc.cuts_file.has_value());
    CHECK_FALSE(fc.scale_alpha.has_value());
    CHECK_FALSE(fc.mean_shift.has_value());
    CHECK_FALSE(fc.sharing.has_value());
    CHECK_FALSE(fc.mode.has_value());
    CHECK_FALSE(fc.valuation.has_value());
  }

  SUBCASE("enums round-trip through to_json")
  {
    FutureCost fc {.uid = Uid {3}, .name = "fcf3"};
    fc.sharing = BoundaryCutSharingMode::per_scene;
    fc.valuation = BoundaryCutSoftCost::avg;
    const auto round = daw::json::from_json<FutureCost>(daw::json::to_json(fc));
    CHECK(
        (round.sharing && *round.sharing == BoundaryCutSharingMode::per_scene));
    CHECK((round.valuation && *round.valuation == BoundaryCutSoftCost::avg));
    CHECK_FALSE(round.mode.has_value());
  }

  SUBCASE("future_cost_array parses inside a System")
  {
    constexpr std::string_view js = R"({
      "name": "sys",
      "future_cost_array": [
        {"uid": 7, "name": "fcf", "sharing": "shared", "mean_shift": false}
      ]
    })";
    const auto sys = daw::json::from_json<System>(js);
    REQUIRE(sys.future_cost_array.size() == 1);
    const auto& fc = sys.future_cost_array.front();
    CHECK(fc.uid == Uid {7});
    CHECK((fc.sharing && *fc.sharing == BoundaryCutSharingMode::shared));
    CHECK(fc.mean_shift.value_or(true) == false);
  }
}
