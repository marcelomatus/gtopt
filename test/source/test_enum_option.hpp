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
#include <gtopt/planning_options_lp.hpp>

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

// ─── ADL-based enum_from_name<E> and enum_name(E) ──────────────────────────

TEST_CASE("ADL enum_from_name<E> and enum_name(E)")  // NOLINT
{
  CHECK(enum_from_name<MethodType>("monolithic") == MethodType::monolithic);
  CHECK(enum_from_name<MethodType>("sddp") == MethodType::sddp);
  CHECK_FALSE(enum_from_name<MethodType>("unknown").has_value());

  CHECK(enum_name(MethodType::monolithic) == "monolithic");
  CHECK(enum_name(MethodType::sddp) == "sddp");
}

// ─── MethodType ──────────────────────────────────────────────────────────────

TEST_CASE("MethodType from_name and name")  // NOLINT
{
  CHECK(enum_from_name<MethodType>("monolithic").value_or(MethodType::sddp)
        == MethodType::monolithic);
  CHECK(enum_from_name<MethodType>("sddp").value_or(MethodType::monolithic)
        == MethodType::sddp);
  CHECK_FALSE(enum_from_name<MethodType>("unknown").has_value());

  CHECK(enum_name(MethodType::monolithic) == "monolithic");
  CHECK(enum_name(MethodType::sddp) == "sddp");
}

// ─── SolveMode ───────────────────────────────────────────────────────────────

TEST_CASE("SolveMode from_name")  // NOLINT
{
  CHECK(enum_from_name<SolveMode>("monolithic").value_or(SolveMode::sequential)
        == SolveMode::monolithic);
  CHECK(enum_from_name<SolveMode>("sequential").value_or(SolveMode::monolithic)
        == SolveMode::sequential);
  CHECK_FALSE(enum_from_name<SolveMode>("bad").has_value());
}

// ─── BoundaryCutsMode ────────────────────────────────────────────────────────

TEST_CASE("BoundaryCutsMode from_name")  // NOLINT
{
  CHECK(enum_from_name<BoundaryCutsMode>("noload").value_or(
            BoundaryCutsMode::separated)
        == BoundaryCutsMode::noload);
  CHECK(enum_from_name<BoundaryCutsMode>("separated")
            .value_or(BoundaryCutsMode::noload)
        == BoundaryCutsMode::separated);
  CHECK(enum_from_name<BoundaryCutsMode>("combined")
            .value_or(BoundaryCutsMode::noload)
        == BoundaryCutsMode::combined);
  CHECK_FALSE(enum_from_name<BoundaryCutsMode>("xyz").has_value());
}

// ─── DataFormat ──────────────────────────────────────────────────────────────

TEST_CASE("DataFormat from_name and name")  // NOLINT
{
  CHECK(enum_from_name<DataFormat>("parquet").value_or(DataFormat::csv)
        == DataFormat::parquet);
  CHECK(enum_from_name<DataFormat>("csv").value_or(DataFormat::parquet)
        == DataFormat::csv);
  CHECK_FALSE(enum_from_name<DataFormat>("json").has_value());

  CHECK(enum_name(DataFormat::parquet) == "parquet");
  CHECK(enum_name(DataFormat::csv) == "csv");
}

// ─── CompressionCodec ────────────────────────────────────────────────────────

TEST_CASE("CompressionCodec from_name")  // NOLINT
{
  CHECK(enum_from_name<CompressionCodec>("zstd").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::zstd);
  CHECK(enum_from_name<CompressionCodec>("gzip").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::gzip);
  CHECK(enum_from_name<CompressionCodec>("lz4").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::lz4);
  CHECK(enum_from_name<CompressionCodec>("bzip2").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::bzip2);
  CHECK(enum_from_name<CompressionCodec>("xz").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::xz);
  CHECK(enum_from_name<CompressionCodec>("uncompressed")
            .value_or(CompressionCodec::zstd)
        == CompressionCodec::uncompressed);
  CHECK(enum_from_name<CompressionCodec>("snappy").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::snappy);
  CHECK(enum_from_name<CompressionCodec>("brotli").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::brotli);
  CHECK(enum_from_name<CompressionCodec>("lzo").value_or(
            CompressionCodec::uncompressed)
        == CompressionCodec::lzo);
  CHECK_FALSE(enum_from_name<CompressionCodec>("unknown_codec").has_value());
}

// ─── CutSharingMode ─────────────────────────────────────────────────────────

