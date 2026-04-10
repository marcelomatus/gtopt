// SPDX-License-Identifier: BSD-3-Clause
#include <unordered_set>

#include <doctest/doctest.h>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LpContext type aliases")
{
  SUBCASE("StageContext is a 2-tuple")
  {
    const auto ctx = StageContext {ScenarioUid {0}, StageUid {1}};
    CHECK(std::get<0>(ctx) == 0);
    CHECK(std::get<1>(ctx) == 1);
  }

  SUBCASE("BlockContext is a 3-tuple")
  {
    const auto ctx = BlockContext {ScenarioUid {0}, StageUid {1}, BlockUid {2}};
    CHECK(std::get<0>(ctx) == 0);
    CHECK(std::get<1>(ctx) == 1);
    CHECK(std::get<2>(ctx) == 2);
  }
}

TEST_CASE("make_stage_context factory")
{
  SUBCASE("accepts strong UID types")
  {
    const auto ctx = make_stage_context(ScenarioUid {3}, StageUid {7});
    CHECK(std::get<0>(ctx) == 3);
    CHECK(std::get<1>(ctx) == 7);
  }

  SUBCASE("result is a StageContext")
  {
    const StageContext ctx = make_stage_context(ScenarioUid {0}, StageUid {0});
    CHECK(std::get<0>(ctx) == 0);
    CHECK(std::get<1>(ctx) == 0);
  }
}

TEST_CASE("make_block_context factory")
{
  SUBCASE("accepts strong UID types")
  {
    const auto ctx =
        make_block_context(ScenarioUid {1}, StageUid {2}, BlockUid {3});
    CHECK(std::get<0>(ctx) == 1);
    CHECK(std::get<1>(ctx) == 2);
    CHECK(std::get<2>(ctx) == 3);
  }
}

TEST_CASE("TupleHash")
{
  SUBCASE("hashes StageContext")
  {
    const TupleHash hasher;
    const auto a = StageContext {ScenarioUid {0}, StageUid {1}};
    const auto b = StageContext {ScenarioUid {0}, StageUid {1}};
    const auto c = StageContext {ScenarioUid {0}, StageUid {2}};

    CHECK(hasher(a) == hasher(b));
    CHECK(hasher(a) != hasher(c));
  }

  SUBCASE("hashes BlockContext")
  {
    const TupleHash hasher;
    const auto a = BlockContext {ScenarioUid {0}, StageUid {1}, BlockUid {2}};
    const auto b = BlockContext {ScenarioUid {0}, StageUid {1}, BlockUid {2}};
    const auto c = BlockContext {ScenarioUid {0}, StageUid {1}, BlockUid {3}};

    CHECK(hasher(a) == hasher(b));
    CHECK(hasher(a) != hasher(c));
  }

  SUBCASE("usable as key in unordered_set")
  {
    std::unordered_set<BlockContext, TupleHash> contexts;
    contexts.insert(
        make_block_context(ScenarioUid {0}, StageUid {0}, BlockUid {0}));
    contexts.insert(
        make_block_context(ScenarioUid {0}, StageUid {0}, BlockUid {1}));
    contexts.insert(make_block_context(
        ScenarioUid {0}, StageUid {0}, BlockUid {0}));  // duplicate

    CHECK(contexts.size() == 2);
  }
}

TEST_CASE("LpContext variant")
{
  SUBCASE("default is monostate")
  {
    const LpContext ctx {};
    CHECK(std::holds_alternative<std::monostate>(ctx));
  }

  SUBCASE("holds StageContext")
  {
    const LpContext ctx = StageContext {ScenarioUid {0}, StageUid {1}};
    CHECK(std::holds_alternative<StageContext>(ctx));
    const auto& sc = std::get<StageContext>(ctx);
    CHECK(std::get<0>(sc) == 0);
    CHECK(std::get<1>(sc) == 1);
  }

  SUBCASE("holds BlockContext")
  {
    const LpContext ctx =
        BlockContext {ScenarioUid {0}, StageUid {1}, BlockUid {2}};
    CHECK(std::holds_alternative<BlockContext>(ctx));
  }
}

TEST_CASE("SparseCol context field")
{
  SUBCASE("default context is monostate")
  {
    const SparseCol col {};
    CHECK(std::holds_alternative<std::monostate>(col.context));
  }

  SUBCASE("block context via designated initializer")
  {
    const auto ctx =
        make_block_context(ScenarioUid {0}, StageUid {1}, BlockUid {2});
    const SparseCol col {
        .class_name = "Bus",
        .variable_name = "theta",
        .variable_uid = Uid {5},
        .context = ctx,
    };
    CHECK(std::holds_alternative<BlockContext>(col.context));
    const auto& bc = std::get<BlockContext>(col.context);
    CHECK(std::get<0>(bc) == 0);
    CHECK(std::get<1>(bc) == 1);
    CHECK(std::get<2>(bc) == 2);
  }

  SUBCASE("stage context via designated initializer")
  {
    const SparseCol col {
        .context = make_stage_context(ScenarioUid {3}, StageUid {4}),
    };
    CHECK(std::holds_alternative<StageContext>(col.context));
  }
}

TEST_CASE("SparseRow context field")
{
  SUBCASE("default context is monostate")
  {
    const SparseRow row {};
    CHECK(std::holds_alternative<std::monostate>(row.context));
  }

  SUBCASE("block context via designated initializer")
  {
    const auto ctx =
        make_block_context(ScenarioUid {0}, StageUid {0}, BlockUid {1});
    auto row =
        SparseRow {
            .context = ctx,
        }
            .equal(0);
    CHECK(std::holds_alternative<BlockContext>(row.context));
    CHECK(row.lowb == doctest::Approx(0.0));
    CHECK(row.uppb == doctest::Approx(0.0));
  }

  SUBCASE("metadata fields for lazy name generation")
  {
    const auto ctx =
        make_block_context(ScenarioUid {0}, StageUid {1}, BlockUid {2});
    auto row =
        SparseRow {
            .class_name = "Bus",
            .constraint_name = "bal",
            .variable_uid = Uid {3},
            .context = ctx,
        }
            .equal(0);
    CHECK(row.class_name == "Bus");
    CHECK(row.constraint_name == "bal");
    CHECK(row.variable_uid == Uid {3});
    CHECK(std::holds_alternative<BlockContext>(row.context));

    // Verify LabelMaker::force_row_label produces the expected label
    // from row metadata + context.
    CHECK(LabelMaker::force_row_label(row) == "bus_bal_3_0_1_2");
  }
}
