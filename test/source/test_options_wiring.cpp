// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_options_wiring.cpp
 * @brief     Tests that PlanningOptions fields are correctly wired through
 *            PlanningOptionsLP accessors, migrate_flat_to_model_options, and
 *            PlanningOptions::merge; exercises all possible enum values for
 *            every critical option.
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

PlanningOptionsLP make_lp(PlanningOptions opts = {})
{
  return PlanningOptionsLP {std::move(opts)};
}

// Minimal JSON for a full-solve test with single bus.
constexpr auto wiring_json = R"({
  "simulation": {
    "block_array":    [{"uid": 1, "duration": 1}],
    "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "wiring_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 10.0, "capacity": 200.0}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50.0}
    ]
  }
})";

std::filesystem::path write_wiring_json(const std::string& name,
                                        std::string_view content = wiring_json)
{
  auto tmp = std::filesystem::temp_directory_path() / name;
  tmp.replace_extension(".json");
  std::ofstream ofs(tmp);
  ofs << content;
  return tmp.replace_extension("");
}

}  // namespace

// ─── Default accessors ───────────────────────────────────────────────────────

TEST_CASE("PlanningOptionsLP defaults — empty PlanningOptions")  // NOLINT
{
  const auto lp = make_lp();

  CHECK(lp.input_directory() == PlanningOptionsLP::default_input_directory);
  CHECK(lp.input_format_enum() == PlanningOptionsLP::default_input_format);
  CHECK(lp.output_directory() == PlanningOptionsLP::default_output_directory);
  CHECK(lp.output_format_enum() == PlanningOptionsLP::default_output_format);
  CHECK(lp.output_compression_enum()
        == PlanningOptionsLP::default_output_compression);

  CHECK(lp.use_kirchhoff() == PlanningOptionsLP::default_use_kirchhoff);
  CHECK(lp.use_single_bus() == PlanningOptionsLP::default_use_single_bus);
  CHECK(lp.use_line_losses() == PlanningOptionsLP::default_use_line_losses);
  CHECK(lp.loss_segments() == PlanningOptionsLP::default_loss_segments);
  CHECK(lp.kirchhoff_threshold()
        == doctest::Approx(PlanningOptionsLP::default_kirchhoff_threshold));
  CHECK(lp.scale_objective()
        == doctest::Approx(PlanningOptionsLP::default_scale_objective));
  CHECK(lp.scale_theta() == doctest::Approx(1.0));
  CHECK(lp.use_uid_fname() == PlanningOptionsLP::default_use_uid_fname);
  CHECK(lp.annual_discount_rate()
        == doctest::Approx(PlanningOptionsLP::default_annual_discount_rate));

  CHECK(lp.lp_debug() == false);
  CHECK(lp.lp_only() == false);
  CHECK(lp.lp_fingerprint() == false);
  CHECK(lp.constraint_mode() == ConstraintMode::strict);
  CHECK(lp.write_out() == OutputFlags::all);
}

// ─── Flat field → migrate_flat_to_model_options ─────────────────────────────

TEST_CASE(
    "migrate_flat_to_model_options — flat use_kirchhoff → model_options")  // NOLINT
{
  PlanningOptions opts;
  opts.use_kirchhoff = false;
  opts.migrate_flat_to_model_options();
  CHECK(opts.model_options.use_kirchhoff.value_or(true) == false);
}

TEST_CASE(
    "migrate_flat_to_model_options — flat use_single_bus → model_options")  // NOLINT
{
  PlanningOptions opts;
  opts.use_single_bus = true;
  opts.migrate_flat_to_model_options();
  CHECK(opts.model_options.use_single_bus.value_or(false) == true);
}

TEST_CASE(
    "migrate_flat_to_model_options — flat demand_fail_cost → model_options")  // NOLINT
{
  PlanningOptions opts;
  opts.demand_fail_cost = 9999.0;
  opts.migrate_flat_to_model_options();
  CHECK(opts.model_options.demand_fail_cost.value_or(0.0)
        == doctest::Approx(9999.0));
}

TEST_CASE(
    "migrate_flat_to_model_options — flat scale_objective → model_options")  // NOLINT
{
  PlanningOptions opts;
  opts.scale_objective = 500.0;
  opts.migrate_flat_to_model_options();
  CHECK(opts.model_options.scale_objective.value_or(0.0)
        == doctest::Approx(500.0));
}

