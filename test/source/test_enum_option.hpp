// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_enum_option.hpp
 * @brief     Unit tests for the enum_option.hpp generic enum framework
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/options_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Generic framework tests ─────────────────────────────────────────────────

TEST_CASE("enum_from_name returns matching value")  // NOLINT
{
  const auto result = enum_from_name(std::span {solver_type_entries}, "sddp");
  REQUIRE(result.has_value());
  CHECK(result.value_or(SolverType::monolithic) == SolverType::sddp);
}

TEST_CASE("enum_from_name returns nullopt for unknown name")  // NOLINT
{
  const auto result = enum_from_name(std::span {solver_type_entries}, "bogus");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("enum_name returns canonical name for known value")  // NOLINT
{
  CHECK(enum_name(std::span {solver_type_entries}, SolverType::monolithic)
        == "monolithic");
  CHECK(enum_name(std::span {solver_type_entries}, SolverType::sddp) == "sddp");
}

TEST_CASE("enum_name returns 'unknown' for out-of-range value")  // NOLINT
{
  CHECK(enum_name(std::span {solver_type_entries}, static_cast<SolverType>(99))
        == "unknown");
}

// ─── SolverType ──────────────────────────────────────────────────────────────

TEST_CASE("solver_type_from_name")  // NOLINT
{
  CHECK(solver_type_from_name("monolithic").value_or(SolverType::sddp)
        == SolverType::monolithic);
  CHECK(solver_type_from_name("sddp").value_or(SolverType::monolithic)
        == SolverType::sddp);
  CHECK_FALSE(solver_type_from_name("unknown").has_value());
}

TEST_CASE("solver_type_name")  // NOLINT
{
  CHECK(solver_type_name(SolverType::monolithic) == "monolithic");
  CHECK(solver_type_name(SolverType::sddp) == "sddp");
}

// ─── SolveMode ───────────────────────────────────────────────────────────────

TEST_CASE("solve_mode_from_name")  // NOLINT
{
  CHECK(solve_mode_from_name("monolithic").value_or(SolveMode::sequential)
        == SolveMode::monolithic);
  CHECK(solve_mode_from_name("sequential").value_or(SolveMode::monolithic)
        == SolveMode::sequential);
  CHECK_FALSE(solve_mode_from_name("bad").has_value());
}

// ─── BoundaryCutsMode ────────────────────────────────────────────────────────

TEST_CASE("boundary_cuts_mode_from_name")  // NOLINT
{
  CHECK(boundary_cuts_mode_from_name("noload").value_or(
            BoundaryCutsMode::separated)
        == BoundaryCutsMode::noload);
  CHECK(boundary_cuts_mode_from_name("separated")
            .value_or(BoundaryCutsMode::noload)
        == BoundaryCutsMode::separated);
  CHECK(boundary_cuts_mode_from_name("combined")
            .value_or(BoundaryCutsMode::noload)
        == BoundaryCutsMode::combined);
  CHECK_FALSE(boundary_cuts_mode_from_name("xyz").has_value());
}

// ─── DataFormat ──────────────────────────────────────────────────────────────

TEST_CASE("data_format_from_name")  // NOLINT
{
  CHECK(data_format_from_name("parquet").value_or(DataFormat::csv)
        == DataFormat::parquet);
  CHECK(data_format_from_name("csv").value_or(DataFormat::parquet)
        == DataFormat::csv);
  CHECK_FALSE(data_format_from_name("json").has_value());
}

TEST_CASE("data_format_name")  // NOLINT
{
  CHECK(data_format_name(DataFormat::parquet) == "parquet");
  CHECK(data_format_name(DataFormat::csv) == "csv");
}

// ─── CompressionCodec ────────────────────────────────────────────────────────

TEST_CASE("compression_codec_from_name")  // NOLINT
{
  CHECK(compression_codec_from_name("zstd").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::zstd);
  CHECK(compression_codec_from_name("gzip").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::gzip);
  CHECK(compression_codec_from_name("lz4").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::lz4);
  CHECK(compression_codec_from_name("bzip2").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::bzip2);
  CHECK(
      compression_codec_from_name("xz").value_or(CompressionCodec::uncompressed)
      == CompressionCodec::xz);
  CHECK(compression_codec_from_name("uncompressed")
            .value_or(CompressionCodec::zstd)
        == CompressionCodec::uncompressed);
  CHECK_FALSE(compression_codec_from_name("snappy").has_value());
}

// ─── CutSharingMode ─────────────────────────────────────────────────────────

TEST_CASE("cut_sharing_mode_from_name")  // NOLINT
{
  CHECK(cut_sharing_mode_from_name("none").value_or(CutSharingMode::expected)
        == CutSharingMode::none);
  CHECK(cut_sharing_mode_from_name("expected").value_or(CutSharingMode::none)
        == CutSharingMode::expected);
  CHECK(cut_sharing_mode_from_name("accumulate").value_or(CutSharingMode::none)
        == CutSharingMode::accumulate);
  CHECK(cut_sharing_mode_from_name("max").value_or(CutSharingMode::none)
        == CutSharingMode::max);
  CHECK_FALSE(cut_sharing_mode_from_name("bad").has_value());
}

// ─── ElasticFilterMode ───────────────────────────────────────────────────────

TEST_CASE("elastic_filter_mode_from_name")  // NOLINT
{
  CHECK(elastic_filter_mode_from_name("single_cut")
            .value_or(ElasticFilterMode::multi_cut)
        == ElasticFilterMode::single_cut);
  // "cut" is a backward-compatible alias for "single_cut"
  CHECK(elastic_filter_mode_from_name("cut").value_or(
            ElasticFilterMode::multi_cut)
        == ElasticFilterMode::single_cut);
  CHECK(elastic_filter_mode_from_name("multi_cut")
            .value_or(ElasticFilterMode::single_cut)
        == ElasticFilterMode::multi_cut);
  CHECK(elastic_filter_mode_from_name("backpropagate")
            .value_or(ElasticFilterMode::single_cut)
        == ElasticFilterMode::backpropagate);
  CHECK_FALSE(elastic_filter_mode_from_name("unknown").has_value());
}

// ─── HotStartMode ───────────────────────────────────────────────────────────

TEST_CASE("hot_start_mode_from_name")  // NOLINT
{
  CHECK(hot_start_mode_from_name("none").value_or(HotStartMode::replace)
        == HotStartMode::none);
  CHECK(hot_start_mode_from_name("keep").value_or(HotStartMode::none)
        == HotStartMode::keep);
  CHECK(hot_start_mode_from_name("append").value_or(HotStartMode::none)
        == HotStartMode::append);
  CHECK(hot_start_mode_from_name("replace").value_or(HotStartMode::none)
        == HotStartMode::replace);
  CHECK_FALSE(hot_start_mode_from_name("bad").has_value());
}

// ─── OptionsLP enum accessors ────────────────────────────────────────────────

TEST_CASE("OptionsLP enum accessors return correct defaults")  // NOLINT
{
  const OptionsLP opts;

  SUBCASE("solver_type_enum defaults to monolithic")
  {
    CHECK(opts.solver_type_enum() == SolverType::monolithic);
  }

  SUBCASE("input_format_enum defaults to parquet")
  {
    CHECK(opts.input_format_enum() == DataFormat::parquet);
  }

  SUBCASE("output_format_enum defaults to parquet")
  {
    CHECK(opts.output_format_enum() == DataFormat::parquet);
  }

  SUBCASE("output_compression_enum defaults to zstd")
  {
    CHECK(opts.output_compression_enum() == CompressionCodec::zstd);
  }

  SUBCASE("sddp_cut_sharing_mode_enum defaults to none")
  {
    CHECK(opts.sddp_cut_sharing_mode_enum() == CutSharingMode::none);
  }

  SUBCASE("sddp_elastic_mode_enum defaults to single_cut")
  {
    CHECK(opts.sddp_elastic_mode_enum() == ElasticFilterMode::single_cut);
  }

  SUBCASE("sddp_boundary_cuts_mode_enum defaults to separated")
  {
    CHECK(opts.sddp_boundary_cuts_mode_enum() == BoundaryCutsMode::separated);
  }

  SUBCASE("sddp_hot_start_mode_enum defaults to none")
  {
    CHECK(opts.sddp_hot_start_mode_enum() == HotStartMode::none);
  }

  SUBCASE("monolithic_solve_mode_enum defaults to monolithic")
  {
    CHECK(opts.monolithic_solve_mode_enum() == SolveMode::monolithic);
  }

  SUBCASE("monolithic_boundary_cuts_mode_enum defaults to separated")
  {
    CHECK(opts.monolithic_boundary_cuts_mode_enum()
          == BoundaryCutsMode::separated);
  }
}

TEST_CASE("OptionsLP enum accessors parse explicit values")  // NOLINT
{
  Options raw;
  raw.solver_type = "sddp";
  raw.input_format = "csv";
  raw.output_format = "csv";
  raw.output_compression = "gzip";
  raw.sddp_options.cut_sharing_mode = "expected";
  raw.sddp_options.elastic_mode = "multi_cut";
  raw.sddp_options.boundary_cuts_mode = "combined";
  raw.sddp_options.hot_start_mode = "append";
  raw.monolithic_options.solve_mode = "sequential";
  raw.monolithic_options.boundary_cuts_mode = "noload";

  const OptionsLP opts(std::move(raw));

  CHECK(opts.solver_type_enum() == SolverType::sddp);
  CHECK(opts.input_format_enum() == DataFormat::csv);
  CHECK(opts.output_format_enum() == DataFormat::csv);
  CHECK(opts.output_compression_enum() == CompressionCodec::gzip);
  CHECK(opts.sddp_cut_sharing_mode_enum() == CutSharingMode::expected);
  CHECK(opts.sddp_elastic_mode_enum() == ElasticFilterMode::multi_cut);
  CHECK(opts.sddp_boundary_cuts_mode_enum() == BoundaryCutsMode::combined);
  CHECK(opts.sddp_hot_start_mode_enum() == HotStartMode::append);
  CHECK(opts.monolithic_solve_mode_enum() == SolveMode::sequential);
  CHECK(opts.monolithic_boundary_cuts_mode_enum() == BoundaryCutsMode::noload);
}

// ─── validate_enum_options ──────────────────────────────────────────────────

TEST_CASE("validate_enum_options returns empty for valid defaults")  // NOLINT
{
  const OptionsLP opts;
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("validate_enum_options returns empty for valid explicit values")
// NOLINT
{
  Options raw;
  raw.solver_type = "sddp";
  raw.input_format = "csv";
  raw.output_format = "parquet";
  raw.output_compression = "gzip";
  raw.sddp_options.cut_sharing_mode = "expected";
  raw.sddp_options.elastic_mode = "multi_cut";
  raw.sddp_options.boundary_cuts_mode = "combined";
  raw.sddp_options.hot_start_mode = "replace";
  raw.monolithic_options.solve_mode = "sequential";
  raw.monolithic_options.boundary_cuts_mode = "noload";

  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("validate_enum_options warns about unknown solver_type")  // NOLINT
{
  Options raw;
  raw.solver_type = "bogus_solver";
  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("solver_type") != std::string::npos);
  CHECK(warnings[0].find("bogus_solver") != std::string::npos);
}

TEST_CASE(
    "validate_enum_options warns about multiple unknown "
    "values")  // NOLINT
{
  Options raw;
  raw.solver_type = "bad";
  raw.input_format = "xml";
  raw.sddp_options.cut_sharing_mode = "invalid";
  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.size() == 3);
}

TEST_CASE("sddp_hot_start_mode backward compat: bool hot_start")  // NOLINT
{
  SUBCASE("hot_start=true maps to replace")
  {
    Options raw;
    raw.sddp_options.hot_start = true;
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_hot_start_mode_enum() == HotStartMode::replace);
  }

  SUBCASE("hot_start=false maps to none")
  {
    Options raw;
    raw.sddp_options.hot_start = false;
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_hot_start_mode_enum() == HotStartMode::none);
  }

  SUBCASE("hot_start_mode takes precedence over hot_start")
  {
    Options raw;
    raw.sddp_options.hot_start = true;
    raw.sddp_options.hot_start_mode = "keep";
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_hot_start_mode_enum() == HotStartMode::keep);
  }
}
