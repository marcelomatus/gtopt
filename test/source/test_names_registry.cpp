// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/names_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

constexpr std::string_view kMinimalDict = R"json(
{
  "version": 1,
  "doc": "minimal test fixture",
  "aliases": [
    {"class": "generator", "canonical": "pmax", "alt": "max_power",      "dialect": "gtopt"},
    {"class": "generator", "canonical": "pmax", "alt": "Max Capacity",   "dialect": "plexos"},
    {"class": "demand",    "canonical": "lmax", "alt": "max_demand",     "dialect": "gtopt"}
  ]
}
)json";

}  // namespace

TEST_CASE("NamesRegistry — load from inline JSON content")  // NOLINT
{
  const NamesRegistry r {kMinimalDict};

  SUBCASE("size matches alias count")
  {
    CHECK(r.size() == 3);
  }

  SUBCASE("canonical_for returns canonical name for a known alias")
  {
    const auto c = r.canonical_for("max_power");
    CHECK((c.has_value() && *c == "pmax"));
  }

  SUBCASE("canonical_for honors PLEXOS-style alias with embedded space")
  {
    const auto c = r.canonical_for("Max Capacity");
    CHECK((c.has_value() && *c == "pmax"));
  }

  SUBCASE("canonical_for returns nullopt for unknown alias")
  {
    CHECK_FALSE(r.canonical_for("unknown_field").has_value());
  }

  SUBCASE("canonical name is not itself registered as alias")
  {
    // The canonical name (e.g. "pmax") should NOT map back to itself
    // via the alias table — it is the canonical, not an alias of
    // itself.  This guards a regression where build_from_json
    // accidentally inserts the canonical name as its own alias.
    CHECK_FALSE(r.canonical_for("pmax").has_value());
    CHECK_FALSE(r.canonical_for("lmax").has_value());
  }

  SUBCASE("canonical_to_aliases inverse map is populated")
  {
    const auto& inv = r.canonical_to_aliases();
    REQUIRE(inv.contains("pmax"));
    CHECK(inv.at("pmax").size() == 2);
  }
}

TEST_CASE("NamesRegistry — uniqueness invariant")  // NOLINT
{
  // Same alias mapped to two different canonical names must throw.
  constexpr std::string_view bad = R"json(
{
  "version": 1,
  "aliases": [
    {"class": "a", "canonical": "pmax", "alt": "max_power", "dialect": "gtopt"},
    {"class": "b", "canonical": "lmax", "alt": "max_power", "dialect": "gtopt"}
  ]
}
)json";

  CHECK_THROWS_AS(NamesRegistry {bad}, std::runtime_error);
}

TEST_CASE(
    "NamesRegistry — duplicate (alias, canonical) row is tolerated")  // NOLINT
{
  // Same row twice is benign — the loader should just skip the dupe.
  constexpr std::string_view dupe = R"json(
{
  "version": 1,
  "aliases": [
    {"class": "generator", "canonical": "pmax", "alt": "max_power", "dialect": "gtopt"},
    {"class": "generator", "canonical": "pmax", "alt": "max_power", "dialect": "pypsa"}
  ]
}
)json";

  const NamesRegistry r {dupe};
  CHECK(r.size() == 1);
  CHECK(r.canonical_for("max_power").value_or("") == "pmax");
}

TEST_CASE("NamesRegistry — version validation")  // NOLINT
{
  constexpr std::string_view wrong_version = R"json(
{ "version": 999, "aliases": [] }
)json";

  CHECK_THROWS_AS(NamesRegistry {wrong_version}, std::runtime_error);
}