TEST_CASE(
    "migrate_flat_to_model_options — model_options wins when both set")  // NOLINT
{
  // model_options takes priority; flat field must NOT override it.
  PlanningOptions opts;
  opts.use_kirchhoff = false;  // flat
  opts.model_options.use_kirchhoff = true;  // model_options (should win)
  opts.migrate_flat_to_model_options();
  CHECK(opts.model_options.use_kirchhoff.value_or(false) == true);
}

// ─── Accessor wiring after construction ──────────────────────────────────────

TEST_CASE("PlanningOptionsLP::use_kirchhoff wired from flat field")  // NOLINT
{
  PlanningOptions opts;
  opts.use_kirchhoff = false;
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.use_kirchhoff() == false);
}

TEST_CASE(
    "PlanningOptionsLP::use_kirchhoff wired from model_options")  // NOLINT
{
  PlanningOptions opts;
  opts.model_options.use_kirchhoff = false;
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.use_kirchhoff() == false);
}

TEST_CASE("PlanningOptionsLP::use_single_bus all values")  // NOLINT
{
  for (const bool val : {false, true}) {
    PlanningOptions opts;
    opts.use_single_bus = val;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.use_single_bus() == val);
  }
}

TEST_CASE("PlanningOptionsLP::scale_objective custom value")  // NOLINT
{
  PlanningOptions opts;
  opts.scale_objective = 2000.0;
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.scale_objective() == doctest::Approx(2000.0));
}

TEST_CASE("PlanningOptionsLP::demand_fail_cost custom value")  // NOLINT
{
  PlanningOptions opts;
  opts.demand_fail_cost = 500.0;
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.demand_fail_cost().value_or(0.0) == doctest::Approx(500.0));
}

TEST_CASE("PlanningOptionsLP::input_directory custom value")  // NOLINT
{
  PlanningOptions opts;
  opts.input_directory = "my_input";
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.input_directory() == "my_input");
}

TEST_CASE("PlanningOptionsLP::output_directory custom value")  // NOLINT
{
  PlanningOptions opts;
  opts.output_directory = "my_output";
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.output_directory() == "my_output");
}

TEST_CASE("PlanningOptionsLP::lp_debug wired correctly")  // NOLINT
{
  for (const bool val : {false, true}) {
    PlanningOptions opts;
    opts.lp_debug = val;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.lp_debug() == val);
  }
}

TEST_CASE("PlanningOptionsLP::lp_only wired correctly")  // NOLINT
{
  for (const bool val : {false, true}) {
    PlanningOptions opts;
    opts.lp_only = val;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.lp_only() == val);
  }
}

// ─── All DataFormat values
// ────────────────────────────────────────────────────

TEST_CASE("DataFormat — all values wire through input_format_enum")  // NOLINT
{
  for (const DataFormat fmt : {DataFormat::parquet, DataFormat::csv}) {
    PlanningOptions opts;
    opts.input_format = fmt;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.input_format_enum() == fmt);
  }
}

TEST_CASE("DataFormat — all values wire through output_format_enum")  // NOLINT
{
  for (const DataFormat fmt : {DataFormat::parquet, DataFormat::csv}) {
    PlanningOptions opts;
    opts.output_format = fmt;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.output_format_enum() == fmt);
  }
}

// ─── All CompressionCodec values ─────────────────────────────────────────────

TEST_CASE(
    "CompressionCodec — all output_compression values wire correctly")  // NOLINT
{
  for (const CompressionCodec codec : {CompressionCodec::uncompressed,
                                       CompressionCodec::gzip,
                                       CompressionCodec::zstd,
                                       CompressionCodec::lz4,
                                       CompressionCodec::bzip2,
                                       CompressionCodec::xz,
                                       CompressionCodec::snappy,
                                       CompressionCodec::brotli,
                                       CompressionCodec::lzo})
  {
    PlanningOptions opts;
    opts.output_compression = codec;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.output_compression_enum() == codec);
  }
}