TEST_CASE("CutSharingMode from_name")  // NOLINT
{
  CHECK(
      enum_from_name<CutSharingMode>("none").value_or(CutSharingMode::expected)
      == CutSharingMode::none);
  CHECK(
      enum_from_name<CutSharingMode>("expected").value_or(CutSharingMode::none)
      == CutSharingMode::expected);
  CHECK(enum_from_name<CutSharingMode>("accumulate")
            .value_or(CutSharingMode::none)
        == CutSharingMode::accumulate);
  CHECK(enum_from_name<CutSharingMode>("max").value_or(CutSharingMode::none)
        == CutSharingMode::max);
  CHECK_FALSE(enum_from_name<CutSharingMode>("bad").has_value());
}

// ─── ElasticFilterMode ──────────────────────────────────────────────────────

TEST_CASE("ElasticFilterMode from_name")  // NOLINT
{
  CHECK(enum_from_name<ElasticFilterMode>("single_cut")
            .value_or(ElasticFilterMode::multi_cut)
        == ElasticFilterMode::single_cut);
  // "cut" is a backward-compatible alias for "single_cut"
  CHECK(enum_from_name<ElasticFilterMode>("cut").value_or(
            ElasticFilterMode::multi_cut)
        == ElasticFilterMode::single_cut);
  CHECK(enum_from_name<ElasticFilterMode>("multi_cut")
            .value_or(ElasticFilterMode::single_cut)
        == ElasticFilterMode::multi_cut);
  CHECK(enum_from_name<ElasticFilterMode>("backpropagate")
            .value_or(ElasticFilterMode::single_cut)
        == ElasticFilterMode::backpropagate);
  CHECK_FALSE(enum_from_name<ElasticFilterMode>("unknown").has_value());
}

// ─── HotStartMode ──────────────────────────────────────────────────────────

TEST_CASE("HotStartMode from_name")  // NOLINT
{
  CHECK(enum_from_name<HotStartMode>("none").value_or(HotStartMode::replace)
        == HotStartMode::none);
  CHECK(enum_from_name<HotStartMode>("keep").value_or(HotStartMode::none)
        == HotStartMode::keep);
  CHECK(enum_from_name<HotStartMode>("append").value_or(HotStartMode::none)
        == HotStartMode::append);
  CHECK(enum_from_name<HotStartMode>("replace").value_or(HotStartMode::none)
        == HotStartMode::replace);
  CHECK_FALSE(enum_from_name<HotStartMode>("bad").has_value());
}

// ─── LpNamesLevel ──────────────────────────────────────────────────────────

TEST_CASE("LpNamesLevel from_name and name")  // NOLINT
{
  CHECK(
      enum_from_name<LpNamesLevel>("minimal").value_or(LpNamesLevel::only_cols)
      == LpNamesLevel::minimal);
  CHECK(
      enum_from_name<LpNamesLevel>("only_cols").value_or(LpNamesLevel::minimal)
      == LpNamesLevel::only_cols);
  CHECK(enum_from_name<LpNamesLevel>("cols_and_rows")
            .value_or(LpNamesLevel::minimal)
        == LpNamesLevel::cols_and_rows);
  CHECK_FALSE(enum_from_name<LpNamesLevel>("bogus").has_value());

  CHECK(enum_name(LpNamesLevel::minimal) == "minimal");
  CHECK(enum_name(LpNamesLevel::only_cols) == "only_cols");
  CHECK(enum_name(LpNamesLevel::cols_and_rows) == "cols_and_rows");
}

// ─── PlanningOptionsLP enum accessors
// ────────────────────────────────────────────────