TEST_CASE("NamesRegistry — class-scoped alias lookup")  // NOLINT
{
  constexpr std::string_view with_class_aliases = R"json(
{
  "version": 1,
  "aliases": [
    {"class": "flow", "canonical": "discharge", "alt": "Vazao", "dialect": "sddp"}
  ],
  "class_aliases": [
    {"class": "flow_right", "canonical": "target", "alt": "discharge",  "dialect": "gtopt-legacy"},
    {"class": "flow_right", "canonical": "uvalue", "alt": "use_value",  "dialect": "gtopt-legacy"}
  ]
}
)json";

  const NamesRegistry r {with_class_aliases};

  SUBCASE("class-scoped lookup resolves within its scope")
  {
    CHECK(r.canonical_for("flow_right", "discharge").value_or("") == "target");
    CHECK(r.canonical_for("flow_right", "use_value").value_or("") == "uvalue");
  }

  SUBCASE("class-scoped alias is invisible to global lookup")
  {
    // `discharge` is class-scoped under flow_right; the class-blind
    // overload must NOT rewrite it (because `discharge` is also
    // canonical for `flow`, and rewriting it globally would break
    // Flow parsing).
    CHECK_FALSE(r.canonical_for("discharge").has_value());
    CHECK_FALSE(r.canonical_for("use_value").has_value());
  }

  SUBCASE("class-scoped lookup falls back to global aliases")
  {
    // `Vazao` is a *global* alias for `discharge` (Flow).  The
    // class-aware overload should find it even when scoped to flow.
    CHECK(r.canonical_for("flow", "Vazao").value_or("") == "discharge");
  }

  SUBCASE("class scope is per-class — other classes see no entry")
  {
    // `discharge` mapped under flow_right scope must NOT appear
    // when the lookup is scoped to a different class.
    CHECK_FALSE(r.canonical_for("flow", "discharge").has_value());
    CHECK_FALSE(r.canonical_for("generator", "discharge").has_value());
  }
}

TEST_CASE("NamesRegistry — class-scoped uniqueness invariant")  // NOLINT
{
  // The same (class, alias) pair mapped to two different canonicals
  // must throw (mirror of the global uniqueness check).
  constexpr std::string_view bad = R"json(
{
  "version": 1,
  "aliases": [],
  "class_aliases": [
    {"class": "flow_right", "canonical": "target", "alt": "discharge", "dialect": "x"},
    {"class": "flow_right", "canonical": "uvalue", "alt": "discharge", "dialect": "y"}
  ]
}
)json";

  CHECK_THROWS_AS(NamesRegistry {bad}, std::runtime_error);
}

TEST_CASE(
    "NamesRegistry — singleton loads from the shipped dictionary")  // NOLINT
{
  // The compiled-in fallback (or the on-disk source file) is shipped
  // with at least the Generator / Demand / Line aliases described in
  // docs/analysis/naming-conventions.md §9.3.
  const auto& r = NamesRegistry::instance();

  SUBCASE("ships the gtopt-modern Generator aliases")
  {
    CHECK(r.canonical_for("marginal_cost").value_or("") == "gcost");
    CHECK(r.canonical_for("max_power").value_or("") == "pmax");
    CHECK(r.canonical_for("min_power").value_or("") == "pmin");
  }

  SUBCASE("ships the gtopt-modern Demand aliases")
  {
    CHECK(r.canonical_for("max_demand").value_or("") == "lmax");
    CHECK(r.canonical_for("curtailment_cost").value_or("") == "fcost");
  }

  SUBCASE("ships the gtopt-modern Line aliases")
  {
    CHECK(r.canonical_for("max_flow_ab").value_or("") == "tmax_ab");
    CHECK(r.canonical_for("max_flow_ba").value_or("") == "tmax_ba");
    CHECK(r.canonical_for("transfer_cost").value_or("") == "tcost");
  }

  SUBCASE("ships the §11 model_options modern aliases")
  {
    CHECK(r.canonical_for("value_of_lost_load").value_or("")
          == "demand_fail_cost");
    CHECK(r.canonical_for("reserve_shortage_cost").value_or("")
          == "reserve_fail_cost");
    CHECK(r.canonical_for("hydro_spill_cost").value_or("")
          == "hydro_fail_cost");
    CHECK(r.canonical_for("state_violation_cost").value_or("")
          == "state_fail_cost");
    // §11.10 rename: demand_option_c → demand_fail_rhs_shift.
    // The legacy name is now the alias; the descriptive name is canonical.
    CHECK(r.canonical_for("demand_option_c").value_or("")
          == "demand_fail_rhs_shift");
    CHECK(r.canonical_for("copper_plate").value_or("") == "use_single_bus");
  }
}
