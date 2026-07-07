// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/json_canonicalize.hpp>
#include <gtopt/names_registry.hpp>

using namespace gtopt;

namespace
{

constexpr std::string_view kDict = R"json(
{
  "version": 1,
  "aliases": [
    {"class": "generator", "canonical": "pmax",  "alt": "max_power",     "dialect": "gtopt"},
    {"class": "generator", "canonical": "pmax",  "alt": "Max Capacity",  "dialect": "plexos"},
    {"class": "generator", "canonical": "gcost", "alt": "marginal_cost", "dialect": "gtopt"},
    {"class": "demand",    "canonical": "lmax",  "alt": "max_demand",    "dialect": "gtopt"},
    {"class": "line",      "canonical": "tmax_ab", "alt": "max_flow_ab", "dialect": "gtopt"}
  ]
}
)json";

}  // namespace

TEST_CASE("canonicalize_json_keys — simple alias rewrite")  // NOLINT
{
  const NamesRegistry r {kDict};

  SUBCASE("single object key rewritten")
  {
    const auto out =
        canonicalize_json_keys(R"({"max_power": 100, "pmin": 5})", r);
    CHECK(out == R"({"pmax": 100, "pmin": 5})");
  }

  SUBCASE("multiple aliases in one object")
  {
    const auto out = canonicalize_json_keys(
        R"({"max_power": 100, "marginal_cost": 50, "max_demand": 200})", r);
    CHECK(out == R"({"pmax": 100, "gcost": 50, "lmax": 200})");
  }

  SUBCASE("nested object — keys at multiple depths")
  {
    const auto out = canonicalize_json_keys(
        R"({"generators": [{"max_power": 100, "marginal_cost": 50}]})", r);
    CHECK(out == R"({"generators": [{"pmax": 100, "gcost": 50}]})");
  }

  SUBCASE("alias key with embedded space is rewritten")
  {
    const auto out = canonicalize_json_keys(R"({"Max Capacity": 100})", r);
    CHECK(out == R"({"pmax": 100})");
  }

  SUBCASE("unregistered key is passed through verbatim")
  {
    const auto out = canonicalize_json_keys(R"({"unknown": 42})", r);
    CHECK(out == R"({"unknown": 42})");
  }

  SUBCASE("canonical key is passed through verbatim")
  {
    const auto out = canonicalize_json_keys(R"({"pmax": 100})", r);
    CHECK(out == R"({"pmax": 100})");
  }
}

TEST_CASE("canonicalize_json_keys — value strings are NOT rewritten")  // NOLINT
{
  const NamesRegistry r {kDict};

  SUBCASE("string value equal to an alias name is left alone")
  {
    // The "name" key has a value that happens to equal an alias.
    // Only the key position should ever be rewritten.
    const auto out = canonicalize_json_keys(R"({"name": "max_power"})", r);
    CHECK(out == R"({"name": "max_power"})");
  }

  SUBCASE("description string with alias-like content stays verbatim")
  {
    const auto out = canonicalize_json_keys(
        R"({"description": "the max_power: legacy field is deprecated"})", r);
    CHECK(out
          == R"({"description": "the max_power: legacy field is deprecated"})");
  }

  SUBCASE("alias name embedded in array of strings")
  {
    const auto out =
        canonicalize_json_keys(R"({"tags": ["max_power", "pmin"]})", r);
    CHECK(out == R"({"tags": ["max_power", "pmin"]})");
  }
}

TEST_CASE(
    "canonicalize_json_keys — handles JSON escapes inside strings")  // NOLINT
{
  const NamesRegistry r {kDict};

  SUBCASE("escaped quote inside value does not terminate string scan")
  {
    // Value contains an escaped quote followed by a colon; without
    // escape-aware scanning, the rewriter could mis-identify the
    // following position as object-key context.
    const auto out =
        canonicalize_json_keys(R"({"name": "a\"b", "max_power": 1})", r);
    CHECK(out == R"({"name": "a\"b", "pmax": 1})");
  }

  SUBCASE("escaped backslash inside a value string")
  {
    const auto out =
        canonicalize_json_keys(R"({"path": "a\\b", "max_power": 1})", r);
    CHECK(out == R"({"path": "a\\b", "pmax": 1})");
  }
}

TEST_CASE("canonicalize_json_keys — empty / minimal documents")  // NOLINT
{
  const NamesRegistry r {kDict};

  CHECK(canonicalize_json_keys("", r).empty());
  CHECK(canonicalize_json_keys("{}", r) == "{}");
  CHECK(canonicalize_json_keys("[]", r) == "[]");
  CHECK(canonicalize_json_keys("null", r) == "null");
  CHECK(canonicalize_json_keys("42", r) == "42");
}

