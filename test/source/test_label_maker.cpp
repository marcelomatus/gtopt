// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

[[nodiscard]] auto make_col(std::string_view class_name,
                            std::string_view variable_name,
                            Uid uid,
                            LpContext ctx,
                            bool is_state = false) -> SparseCol
{
  SparseCol col;
  col.class_name = class_name;
  col.variable_name = variable_name;
  col.variable_uid = uid;
  col.context = std::move(ctx);
  col.is_state = is_state;
  return col;
}

[[nodiscard]] auto make_row(std::string_view class_name,
                            std::string_view constraint_name,
                            Uid uid,
                            LpContext ctx) -> SparseRow
{
  SparseRow row;
  row.class_name = class_name;
  row.constraint_name = constraint_name;
  row.variable_uid = uid;
  row.context = std::move(ctx);
  return row;
}

}  // namespace

TEST_CASE("LabelMaker default construction is off")
{
  const LabelMaker maker;
  CHECK(maker.names_level() == LpNamesLevel::none);
  CHECK_FALSE(maker.col_names_enabled());
  CHECK_FALSE(maker.row_names_enabled());
  CHECK_FALSE(maker.all_col_names_enabled());
  CHECK_FALSE(maker.duplicates_are_errors());
}

TEST_CASE("LabelMaker level predicates")
{
  SUBCASE("none")
  {
    const LabelMaker maker {LpNamesLevel::none};
    CHECK_FALSE(maker.col_names_enabled());
    CHECK_FALSE(maker.row_names_enabled());
    CHECK_FALSE(maker.all_col_names_enabled());
    CHECK_FALSE(maker.duplicates_are_errors());
  }
  SUBCASE("minimal")
  {
    const LabelMaker maker {LpNamesLevel::minimal};
    CHECK(maker.col_names_enabled());
    CHECK_FALSE(maker.row_names_enabled());
    CHECK_FALSE(maker.all_col_names_enabled());
    CHECK_FALSE(maker.duplicates_are_errors());
  }
  SUBCASE("only_cols")
  {
    const LabelMaker maker {LpNamesLevel::only_cols};
    CHECK(maker.col_names_enabled());
    CHECK_FALSE(maker.row_names_enabled());
    CHECK(maker.all_col_names_enabled());
    CHECK_FALSE(maker.duplicates_are_errors());
  }
  SUBCASE("cols_and_rows")
  {
    const LabelMaker maker {LpNamesLevel::cols_and_rows};
    CHECK(maker.col_names_enabled());
    CHECK(maker.row_names_enabled());
    CHECK(maker.all_col_names_enabled());
    CHECK(maker.duplicates_are_errors());
  }
}

TEST_CASE("LabelMaker::make_col_label honors the level gate")
{
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(1));
  const auto regular =
      make_col("Bus", "theta", Uid {3}, ctx, /*is_state*/ false);
  const auto state = make_col("Bus", "eini", Uid {5}, ctx, /*is_state*/ true);

  SUBCASE("none emits nothing")
  {
    const LabelMaker maker {LpNamesLevel::none};
    CHECK(maker.make_col_label(regular).empty());
    CHECK(maker.make_col_label(state).empty());
  }
  SUBCASE("minimal emits state cols only")
  {
    const LabelMaker maker {LpNamesLevel::minimal};
    CHECK(maker.make_col_label(regular).empty());
    CHECK(maker.make_col_label(state) == "bus_eini_5_0_1");
  }
  SUBCASE("only_cols emits every column")
  {
    const LabelMaker maker {LpNamesLevel::only_cols};
    CHECK(maker.make_col_label(regular) == "bus_theta_3_0_1");
    CHECK(maker.make_col_label(state) == "bus_eini_5_0_1");
  }
  SUBCASE("cols_and_rows emits every column")
  {
    const LabelMaker maker {LpNamesLevel::cols_and_rows};
    CHECK(maker.make_col_label(regular) == "bus_theta_3_0_1");
  }
}

TEST_CASE("LabelMaker::make_col_label explicit name override wins")
{
  const LabelMaker maker {LpNamesLevel::only_cols};
  SparseCol col;
  col.name = "custom";
  col.class_name = "Bus";
  col.variable_name = "theta";
  col.variable_uid = Uid {7};
  CHECK(maker.make_col_label(col) == "custom");
}

TEST_CASE(
    "LabelMaker::make_col_label without context falls back to class+var+uid")
{
  const LabelMaker maker {LpNamesLevel::only_cols};
  SparseCol col;
  col.class_name = "Generator";
  col.variable_name = "gen";
  col.variable_uid = Uid {42};
  CHECK(maker.make_col_label(col) == "generator_gen_42");
}

TEST_CASE("LabelMaker::make_row_label requires cols_and_rows")
{
  const auto ctx = make_block_context(
      make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3));
  const auto row = make_row("Bus", "bal", Uid {4}, ctx);

  SUBCASE("none / minimal / only_cols emit nothing")
  {
    for (auto lvl :
         {LpNamesLevel::none, LpNamesLevel::minimal, LpNamesLevel::only_cols})
    {
      const LabelMaker maker {lvl};
      CHECK(maker.make_row_label(row).empty());
    }
  }
  SUBCASE("cols_and_rows emits the label")
  {
    const LabelMaker maker {LpNamesLevel::cols_and_rows};
    CHECK(maker.make_row_label(row) == "bus_bal_4_1_2_3");
  }
}

TEST_CASE("LabelMaker::force_col_label ignores level")
{
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(1));
  const auto col = make_col("Bus", "theta", Uid {3}, ctx);
  CHECK(LabelMaker::force_col_label(col) == "bus_theta_3_0_1");

  SparseCol named;
  named.name = "custom";
  CHECK(LabelMaker::force_col_label(named) == "custom");
}

TEST_CASE("LabelMaker::force_row_label ignores level")
{
  const auto ctx = make_block_context(
      make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3));
  const auto row = make_row("Bus", "bal", Uid {4}, ctx);
  CHECK(LabelMaker::force_row_label(row) == "bus_bal_4_1_2_3");
}

TEST_CASE("LabelMaker::make_col_label empty class and name → empty")
{
  const LabelMaker maker {LpNamesLevel::only_cols};
  const SparseCol col {};
  CHECK(maker.make_col_label(col).empty());
}