TEST_CASE(
    "CompressionCodec — all lp_compression values wire correctly")  // NOLINT
{
  for (const CompressionCodec codec : {CompressionCodec::uncompressed,
                                       CompressionCodec::gzip,
                                       CompressionCodec::zstd,
                                       CompressionCodec::lz4})
  {
    PlanningOptions opts;
    opts.lp_compression = codec;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.lp_compression_enum().value_or(CompressionCodec::bzip2) == codec);
  }
}

// ─── All MethodType values
// ────────────────────────────────────────────────────

TEST_CASE("MethodType — enum_name round-trips all values")  // NOLINT
{
  for (const MethodType m :
       {MethodType::monolithic, MethodType::sddp, MethodType::cascade})
  {
    const auto name = enum_name(m);
    CHECK_FALSE(name.empty());
    const auto back = enum_from_name<MethodType>(name);
    REQUIRE((back && *back == m));
  }
}

TEST_CASE("MethodType — method field wires through PlanningOptions")  // NOLINT
{
  for (const MethodType m :
       {MethodType::monolithic, MethodType::sddp, MethodType::cascade})
  {
    PlanningOptions opts;
    opts.method = m;
    CHECK(opts.method.value() == m);
  }
}

// ─── All BuildMode values
// ─────────────────────────────────────────────────────

TEST_CASE("BuildMode — enum_name round-trips all canonical values")  // NOLINT
{
  for (const BuildMode bm : {BuildMode::serial,
                             BuildMode::scene_parallel,
                             BuildMode::full_parallel,
                             BuildMode::direct_parallel})
  {
    const auto name = enum_name(bm);
    CHECK_FALSE(name.empty());
    const auto back = enum_from_name<BuildMode>(name);
    REQUIRE((back && *back == bm));
  }
}

TEST_CASE("BuildMode — underscore aliases accepted")  // NOLINT
{
  const auto sp = enum_from_name<BuildMode>("scene_parallel");
  REQUIRE(sp.has_value());
  CHECK(*sp == BuildMode::scene_parallel);

  const auto fp = enum_from_name<BuildMode>("full_parallel");
  REQUIRE(fp.has_value());
  CHECK(*fp == BuildMode::full_parallel);
}

// ─── All ConstraintMode values
// ────────────────────────────────────────────────

TEST_CASE(
    "ConstraintMode — all values wire through PlanningOptionsLP")  // NOLINT
{
  for (const ConstraintMode cm :
       {ConstraintMode::normal, ConstraintMode::strict, ConstraintMode::debug})
  {
    PlanningOptions opts;
    opts.constraint_mode = cm;
    const auto lp = make_lp(std::move(opts));
    CHECK(lp.constraint_mode() == cm);
  }
}

TEST_CASE("ConstraintMode — default is strict")  // NOLINT
{
  const auto lp = make_lp();
  CHECK(lp.constraint_mode() == ConstraintMode::strict);
}

// ─── All OutputFlags values
// ───────────────────────────────────────────────────

TEST_CASE("OutputFlags — has_flag correctly tests each bit")  // NOLINT
{
  CHECK(has_flag(OutputFlags::all, OutputFlags::solution));
  CHECK(has_flag(OutputFlags::all, OutputFlags::dual));
  CHECK(has_flag(OutputFlags::all, OutputFlags::reduced_cost));
  CHECK_FALSE(has_flag(OutputFlags::none, OutputFlags::solution));
  CHECK_FALSE(has_flag(OutputFlags::none, OutputFlags::dual));
}

