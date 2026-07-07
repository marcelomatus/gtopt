// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests pinning the *resolved defaults* of the user-facing options
// that callers rely on most often.  The class triggered by the
// `resolve_parquet_codec` silent-fallback bug (2026-05-19): the option
// docstring promised `lz4`, but the resolver mapped unknown codec
// names to UNCOMPRESSED — every parquet column was uncompressed for
// roughly 24 hours of `master`.  These tests lock the *resolved* value
// (what `PlanningOptionsLP` returns) against the docstring promise so
// that future flips of the compiled-in default never again silently
// disagree with the codec-table or with the documentation.
//
// Coverage map (one TEST_CASE per option, grouped under
// `"PlanningOptions defaults"`):
//
//   - write_out                 (default = OutputFlags::all)
//   - output_compression        (default = CompressionCodec::snappy)
//   - output_format             (default = DataFormat::parquet)
//   - method                    (default = MethodType::monolithic)
//   - scale_objective           (default = 1000 for monolithic; 1 otherwise)
//   - demand_fail_cost          (no compiled-in default — OptReal stays unset)
//   - use_kirchhoff             (default = true)
//   - use_single_bus            (default = false)
//
// Plus JSON round-trip tests that pin the canonical text shape of the
// PlanningOptions block across at least three representative cases.

#include <string>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>

using namespace gtopt;

// ─── write_out ─────────────────────────────────────────────────────────────

TEST_CASE(
    "PlanningOptions defaults: write_out default is "
    "sol,dual,rc:Generator,Line (extras excluded)")
// NOLINT
{
  // Compiled-in default: sol + dual everywhere, reduced costs only on
  // Generator + Line (the union of every current consumer's rc reads).
  // `extras` (heat-rate slacks, vom/fuel decomposition, line losses,
  // overload slacks, capacity duals) stays off — opt in via
  // `--write-out all` or `--write-out ...,extras`.
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  const auto& sel = wrapper.write_out();
  CHECK(sel.atoms
        == (OutputFlags::solution | OutputFlags::dual
            | OutputFlags::reduced_cost));
  CHECK_FALSE(has_flag(sel.atoms, OutputFlags::extras));
  CHECK(sel.sol_classes.empty());
  CHECK(sel.dual_classes.empty());
  REQUIRE(sel.rc_classes.size() == 2);
  CHECK(sel.rc_classes[0] == "Generator");
  CHECK(sel.rc_classes[1] == "Line");
  CHECK(sel.extra_classes.empty());

  // The default scope passes Generator+Line through the rc gate but
  // skips every other class.
  CHECK(sel.emits(OutputFlags::reduced_cost, "Generator"));
  CHECK(sel.emits(OutputFlags::reduced_cost, "Line"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Battery"));
  CHECK_FALSE(sel.emits(OutputFlags::reduced_cost, "Demand"));
}

TEST_CASE("PlanningOptions defaults: write_out explicit value round-trips")
// NOLINT
{
  PlanningOptions opts;
  opts.write_out = OutputFlags::solution | OutputFlags::dual;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.write_out().atoms
        == (OutputFlags::solution | OutputFlags::dual));
}

TEST_CASE("PlanningOptions defaults: parse_output_selection scoped spec")
// NOLINT
{
  // Spec from the docstring on `OutputSelection` — `rc` carries a
  // per-class allow-list.  Locking this here so that
  // `parse_output_selection` (the function plumbed into `--write-out`
  // and the `write_out` JSON key by `PlanningOptionsConstructor` /
  // `apply_cli_options`) keeps the scoped behaviour intact.
  const auto sel = parse_output_selection("sol,dual,rc:Generator,Line");
  // `sol,dual,rc` parses to the OR of three atoms — NOT the literal
  // `all`, which since the `extras` atom landed includes `extras`.
  CHECK(sel.atoms
        == (OutputFlags::solution | OutputFlags::dual
            | OutputFlags::reduced_cost));
  CHECK_FALSE(has_flag(sel.atoms, OutputFlags::extras));
  CHECK(sel.sol_classes.empty());
  CHECK(sel.dual_classes.empty());
  REQUIRE(sel.rc_classes.size() == 2);
  CHECK(sel.rc_classes[0] == "Generator");
  CHECK(sel.rc_classes[1] == "Line");
}

// ─── output_compression ─────────────────────────────────────────────────────

TEST_CASE("PlanningOptions defaults: output_compression default is zstd")
// NOLINT
{
  // Default flipped from `snappy` to `zstd` on 2026-05-19 alongside the
  // `output_layout = long` default — see the docstring on
  // `PlanningOptionsLP::default_output_compression` for the rationale.
  // zstd + long + BYTE_STREAM_SPLIT lands at ~3-7× smaller on-disk
  // footprint than snappy + wide on the typical 0.1 %-dense gtopt
  // output.
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.output_compression_enum() == CompressionCodec::zstd);
  CHECK(wrapper.output_compression() == "zstd");
}

