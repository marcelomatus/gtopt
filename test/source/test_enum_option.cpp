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
#include <gtopt/stage_enums.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── Generic framework tests ─────────────────────────────────────────────────

TEST_CASE("enum_from_name returns matching value")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto result = enum_from_name(std::span {method_type_entries}, "sddp");
  REQUIRE(result.has_value());
  CHECK(result.value_or(MethodType::monolithic) == MethodType::sddp);
}

TEST_CASE("enum_from_name returns nullopt for unknown name")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto result = enum_from_name(std::span {method_type_entries}, "bogus");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("enum_name returns canonical name for known value")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_name(std::span {method_type_entries}, MethodType::monolithic)
        == "monolithic");
  CHECK(enum_name(std::span {method_type_entries}, MethodType::sddp) == "sddp");
}

TEST_CASE("enum_name returns 'unknown' for out-of-range value")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_name(std::span {method_type_entries}, static_cast<MethodType>(99))
        == "unknown");
}

// ─── ADL-based enum_from_name<E> and enum_name(E) ──────────────────────────

TEST_CASE("ADL enum_from_name<E> and enum_name(E)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_from_name<MethodType>("monolithic") == MethodType::monolithic);
  CHECK(enum_from_name<MethodType>("sddp") == MethodType::sddp);
  CHECK_FALSE(enum_from_name<MethodType>("unknown").has_value());

  CHECK(enum_name(MethodType::monolithic) == "monolithic");
  CHECK(enum_name(MethodType::sddp) == "sddp");
}

// ─── MethodType ──────────────────────────────────────────────────────────────

TEST_CASE("MethodType from_name and name")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_from_name<SolveMode>("monolithic").value_or(SolveMode::sequential)
        == SolveMode::monolithic);
  CHECK(enum_from_name<SolveMode>("sequential").value_or(SolveMode::monolithic)
        == SolveMode::sequential);
  CHECK_FALSE(enum_from_name<SolveMode>("bad").has_value());
}

// ─── BoundaryCutsMode ────────────────────────────────────────────────────────

TEST_CASE("BoundaryCutsMode from_name")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_from_name<LpNamesLevel>("only_cols").value_or(LpNamesLevel::none)
        == LpNamesLevel::only_cols);
  CHECK(
      enum_from_name<LpNamesLevel>("cols_and_rows").value_or(LpNamesLevel::none)
      == LpNamesLevel::cols_and_rows);
  CHECK_FALSE(enum_from_name<LpNamesLevel>("bogus").has_value());
  CHECK_FALSE(enum_from_name<LpNamesLevel>("minimal").has_value());

  CHECK(enum_name(LpNamesLevel::none) == "none");
  CHECK(enum_name(LpNamesLevel::only_cols) == "only_cols");
  CHECK(enum_name(LpNamesLevel::cols_and_rows) == "cols_and_rows");
}

// ─── PlanningOptionsLP enum accessors
// ────────────────────────────────────────────────

TEST_CASE("PlanningOptionsLP enum accessors return correct defaults")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const PlanningOptionsLP opts;
  const auto warnings = opts.validate_enum_options();
  CHECK(warnings.empty());
}

TEST_CASE("validate_enum_options returns empty for valid explicit values")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const PlanningOptionsLP opts(PlanningOptions {});
  CHECK(opts.sddp_state_variable_lookup_mode()
        == StateVariableLookupMode::warm_start);
}

TEST_CASE("sddp_state_variable_lookup_mode cross_phase when set")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  PlanningOptions raw;
  raw.sddp_options.state_variable_lookup_mode =
      StateVariableLookupMode::cross_phase;
  const PlanningOptionsLP opts(std::move(raw));
  CHECK(opts.sddp_state_variable_lookup_mode()
        == StateVariableLookupMode::cross_phase);
}

// ─── MonthType: English + Spanish, full + abbrev, case-insensitive ──────────