TEST_CASE(
    "canonicalize_json_keys — empty registry returns input verbatim")  // NOLINT
{
  // Registry with no aliases: any input returns unchanged.
  constexpr std::string_view empty_dict = R"({"version": 1, "aliases": []})";
  const NamesRegistry r {empty_dict};
  CHECK(r.size() == 0);

  const std::string input = R"({"max_power": 100, "pmax": 200})";
  CHECK(canonicalize_json_keys(input, r) == input);
}

TEST_CASE(
    "canonicalize_json_keys — convenience overload uses singleton")  // NOLINT
{
  // The singleton ships with the gtopt-shipped dictionary, which
  // includes `marginal_cost` -> `gcost`.
  const auto out = canonicalize_json_keys(R"({"marginal_cost": 25.5})");
  CHECK(out == R"({"gcost": 25.5})");
}

TEST_CASE(
    "canonicalize_json_keys — enforce_dialect still canonicalizes")  // NOLINT
{
  // The warn-only enforcement does NOT block the rewrite; alias keys
  // from any dialect are still folded to canonical so strict daw::json
  // parsing continues to succeed.  The dialect mismatch only surfaces
  // through `spdlog::warn` (not captured here — assert the rewrite).
  const NamesRegistry r {kDict};

  const auto out = canonicalize_json_keys(
      R"({"max_power": 100, "Max Capacity": 50})", r, "gtopt");
  CHECK(out == R"({"pmax": 100, "pmax": 50})");
}

TEST_CASE(
    "canonicalize_json_keys — empty enforce_dialect = silent legacy")  // NOLINT
{
  const NamesRegistry r {kDict};
  // Same input as the previous case but with empty dialect: no warning
  // and output is the same canonicalized form.
  const auto out = canonicalize_json_keys(
      R"({"Max Capacity": 50})", r, /*enforce_dialect=*/"");
  CHECK(out == R"({"pmax": 50})");
}

TEST_CASE(
    "canonicalize_json_keys — singleton: dialect mismatch on reservoir.emax")  // NOLINT
{
  // Integration smoke against the real `share/gtopt/naming_dialects.json`
  // + `share/gtopt/unit_dialects.json` singletons.  Asserts the
  // canonicalization output only (the unit-mismatch escalation lives
  // in the spdlog stream and is not captured here) but exercises the
  // full registry-loading + dialect-tagging + unit-lookup pipeline.
  //
  // Input: a PLP-flavoured `VolMax` key under `--naming-dialect=gtopt`.
  // - naming_dialects.json maps VolMax → emax (reservoir, plp).
  // - unit_dialects.json has emax/gtopt=Mm3 and emax/plp=hm3.
  // The canonicalize pass should rewrite the key to `emax` and, in
  // the spdlog stream, emit the UNIT MISMATCH error for hm3 vs Mm3.
  const auto out = canonicalize_json_keys(R"({"VolMax": 1500})",
                                          /*enforce_dialect=*/"gtopt");
  CHECK(out == R"({"emax": 1500})");
}

TEST_CASE("decanonicalize_json_keys — canonical -> alias rewrite")  // NOLINT
{
  const NamesRegistry r {kDict};

  SUBCASE("single canonical key rewritten to plexos alias")
  {
    const auto out = decanonicalize_json_keys(R"({"pmax": 100})", r, "plexos");
    CHECK(out == R"({"Max Capacity": 100})");
  }

  SUBCASE("keys without a matching (canonical, dialect) pair stay verbatim")
  {
    // `pmax` has a plexos alias, but `pmin` and `gcost` do not in the
    // test dictionary; only `pmax` is rewritten.
    const auto out = decanonicalize_json_keys(
        R"({"pmax": 100, "pmin": 5, "gcost": 50})", r, "plexos");
    CHECK(out == R"({"Max Capacity": 100, "pmin": 5, "gcost": 50})");
  }

  SUBCASE("empty dialect short-circuits to verbatim")
  {
    const std::string input = R"({"pmax": 100, "gcost": 50})";
    CHECK(decanonicalize_json_keys(input, r, "") == input);
  }

  SUBCASE("round-trip: canonical -> alias -> canonical")
  {
    const std::string canonical = R"({"pmax": 100, "gcost": 50})";
    const auto plexos_form = decanonicalize_json_keys(canonical, r, "gtopt");
    const auto back = canonicalize_json_keys(plexos_form, r);
    CHECK(back == canonical);
  }
}