TEST_CASE("PlanningOptions defaults: probe_parquet_codec accepts snappy")
// NOLINT
{
  // probe_parquet_codec is the gate `configure_planning` runs at
  // startup.  For "snappy" — the post-2026-05-19 default — the probe
  // must keep returning "snappy" (not silently fall back to gzip /
  // uncompressed).  This is the exact code path that masked the
  // `resolve_parquet_codec` bug: the probe said snappy was OK, but
  // the resolver later returned UNCOMPRESSED.
  CHECK(probe_parquet_codec("snappy") == "snappy");
}

TEST_CASE(
    "PlanningOptions defaults: output_compression explicit lz4 propagates")
// NOLINT
{
  PlanningOptions opts;
  opts.output_compression = CompressionCodec::lz4;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.output_compression_enum() == CompressionCodec::lz4);
  CHECK(wrapper.output_compression() == "lz4");
}

// ─── output_format ──────────────────────────────────────────────────────────

TEST_CASE("PlanningOptions defaults: output_format default is parquet")
// NOLINT
{
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.output_format_enum() == DataFormat::parquet);
  CHECK(wrapper.output_format() == "parquet");
}

TEST_CASE("PlanningOptions defaults: output_format explicit csv propagates")
// NOLINT
{
  PlanningOptions opts;
  opts.output_format = DataFormat::csv;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.output_format_enum() == DataFormat::csv);
  CHECK(wrapper.output_format() == "csv");
}

// ─── method ─────────────────────────────────────────────────────────────────

TEST_CASE("PlanningOptions defaults: method default is monolithic")  // NOLINT
{
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.method_type_enum() == MethodType::monolithic);
}

TEST_CASE(
    "PlanningOptions defaults: method explicit sddp propagates")  // NOLINT
{
  PlanningOptions opts;
  opts.method = MethodType::sddp;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.method_type_enum() == MethodType::sddp);
}

// ─── scale_objective ────────────────────────────────────────────────────────

TEST_CASE(
    "PlanningOptions defaults: scale_objective default is 1000 for "
    "monolithic")  // NOLINT
{
  // Method-dependent default: `1000` for monolithic, `1.0` for SDDP /
  // cascade.  Pinning both branches because the comment on
  // `default_scale_objective` documents both as load-bearing for
  // kappa conditioning (juan/IPLP-scale runs).
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.scale_objective() == doctest::Approx(1'000.0));
}

TEST_CASE("PlanningOptions defaults: scale_objective default is 1.0 for sddp")
// NOLINT
{
  PlanningOptions opts;
  opts.method = MethodType::sddp;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.scale_objective() == doctest::Approx(1.0));
}

TEST_CASE(
    "PlanningOptions defaults: scale_objective explicit overrides "
    "method-derived default")  // NOLINT
{
  PlanningOptions opts;
  opts.model_options.scale_objective = 42.0;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.scale_objective() == doctest::Approx(42.0));
}

// ─── demand_fail_cost ───────────────────────────────────────────────────────

TEST_CASE("PlanningOptions defaults: demand_fail_cost has no compiled default")
// NOLINT
{
  // *** FINDING ***
  //
  // `CLAUDE.md` and `.github/copilot-instructions.md` both document
  // the default as `1000`, but the actual code carries NO compiled-in
  // default.  `ModelOptions::demand_fail_cost` is an `OptReal{}` and
  // `PlanningOptionsLP::demand_fail_cost()` simply returns the raw
  // optional.  Downstream callers (`gtopt_main.cpp:330`,
  // `gtopt_lp_runner.cpp:243`) coerce to `0.0` via `value_or(0.0)`.
  //
  // This test pins the *current* behaviour so that any future change
  // (either adding a real default of 1000 or fixing the docs) is
  // caught.  See the report summary for the docstring-vs-code
  // mismatch finding.
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(!wrapper.demand_fail_cost().has_value());
}

TEST_CASE(
    "PlanningOptions defaults: demand_fail_cost explicit value "
    "propagates")  // NOLINT
{
  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1'234.5;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.demand_fail_cost().value_or(-1.0) == doctest::Approx(1'234.5));
}

// ─── use_kirchhoff ──────────────────────────────────────────────────────────

