// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/unit_registry.hpp>

using namespace gtopt;

namespace
{

constexpr std::string_view kDict = R"json(
{
  "version": 1,
  "units": [
    {"class": "generator", "canonical": "pmax",  "dialect": "gtopt",  "unit": "MW"},
    {"class": "generator", "canonical": "pmax",  "dialect": "plexos", "unit": "MW"},
    {"class": "generator", "canonical": "pmax",  "dialect": "sddp",   "unit": "MW"},
    {"class": "generator", "canonical": "gcost", "dialect": "gtopt",  "unit": "USD/MWh"},
    {"class": "reservoir", "canonical": "emax",  "dialect": "gtopt",  "unit": "Mm3"},
    {"class": "reservoir", "canonical": "emax",  "dialect": "plp",    "unit": "hm3"},
    {"class": "waterway",  "canonical": "fmax",  "dialect": "gtopt",  "unit": "m3/s"},
    {"class": "line",      "canonical": "lossfactor", "dialect": "gtopt", "unit": ""}
  ]
}
)json";

}  // namespace

TEST_CASE("UnitRegistry — basic (class, canonical, dialect) lookup")  // NOLINT
{
  const UnitRegistry r {kDict};

  SUBCASE("known triple returns the unit string")
  {
    CHECK(r.unit_for("generator", "pmax", "gtopt").value_or("") == "MW");
    CHECK(r.unit_for("generator", "gcost", "gtopt").value_or("") == "USD/MWh");
    CHECK(r.unit_for("waterway", "fmax", "gtopt").value_or("") == "m3/s");
  }

  SUBCASE("dialect divergence — same canonical, different dialect units")
  {
    // The whole point of the per-dialect index: PLP and gtopt agree on
    // the canonical name but disagree on the unit.  A future
    // unit-mismatch warning under --naming-dialect relies on this.
    CHECK(r.unit_for("reservoir", "emax", "gtopt").value_or("") == "Mm3");
    CHECK(r.unit_for("reservoir", "emax", "plp").value_or("") == "hm3");
  }

  SUBCASE("dimensionless entries return an empty string, not nullopt")
  {
    const auto u = r.unit_for("line", "lossfactor", "gtopt");
    REQUIRE(u.has_value());
    CHECK(u->empty());
  }
}

TEST_CASE("UnitRegistry — missing entries return nullopt")  // NOLINT
{
  const UnitRegistry r {kDict};

  CHECK_FALSE(r.unit_for("generator", "pmax", "unknown_dialect").has_value());
  CHECK_FALSE(r.unit_for("generator", "not_a_field", "gtopt").has_value());
  CHECK_FALSE(r.unit_for("unknown_class", "pmax", "gtopt").has_value());
}

TEST_CASE(
    "UnitRegistry — convenience overload assumes gtopt dialect")  // NOLINT
{
  const UnitRegistry r {kDict};

  // The 2-arg form is sugar for "gtopt"; LP-construction call sites
  // already know the canonical name and the implicit dialect is the
  // gtopt-native one.
  CHECK(r.unit_for("generator", "pmax").value_or("") == "MW");
  CHECK(r.unit_for("waterway", "fmax").value_or("") == "m3/s");
  CHECK_FALSE(r.unit_for("generator", "not_a_field").has_value());
}

TEST_CASE("UnitRegistry — version validation")  // NOLINT
{
  // The loader must reject unknown schema versions so the build/install
  // story can evolve cleanly.  An empty units[] is fine (size==0).
  CHECK_NOTHROW(
      UnitRegistry {std::string_view {R"({"version": 1, "units": []})"}});
  CHECK_THROWS(
      UnitRegistry {std::string_view {R"({"version": 2, "units": []})"}});
  CHECK_THROWS(
      UnitRegistry {std::string_view {R"({"version": 0, "units": []})"}});
}

TEST_CASE(
    "UnitRegistry — empty registry: lookups all return nullopt")  // NOLINT
{
  const UnitRegistry r {std::string_view {R"({"version": 1, "units": []})"}};
  CHECK(r.size() == 0);
  CHECK_FALSE(r.unit_for("generator", "pmax", "gtopt").has_value());
}

TEST_CASE(
    "UnitRegistry — class_agnostic_unit_for collapses agreeing classes")  // NOLINT
{
  // When every registered entry for a (canonical, dialect) pair
  // resolves to the same unit, the class-agnostic accessor can drop
  // the class dimension cleanly.  That is the input-warn path's
  // happy case: canonicalize_json_keys knows the canonical and the
  // dialect but not the element type.
  constexpr std::string_view dict = R"json(
{
  "version": 1,
  "units": [
    {"class": "generator", "canonical": "pmax", "dialect": "gtopt",  "unit": "MW"},
    {"class": "battery",   "canonical": "pmax", "dialect": "gtopt",  "unit": "MW"},
    {"class": "reservoir", "canonical": "emax", "dialect": "gtopt",  "unit": "Mm3"},
    {"class": "reservoir", "canonical": "emax", "dialect": "plp",    "unit": "hm3"}
  ]
}
)json";

  const UnitRegistry r {std::string_view {dict}};

  SUBCASE("agreeing classes — returns the single unit")
  {
    CHECK(r.class_agnostic_unit_for("pmax", "gtopt").value_or("") == "MW");
    CHECK(r.class_agnostic_unit_for("emax", "gtopt").value_or("") == "Mm3");
    CHECK(r.class_agnostic_unit_for("emax", "plp").value_or("") == "hm3");
  }

  SUBCASE("unknown canonical — nullopt")
  {
    CHECK_FALSE(r.class_agnostic_unit_for("not_a_field", "gtopt").has_value());
  }

  SUBCASE("unknown dialect — nullopt")
  {
    CHECK_FALSE(
        r.class_agnostic_unit_for("pmax", "no_such_dialect").has_value());
  }
}

TEST_CASE(
    "UnitRegistry — class_agnostic_unit_for returns nullopt on disagreement")  // NOLINT
{
  // Two classes register the same canonical name under the same
  // dialect but with different units — the accessor must refuse to
  // pick one over the other (no class context means no way to
  // disambiguate).  Caller falls back to the dialect-only warning.
  constexpr std::string_view dict = R"json(
{
  "version": 1,
  "units": [
    {"class": "generator", "canonical": "cost", "dialect": "gtopt", "unit": "USD/MWh"},
    {"class": "fuel",      "canonical": "cost", "dialect": "gtopt", "unit": "USD/MMBtu"}
  ]
}
)json";

  const UnitRegistry r {std::string_view {dict}};
  CHECK_FALSE(r.class_agnostic_unit_for("cost", "gtopt").has_value());
}

TEST_CASE(
    "UnitRegistry — singleton ships with gtopt's unit_dialects.json")  // NOLINT
{
  // The compiled-in fallback parses cleanly and covers the canonical
  // pmax/MW pin — this is the smoke test for the configure-time
  // bake-in pipeline.  If this fails, share/gtopt/unit_dialects.json
  // diverged from the schema between source edits and the last
  // `cmake configure`.
  const auto& r = UnitRegistry::instance();
  CHECK(r.size() > 0);
  CHECK(r.unit_for("generator", "pmax", "gtopt").value_or("") == "MW");
}