TEST_CASE("MonthType accepts English full names, case-insensitive")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_from_name<MonthType>("january") == MonthType::january);
  CHECK(enum_from_name<MonthType>("January") == MonthType::january);
  CHECK(enum_from_name<MonthType>("JANUARY") == MonthType::january);
  CHECK(enum_from_name<MonthType>("december") == MonthType::december);
  CHECK(enum_from_name<MonthType>("December") == MonthType::december);
  CHECK(enum_from_name<MonthType>("DECEMBER") == MonthType::december);
}

TEST_CASE("MonthType accepts Spanish full names, case-insensitive")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK(enum_from_name<MonthType>("enero") == MonthType::january);
  CHECK(enum_from_name<MonthType>("Enero") == MonthType::january);
  CHECK(enum_from_name<MonthType>("ENERO") == MonthType::january);
  CHECK(enum_from_name<MonthType>("febrero") == MonthType::february);
  CHECK(enum_from_name<MonthType>("marzo") == MonthType::march);
  CHECK(enum_from_name<MonthType>("abril") == MonthType::april);
  CHECK(enum_from_name<MonthType>("mayo") == MonthType::may);
  CHECK(enum_from_name<MonthType>("junio") == MonthType::june);
  CHECK(enum_from_name<MonthType>("julio") == MonthType::july);
  CHECK(enum_from_name<MonthType>("agosto") == MonthType::august);
  CHECK(enum_from_name<MonthType>("septiembre") == MonthType::september);
  CHECK(enum_from_name<MonthType>("setiembre") == MonthType::september);
  CHECK(enum_from_name<MonthType>("octubre") == MonthType::october);
  CHECK(enum_from_name<MonthType>("noviembre") == MonthType::november);
  CHECK(enum_from_name<MonthType>("diciembre") == MonthType::december);
}

TEST_CASE("MonthType accepts 3-letter abbreviations (EN + ES)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // English abbreviations.
  CHECK(enum_from_name<MonthType>("jan") == MonthType::january);
  CHECK(enum_from_name<MonthType>("Feb") == MonthType::february);
  CHECK(enum_from_name<MonthType>("MAR") == MonthType::march);
  CHECK(enum_from_name<MonthType>("apr") == MonthType::april);
  CHECK(enum_from_name<MonthType>("may") == MonthType::may);
  CHECK(enum_from_name<MonthType>("jun") == MonthType::june);
  CHECK(enum_from_name<MonthType>("jul") == MonthType::july);
  CHECK(enum_from_name<MonthType>("aug") == MonthType::august);
  CHECK(enum_from_name<MonthType>("sep") == MonthType::september);
  CHECK(enum_from_name<MonthType>("oct") == MonthType::october);
  CHECK(enum_from_name<MonthType>("nov") == MonthType::november);
  CHECK(enum_from_name<MonthType>("dec") == MonthType::december);

  // Spanish-only abbreviations.
  CHECK(enum_from_name<MonthType>("ene") == MonthType::january);
  CHECK(enum_from_name<MonthType>("Ene") == MonthType::january);
  CHECK(enum_from_name<MonthType>("ENE") == MonthType::january);
  CHECK(enum_from_name<MonthType>("abr") == MonthType::april);
  CHECK(enum_from_name<MonthType>("ago") == MonthType::august);
  CHECK(enum_from_name<MonthType>("set") == MonthType::september);
  CHECK(enum_from_name<MonthType>("dic") == MonthType::december);
}

TEST_CASE("MonthType reverse lookup returns canonical English name")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // enum_name() walks the table in order and returns the first match by
  // value; English full names are listed first, so the canonical name
  // for MonthType::january must be "january" even though "enero", "jan"
  // and "ene" also map to it.
  CHECK(enum_name(MonthType::january) == "january");
  CHECK(enum_name(MonthType::february) == "february");
  CHECK(enum_name(MonthType::september) == "september");
  CHECK(enum_name(MonthType::december) == "december");
}

TEST_CASE("MonthType rejects unknown names")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CHECK_FALSE(enum_from_name<MonthType>("").has_value());
  CHECK_FALSE(enum_from_name<MonthType>("bogus").has_value());
  CHECK_FALSE(enum_from_name<MonthType>("januar").has_value());
  CHECK_FALSE(enum_from_name<MonthType>("januarys").has_value());
  CHECK_FALSE(enum_from_name<MonthType>("ja").has_value());
}