TEST_CASE(
    "OutputFlags — parse_output_flags round-trips all keywords")  // NOLINT
{
  CHECK(parse_output_flags("none") == OutputFlags::none);
  CHECK(parse_output_flags("solution") == OutputFlags::solution);
  CHECK(parse_output_flags("sol") == OutputFlags::solution);
  CHECK(parse_output_flags("dual") == OutputFlags::dual);
  CHECK(parse_output_flags("reduced_cost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("rcost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("rc") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("cost") == OutputFlags::reduced_cost);
  CHECK(parse_output_flags("all") == OutputFlags::all);
}

TEST_CASE("OutputFlags — parse_output_flags multi-token")  // NOLINT
{
  const auto f = parse_output_flags("solution,dual");
  CHECK(has_flag(f, OutputFlags::solution));
  CHECK(has_flag(f, OutputFlags::dual));
  CHECK_FALSE(has_flag(f, OutputFlags::reduced_cost));
}

TEST_CASE("OutputFlags — output_flags_to_string round-trips")  // NOLINT
{
  CHECK(output_flags_to_string(OutputFlags::none) == "none");
  CHECK(output_flags_to_string(OutputFlags::all) == "all");
  const auto s =
      output_flags_to_string(OutputFlags::solution | OutputFlags::dual);
  CHECK(s.find("solution") != std::string::npos);
  CHECK(s.find("dual") != std::string::npos);
}

TEST_CASE("OutputFlags — write_out wired through PlanningOptionsLP")  // NOLINT
{
  PlanningOptions opts;
  opts.write_out = OutputFlags::solution;
  const auto lp = make_lp(std::move(opts));
  CHECK(lp.write_out() == OutputFlags::solution);
}

TEST_CASE("OutputFlags — default is all")  // NOLINT
{
  const auto lp = make_lp();
  CHECK(lp.write_out() == OutputFlags::all);
}

// ─── PlanningOptions::merge
// ───────────────────────────────────────────────────

TEST_CASE("PlanningOptions::merge — overlay wins for scalars")  // NOLINT
{
  PlanningOptions base;
  base.demand_fail_cost = 1000.0;
  base.use_kirchhoff = false;

  PlanningOptions overlay;
  overlay.demand_fail_cost = 2000.0;  // overlay wins
  overlay.use_kirchhoff = true;  // overlay wins

  base.merge(std::move(overlay));
  const auto lp = make_lp(std::move(base));
  CHECK(lp.demand_fail_cost().value_or(0.0) == doctest::Approx(2000.0));
  CHECK(lp.use_kirchhoff() == true);
}

TEST_CASE("PlanningOptions::merge — overlay fills absent fields")  // NOLINT
{
  PlanningOptions base;

  PlanningOptions overlay;
  overlay.demand_fail_cost = 500.0;
  overlay.use_single_bus = true;
  overlay.output_directory = "merged_out";

  base.merge(std::move(overlay));
  const auto lp = make_lp(std::move(base));
  CHECK(lp.demand_fail_cost().value_or(0.0) == doctest::Approx(500.0));
  CHECK(lp.use_single_bus() == true);
  CHECK(lp.output_directory() == "merged_out");
}

TEST_CASE("PlanningOptions::merge — variable_scales appended")  // NOLINT
{
  PlanningOptions base;
  base.variable_scales.push_back({.class_name = "Bus", .variable = "theta"});

  PlanningOptions overlay;
  overlay.variable_scales.push_back(
      {.class_name = "Generator", .variable = "generation"});

  base.merge(std::move(overlay));
  CHECK(base.variable_scales.size() == 2);
}

// ─── Full-solve wiring tests
// ──────────────────────────────────────────────────

TEST_CASE(
    "use_single_bus=true suppresses multi-bus topology (solve succeeds)")  // NOLINT
{
  const auto stem = write_wiring_json("wiring_single_bus");
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "wiring_single_bus_out")
          .string();
  const auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("lp_only=true completes without solving")  // NOLINT
{
  const auto stem = write_wiring_json("wiring_lp_only");
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "wiring_lp_only_out").string();
  const auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .lp_only = true,
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}

TEST_CASE("output_format=csv produces csv output")  // NOLINT
{
  const auto stem = write_wiring_json("wiring_csv_output");
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "wiring_csv_output_out")
          .string();
  const auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .output_format = "csv",
      .use_single_bus = true,
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
  // Verify at least one csv file was written
  bool found_csv = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(out_dir))
  {
    if (entry.path().extension() == ".csv") {
      found_csv = true;
      break;
    }
  }
  CHECK(found_csv);
}

TEST_CASE("--set demand_fail_cost wiring via set_options")  // NOLINT
{
  const auto stem = write_wiring_json("wiring_set_demand");
  const auto out_dir =
      (std::filesystem::temp_directory_path() / "wiring_set_demand_out")
          .string();
  const auto result = gtopt_main(MainOptions {
      .planning_files = {stem.string()},
      .output_directory = out_dir,
      .use_single_bus = true,
      .set_options = {"demand_fail_cost=9999"},
  });
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1) == 0);
}