TEST_CASE("PlanningOptionsLP enum accessors return correct defaults")  // NOLINT
{
  const PlanningOptionsLP opts;

  SUBCASE("method_type_enum defaults to monolithic")
  {
    CHECK(opts.method_type_enum() == MethodType::monolithic);
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

TEST_CASE("PlanningOptionsLP enum accessors parse explicit values")  // NOLINT
{
  PlanningOptions raw;
  raw.method = MethodType::sddp;
  raw.sddp_options.boundary_cuts_mode = BoundaryCutsMode::combined;
  raw.sddp_options.cut_recovery_mode = HotStartMode::append;
  raw.monolithic_options.solve_mode = SolveMode::sequential;
  raw.monolithic_options.boundary_cuts_mode = BoundaryCutsMode::noload;

  const PlanningOptionsLP opts(std::move(raw));

  CHECK(opts.method_type_enum() == MethodType::sddp);
  CHECK(opts.sddp_boundary_cuts_mode_enum() == BoundaryCutsMode::combined);
  CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::append);
  CHECK(opts.monolithic_solve_mode_enum() == SolveMode::sequential);
  CHECK(opts.monolithic_boundary_cuts_mode_enum() == BoundaryCutsMode::noload);
}

// ─── validate_enum_options ──────────────────────────────────────────────────

TEST_CASE("validate_enum_options returns empty for valid defaults")  // NOLINT
{
  const PlanningOptionsLP opts;
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("validate_enum_options returns empty for valid explicit values")
// NOLINT
{
  PlanningOptions raw;
  raw.method = MethodType::sddp;
  raw.input_format = DataFormat::csv;
  raw.output_format = DataFormat::parquet;
  raw.output_compression = CompressionCodec::gzip;
  raw.sddp_options.cut_sharing_mode = CutSharingMode::expected;
  raw.sddp_options.elastic_mode = ElasticFilterMode::multi_cut;
  raw.sddp_options.boundary_cuts_mode = BoundaryCutsMode::combined;
  raw.sddp_options.cut_recovery_mode = HotStartMode::replace;
  raw.monolithic_options.solve_mode = SolveMode::sequential;
  raw.monolithic_options.boundary_cuts_mode = BoundaryCutsMode::noload;

  const PlanningOptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE(
    "validate_enum_options returns empty for typed enum "
    "values")  // NOLINT
{
  // With typed enum fields, invalid strings are rejected at JSON parse
  // time.  Validation of already-constructed Options always succeeds.
  PlanningOptions raw;
  raw.method = MethodType::sddp;
  const PlanningOptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE(
    "validate_enum_options returns empty for multiple typed enum "
    "values")  // NOLINT
{
  // With typed enum fields, invalid strings are rejected at JSON parse
  // time.  Multiple valid enum assignments always pass validation.
  PlanningOptions raw;
  raw.method = MethodType::sddp;
  raw.input_format = DataFormat::csv;
  raw.sddp_options.cut_sharing_mode = CutSharingMode::expected;
  const PlanningOptionsLP opts(std::move(raw));
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("sddp_cut_recovery_mode explicit values")  // NOLINT
{
  SUBCASE("cut_recovery_mode=replace maps to replace")
  {
    PlanningOptions raw;
    raw.sddp_options.cut_recovery_mode = HotStartMode::replace;
    const PlanningOptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::replace);
  }

  SUBCASE("cut_recovery_mode=none maps to none")
  {
    PlanningOptions raw;
    raw.sddp_options.cut_recovery_mode = HotStartMode::none;
    const PlanningOptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::none);
  }

  SUBCASE("cut_recovery_mode=keep maps to keep")
  {
    PlanningOptions raw;
    raw.sddp_options.cut_recovery_mode = HotStartMode::keep;
    const PlanningOptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::keep);
  }

  SUBCASE("cut_recovery_mode=append maps to append")
  {
    PlanningOptions raw;
    raw.sddp_options.cut_recovery_mode = HotStartMode::append;
    const PlanningOptionsLP opts(std::move(raw));
    CHECK(opts.sddp_cut_recovery_mode_enum() == HotStartMode::append);
  }
}

// ─── StateVariableLookupMode ────────────────────────────────────────────────

TEST_CASE(
    "StateVariableLookupMode enum from_name and name round-trip")  // NOLINT
{
  CHECK(enum_from_name<StateVariableLookupMode>("warm_start")
            .value_or(StateVariableLookupMode::cross_phase)
        == StateVariableLookupMode::warm_start);
  CHECK(enum_from_name<StateVariableLookupMode>("cross_phase")
            .value_or(StateVariableLookupMode::warm_start)
        == StateVariableLookupMode::cross_phase);
  CHECK_FALSE(enum_from_name<StateVariableLookupMode>("unknown").has_value());

  CHECK(enum_name(StateVariableLookupMode::warm_start) == "warm_start");
  CHECK(enum_name(StateVariableLookupMode::cross_phase) == "cross_phase");
}

TEST_CASE("sddp_state_variable_lookup_mode default is warm_start")  // NOLINT
{
  const PlanningOptionsLP opts(PlanningOptions {});
  CHECK(opts.sddp_state_variable_lookup_mode()
        == StateVariableLookupMode::warm_start);
}

TEST_CASE("sddp_state_variable_lookup_mode cross_phase when set")  // NOLINT
{
  PlanningOptions raw;
  raw.sddp_options.state_variable_lookup_mode =
      StateVariableLookupMode::cross_phase;
  const PlanningOptionsLP opts(std::move(raw));
  CHECK(opts.sddp_state_variable_lookup_mode()
        == StateVariableLookupMode::cross_phase);
}