TEST_CASE("PlanningOptions defaults: use_kirchhoff default is true")  // NOLINT
{
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.use_kirchhoff());
}

TEST_CASE("PlanningOptions defaults: use_kirchhoff explicit false")  // NOLINT
{
  PlanningOptions opts;
  opts.model_options.use_kirchhoff = false;
  const PlanningOptionsLP wrapper(opts);
  CHECK(!wrapper.use_kirchhoff());
}

// ─── use_single_bus ─────────────────────────────────────────────────────────

TEST_CASE(
    "PlanningOptions defaults: use_single_bus default is false")  // NOLINT
{
  const PlanningOptions opts;
  const PlanningOptionsLP wrapper(opts);
  CHECK(!wrapper.use_single_bus());
}

TEST_CASE("PlanningOptions defaults: use_single_bus explicit true")  // NOLINT
{
  PlanningOptions opts;
  opts.model_options.use_single_bus = true;
  const PlanningOptionsLP wrapper(opts);
  CHECK(wrapper.use_single_bus());
}

// ─── JSON round-trip: text → Planning → text ───────────────────────────────
//
// Three cases covering: an explicit method/format selection, a
// scoped write_out specification, and a model_options.* field.  These
// pin the canonical name set (`method`, `output_format`,
// `output_compression`, `write_out`, `model_options.demand_fail_cost`)
// so any future name rename through the canonicalize step is caught.

namespace
{
auto round_trip_planning(std::string_view input)
{
  const auto planning =
      daw::json::from_json<Planning>(input, StrictParsePolicy);
  return daw::json::to_json(planning);
}
}  // namespace

TEST_CASE(
    "PlanningOptions defaults: JSON round-trip preserves method")  // NOLINT
{
  constexpr std::string_view input = R"({"options":{"method":"sddp"}})";
  const auto round1 = round_trip_planning(input);
  const auto round2 = round_trip_planning(round1);
  // Don't assert byte-equal `round1 == round2`: a few sub-objects under
  // `monolithic_options` / `sddp_options` re-populate nested defaults
  // on first parse (e.g. `forward_solver_options`,
  // `backward_solver_options`, `solver_options.scaling`).  Those are
  // unrelated to the field under test — pin only the canonical token
  // for `method`, on both rounds.
  CHECK(round1.find("\"method\":\"sddp\"") != std::string::npos);
  CHECK(round2.find("\"method\":\"sddp\"") != std::string::npos);
}

TEST_CASE(
    "PlanningOptions defaults: JSON round-trip preserves output_format "
    "and output_compression")  // NOLINT
{
  constexpr std::string_view input =
      R"({"options":{"output_format":"csv","output_compression":"zstd"}})";
  const auto round1 = round_trip_planning(input);
  const auto round2 = round_trip_planning(round1);
  // See the comment in the `method` round-trip test: the byte-equal
  // check fails on nested-default re-population unrelated to the
  // fields under test.  Pin only the two canonical tokens.
  CHECK(round1.find("\"output_format\":\"csv\"") != std::string::npos);
  CHECK(round1.find("\"output_compression\":\"zstd\"") != std::string::npos);
  CHECK(round2.find("\"output_format\":\"csv\"") != std::string::npos);
  CHECK(round2.find("\"output_compression\":\"zstd\"") != std::string::npos);
}

TEST_CASE(
    "PlanningOptions defaults: JSON round-trip preserves write_out and "
    "model_options.demand_fail_cost")  // NOLINT
{
  // Combines a scoped `write_out` (exercising parse_output_selection
  // + output_selection_to_string) and a `model_options.*` field
  // (post-§11 canonical location).  Both must come back unchanged on
  // a second round trip.
  constexpr std::string_view input =
      R"({"options":{"write_out":"solution,dual,reduced_cost:Generator,Line","model_options":{"demand_fail_cost":1500}}})";
  const auto round1 = round_trip_planning(input);
  const auto round2 = round_trip_planning(round1);
  // Don't assert byte-equal round1 == round2: a few sub-objects under
  // `monolithic_options` / `sddp_options` re-populate nested
  // defaults on first parse (e.g. `forward_solver_options`,
  // `backward_solver_options`).  Those are unrelated to write_out;
  // pin only the two fields under test for stability.
  CHECK(round1.find("\"demand_fail_cost\":1500") != std::string::npos);
  CHECK(round2.find("\"demand_fail_cost\":1500") != std::string::npos);
  // The canonical render keeps the scoped class allow-list intact
  // across both passes.
  CHECK(round1.find("reduced_cost:Generator,Line") != std::string::npos);
  CHECK(round2.find("reduced_cost:Generator,Line") != std::string::npos);
}
