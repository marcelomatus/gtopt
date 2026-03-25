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
  const auto result = enum_from_name(std::span {method_type_entries}, "sddp");
  REQUIRE(result.has_value());
  CHECK(result.value_or(MethodType::monolithic) == MethodType::sddp);
}

TEST_CASE("enum_from_name returns nullopt for unknown name")  // NOLINT
{
  const auto result = enum_from_name(std::span {method_type_entries}, "bogus");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("enum_name returns canonical name for known value")  // NOLINT
{
  CHECK(enum_name(std::span {method_type_entries}, MethodType::monolithic)
        == "monolithic");
  CHECK(enum_name(std::span {method_type_entries}, MethodType::sddp) == "sddp");
}

TEST_CASE("enum_name returns 'unknown' for out-of-range value")  // NOLINT
{
  CHECK(enum_name(std::span {method_type_entries}, static_cast<MethodType>(99))
        == "unknown");
}

// ─── MethodType ──────────────────────────────────────────────────────────────

TEST_CASE("method_type_from_name")  // NOLINT
{
  CHECK(method_type_from_name("monolithic").value_or(MethodType::sddp)
        == MethodType::monolithic);
  CHECK(method_type_from_name("sddp").value_or(MethodType::monolithic)
        == MethodType::sddp);
  CHECK_FALSE(method_type_from_name("unknown").has_value());
}

TEST_CASE("method_type_name")  // NOLINT
{
  CHECK(method_type_name(MethodType::monolithic) == "monolithic");
  CHECK(method_type_name(MethodType::sddp) == "sddp");
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

TEST_CASE("cut_recovery_mode_from_name")  // NOLINT
{
  CHECK(cut_recovery_mode_from_name("none").value_or(HotStartMode::replace)
        == HotStartMode::none);
  CHECK(cut_recovery_mode_from_name("keep").value_or(HotStartMode::none)
        == HotStartMode::keep);
  CHECK(cut_recovery_mode_from_name("append").value_or(HotStartMode::none)
        == HotStartMode::append);
  CHECK(cut_recovery_mode_from_name("replace").value_or(HotStartMode::none)
        == HotStartMode::replace);
  CHECK_FALSE(cut_recovery_mode_from_name("bad").has_value());
}

// ─── LpNamesLevel ───────────────────────────────────────────────────────────

TEST_CASE("lp_names_level_from_name")  // NOLINT
{
  CHECK(lp_names_level_from_name("minimal").value_or(LpNamesLevel::only_cols)
        == LpNamesLevel::minimal);
  CHECK(lp_names_level_from_name("only_cols").value_or(LpNamesLevel::minimal)
        == LpNamesLevel::only_cols);
  CHECK(
      lp_names_level_from_name("cols_and_rows").value_or(LpNamesLevel::minimal)
      == LpNamesLevel::cols_and_rows);
  CHECK_FALSE(lp_names_level_from_name("bogus").has_value());
}

TEST_CASE("lp_names_level_name")  // NOLINT
{
  CHECK(lp_names_level_name(LpNamesLevel::minimal) == "minimal");
  CHECK(lp_names_level_name(LpNamesLevel::only_cols) == "only_cols");
  CHECK(lp_names_level_name(LpNamesLevel::cols_and_rows) == "cols_and_rows");
}

// ─── OptionsLP enum accessors ────────────────────────────────────────────────

TEST_CASE("OptionsLP enum accessors return correct defaults")  // NOLINT
{
  const OptionsLP opts;

  SUBCASE("method_type_enum defaults to monolithic")
  {
    CHECK(opts.method_type_enum() == MethodType::monolithic);
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

  SUBCASE("sddp_cut_recovery_mode_enum defaults to none")
  {
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::none);
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
  raw.method = "sddp";
  raw.input_format = "csv";
  raw.output_format = "csv";
  raw.output_compression = "gzip";
  raw.sddp_options.cut_sharing_mode = "expected";
  raw.sddp_options.elastic_mode = "multi_cut";
  raw.sddp_options.boundary_cuts_mode = "combined";
  raw.sddp_options.cut_recovery_mode = "append";
  raw.monolithic_options.solve_mode = "sequential";
  raw.monolithic_options.boundary_cuts_mode = "noload";

  const OptionsLP opts(std::move(raw));

  CHECK(opts.method_type_enum() == MethodType::sddp);
  CHECK(opts.input_format_enum() == DataFormat::csv);
  CHECK(opts.output_format_enum() == DataFormat::csv);
  CHECK(opts.output_compression_enum() == CompressionCodec::gzip);
  CHECK(opts.sddp_cut_sharing_mode_enum() == CutSharingMode::expected);
  CHECK(opts.sddp_elastic_mode_enum() == ElasticFilterMode::multi_cut);
  CHECK(opts.sddp_boundary_cuts_mode_enum() == BoundaryCutsMode::combined);
  CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::append);
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
  raw.method = "sddp";
  raw.input_format = "csv";
  raw.output_format = "parquet";
  raw.output_compression = "gzip";
  raw.sddp_options.cut_sharing_mode = "expected";
  raw.sddp_options.elastic_mode = "multi_cut";
  raw.sddp_options.boundary_cuts_mode = "combined";
  raw.sddp_options.cut_recovery_mode = "replace";
  raw.monolithic_options.solve_mode = "sequential";
  raw.monolithic_options.boundary_cuts_mode = "noload";

  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("validate_enum_options warns about unknown method")  // NOLINT
{
  Options raw;
  raw.method = "bogus_solver";
  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  REQUIRE(warnings.size() == 1);
  CHECK(warnings[0].find("method") != std::string::npos);
  CHECK(warnings[0].find("bogus_solver") != std::string::npos);
}

TEST_CASE(
    "validate_enum_options warns about multiple unknown "
    "values")  // NOLINT
{
  Options raw;
  raw.method = "bad";
  raw.input_format = "xml";
  raw.sddp_options.cut_sharing_mode = "invalid";
  const OptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.size() == 3);
}

TEST_CASE("sddp_cut_recovery_mode explicit values")  // NOLINT
{
  SUBCASE("cut_recovery_mode=replace maps to replace")
  {
    Options raw;
    raw.sddp_options.cut_recovery_mode = "replace";
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::replace);
  }

  SUBCASE("cut_recovery_mode=none maps to none")
  {
    Options raw;
    raw.sddp_options.cut_recovery_mode = "none";
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::none);
  }

  SUBCASE("cut_recovery_mode=keep maps to keep")
  {
    Options raw;
    raw.sddp_options.cut_recovery_mode = "keep";
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::keep);
  }

  SUBCASE("cut_recovery_mode=append maps to append")
  {
    Options raw;
    raw.sddp_options.cut_recovery_mode = "append";
    const OptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::append);
  }
}
