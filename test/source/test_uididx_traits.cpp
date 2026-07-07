#include <arrow/api.h>
#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/uididx_traits.hpp>

using namespace gtopt;

TEST_CASE("Basic functionality 1")
{
  using namespace gtopt;
  using TestTraits = UidMapTraits<int, std::string, int>;

  // SUBCASE("Type aliases")
  {
    CHECK(std::is_same_v<TestTraits::value_type, int>);
    CHECK(std::is_same_v<TestTraits::key_type, std::tuple<std::string, int>>);
    CHECK(std::is_same_v<TestTraits::uid_map_t,
                         gtopt::flat_map<std::tuple<std::string, int>, int>>);
  }

  // SUBCASE("Map operations")
  {
    TestTraits::uid_map_t map;
    auto key = std::make_tuple("test", 42);
    map.emplace(key, 100);

    CHECK(map.size() == 1);
    CHECK(map.at(key) == 100);
  }
}

TEST_CASE("Inheritance and type traits")
{
  using TestTraits = ArrowUidTraits<std::string, int>;

  SUBCASE("Inheritance")
  {
    CHECK(std::is_base_of_v<ArrowTraits<Uid>, TestTraits>);
    CHECK(std::is_base_of_v<UidMapTraits<ArrowIndex, std::string, int>,
                            TestTraits>);
  }

  SUBCASE("make_uid_column error cases")
  {
    auto result = TestTraits::make_uid_column(nullptr, "test");
    CHECK(!result.has_value());
    CHECK(result.error() == "Null table, no column for name 'test'");
  }
}

TEST_SUITE("UidToArrowIdx")
{
  TEST_CASE("Type traits")
  {
    using TestTraits = UidToArrowIdx<ScenarioUid, StageUid, BlockUid>;

    CHECK(std::is_base_of_v<ArrowUidTraits<ScenarioUid, StageUid, BlockUid>,
                            TestTraits>);
  }
}

TEST_CASE("Scenario-Stage-Block mapping")
{
  using TestTraits = UidToVectorIdx<ScenarioUid, StageUid, BlockUid>;

  SUBCASE("Type traits")
  {
    CHECK(
        std::is_same_v<TestTraits::IndexKey, std::tuple<Index, Index, Index>>);
    CHECK(std::is_same_v<TestTraits::UidKey,
                         std::tuple<ScenarioUid, StageUid, BlockUid>>);
  }

  SUBCASE("Empty simulation")
  {
    const Simulation sim;
    const PlanningOptionsLP options;
    const SimulationLP sim_lp(sim, options);

    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->empty());
  }

  SUBCASE("Multiple entries")
  {
    Simulation sim;
    sim.scenario_array.emplace_back(Scenario {.uid = make_uid<Scenario>(1)});
    sim.scenario_array.emplace_back(Scenario {.uid = make_uid<Scenario>(2)});

    sim.stage_array.emplace_back(
        Stage {.uid = make_uid<Stage>(1), .first_block = 0, .count_block = 1});
    sim.stage_array.emplace_back(
        Stage {.uid = make_uid<Stage>(2), .first_block = 1, .count_block = 2});

    sim.block_array.emplace_back(Block {.uid = make_uid<Block>(1)});
    sim.block_array.emplace_back(Block {.uid = make_uid<Block>(2)});
    sim.block_array.emplace_back(Block {.uid = make_uid<Block>(3)});
    // Need to add blocks to test full mapping

    const PlanningOptionsLP options;
    const SimulationLP sim_lp(sim, options);

    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 6);
    CHECK(result->at(
              {make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1)})
          == std::tuple {0, 0, 0});

    auto tuid = std::tuple {
        make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3)};
    auto tidx = std::tuple {0, 1, 1};
    CHECK(as_string(result->at(tuid)) == as_string(tidx));
    CHECK(result->at(tuid) == tidx);
  }
}

TEST_CASE("Scenario-Stage mapping")
{
  using TestTraits = UidToVectorIdx<ScenarioUid, StageUid>;

  SUBCASE("Basic mapping")
  {
    Simulation sim;
    sim.scenario_array.emplace_back(Scenario {.uid = make_uid<Scenario>(1)});
    sim.stage_array.emplace_back(Stage {.uid = make_uid<Stage>(1)});

    const PlanningOptionsLP options;
    const SimulationLP sim_lp(sim, options);

    // Remove constexpr requirement since flat_map isn't constexpr
    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 1);
    CHECK(result->at(std::make_tuple(make_uid<Scenario>(1), make_uid<Stage>(1)))
          == std::make_tuple(0, 0));
  }
}

TEST_CASE("Stage mapping")
{
  using TestTraits = UidToVectorIdx<StageUid>;

  SUBCASE("Duplicate UID detection")
  {
    Simulation sim;
    sim.stage_array.emplace_back(Stage {.uid = make_uid<Stage>(1)});
    sim.stage_array.emplace_back(
        Stage {.uid = make_uid<Stage>(1)});  // Duplicate

    const PlanningOptionsLP options;
    const SimulationLP sim_lp(sim, options);

    // Remove constexpr requirement since flat_map isn't constexpr
    auto result = TestTraits::make_vector_uids_idx(sim_lp);
    CHECK(result->size() == 1);  // Only one unique UID should be stored
  }
}

TEST_CASE("Empty UID tuple")
{
  using TestTraits = UidMapTraits<int>;

  CHECK(std::is_same_v<TestTraits::key_type, std::tuple<>>);
}

TEST_CASE("Single UID type")
{
  using TestTraits = UidToVectorIdx<StageUid>;

  Simulation sim;
  sim.stage_array.emplace_back(Stage {.uid = make_uid<Stage>(42)});

  const PlanningOptionsLP options;
  const SimulationLP sim_lp(sim, options);

  // Remove constexpr requirement since flat_map isn't constexpr
  auto result = TestTraits::make_vector_uids_idx(sim_lp);
  CHECK(result->at(std::make_tuple(make_uid<Stage>(42))) == std::make_tuple(0));
}

TEST_SUITE("cast_to_int32_array")
{
  TEST_CASE("cast int32 array passthrough")
  {
    arrow::Int32Builder builder;
    REQUIRE(builder.AppendValues({10, 20, 30}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_int32_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);
    CHECK(result->Value(0) == 10);
    CHECK(result->Value(1) == 20);
    CHECK(result->Value(2) == 30);
  }

  TEST_CASE("cast int16 array to int32")
  {
    arrow::Int16Builder builder;
    REQUIRE(builder.AppendValues({100, 200, 300}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_int32_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);
    CHECK(result->Value(0) == 100);
    CHECK(result->Value(1) == 200);
    CHECK(result->Value(2) == 300);
  }

  TEST_CASE("cast int8 array to int32")
  {
    arrow::Int8Builder builder;
    REQUIRE(builder.AppendValues({1, 2, 3}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_int32_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);
    CHECK(result->Value(0) == 1);
    CHECK(result->Value(1) == 2);
    CHECK(result->Value(2) == 3);
  }

  TEST_CASE("cast int64 array to int32")
  {
    arrow::Int64Builder builder;
    REQUIRE(builder.AppendValues({100, 200, 300}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_int32_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);
    CHECK(result->Value(0) == 100);
    CHECK(result->Value(1) == 200);
    CHECK(result->Value(2) == 300);
  }

  TEST_CASE("cast incompatible type returns nullptr")
  {
    arrow::DoubleBuilder builder;
    REQUIRE(builder.AppendValues({1.0, 2.0}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_int32_array(array);
    CHECK(result == nullptr);
  }

  TEST_CASE("cast null chunk returns nullptr")
  {
    auto result = cast_to_int32_array(nullptr);
    CHECK(result == nullptr);
  }
}

TEST_SUITE("is_compatible_int32_type")
{
  TEST_CASE("compatible types")
  {
    CHECK(is_compatible_int32_type(arrow::Type::INT8));
    CHECK(is_compatible_int32_type(arrow::Type::INT16));
    CHECK(is_compatible_int32_type(arrow::Type::INT32));
    CHECK(is_compatible_int32_type(arrow::Type::INT64));
  }

  TEST_CASE("incompatible types")
  {
    CHECK_FALSE(is_compatible_int32_type(arrow::Type::DOUBLE));
    CHECK_FALSE(is_compatible_int32_type(arrow::Type::STRING));
  }
}

TEST_SUITE("make_uid_column with int16/int8 columns")
{
  TEST_CASE("make_uid_column reads int32 column")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    arrow::Int32Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({1, 2, 3}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int32())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto result = TestTraits::make_uid_column(table, "stage");
    REQUIRE(result.has_value());
    CHECK((*result)->Value(0) == 1);
    CHECK((*result)->Value(1) == 2);
    CHECK((*result)->Value(2) == 3);
  }

  TEST_CASE("make_uid_column reads int16 column as int32")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    arrow::Int16Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({10, 20, 30}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int16())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto result = TestTraits::make_uid_column(table, "stage");
    REQUIRE(result.has_value());
    CHECK((*result)->Value(0) == 10);
    CHECK((*result)->Value(1) == 20);
    CHECK((*result)->Value(2) == 30);
  }

  TEST_CASE("make_uid_column reads int8 column as int32")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    arrow::Int8Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({4, 5, 6}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int8())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto result = TestTraits::make_uid_column(table, "stage");
    REQUIRE(result.has_value());
    CHECK((*result)->Value(0) == 4);
    CHECK((*result)->Value(1) == 5);
    CHECK((*result)->Value(2) == 6);
  }

  TEST_CASE("make_uid_column reads int64 column as int32")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    arrow::Int64Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({7, 8, 9}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int64())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto result = TestTraits::make_uid_column(table, "stage");
    REQUIRE(result.has_value());
    CHECK((*result)->Value(0) == 7);
    CHECK((*result)->Value(1) == 8);
    CHECK((*result)->Value(2) == 9);
  }

  TEST_CASE("make_uid_column rejects incompatible type")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    arrow::DoubleBuilder stage_builder;
    REQUIRE(stage_builder.AppendValues({1.0, 2.0}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::float64())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto result = TestTraits::make_uid_column(table, "stage");
    CHECK_FALSE(result.has_value());
  }

  TEST_CASE("UidToArrowIdx with int16 columns")
  {
    using TestTraits = UidToArrowIdx<StageUid>;

    arrow::Int16Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({1, 2, 3}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int16())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto [result, mask] = TestTraits::make_arrow_uids_idx(table);
    REQUIRE(result != nullptr);
    CHECK(mask == 1U);
    CHECK(result->size() == 3);
    CHECK(result->at({make_uid<Stage>(1)}) == 0);
    CHECK(result->at({make_uid<Stage>(2)}) == 1);
    CHECK(result->at({make_uid<Stage>(3)}) == 2);
  }

  TEST_CASE("UidToArrowIdx with int64 columns")
  {
    using TestTraits = UidToArrowIdx<StageUid>;

    arrow::Int64Builder stage_builder;
    REQUIRE(stage_builder.AppendValues({10, 20, 30}).ok());
    std::shared_ptr<arrow::Array> stage_array;
    REQUIRE(stage_builder.Finish(&stage_array).ok());

    auto schema = arrow::schema({arrow::field("stage", arrow::int64())});
    auto table = arrow::Table::Make(schema, {stage_array});

    auto [result, mask] = TestTraits::make_arrow_uids_idx(table);
    REQUIRE(result != nullptr);
    CHECK(mask == 1U);
    CHECK(result->size() == 3);
    CHECK(result->at({make_uid<Stage>(10)}) == 0);
    CHECK(result->at({make_uid<Stage>(20)}) == 1);
    CHECK(result->at({make_uid<Stage>(30)}) == 2);
  }
}

TEST_SUITE("ArrowTraits int types use int32")
{
  TEST_CASE("ArrowTraits<int> uses int32")
  {
    CHECK(ArrowTraits<int>::type()->id() == arrow::Type::INT32);
    CHECK(std::is_same_v<ArrowTraits<int>::Type, arrow::Int32Type>);
  }

  TEST_CASE("ArrowTraits<int16_t> uses int32")
  {
    CHECK(ArrowTraits<int16_t>::type()->id() == arrow::Type::INT32);
    CHECK(std::is_same_v<ArrowTraits<int16_t>::Type, arrow::Int32Type>);
  }

  TEST_CASE("ArrowTraits<int8_t> uses int32")
  {
    CHECK(ArrowTraits<int8_t>::type()->id() == arrow::Type::INT32);
    CHECK(std::is_same_v<ArrowTraits<int8_t>::Type, arrow::Int32Type>);
  }
}

TEST_SUITE("is_compatible_double_type")
{
  TEST_CASE("compatible double types")
  {
    CHECK(is_compatible_double_type(arrow::Type::FLOAT));
    CHECK(is_compatible_double_type(arrow::Type::DOUBLE));
  }

  TEST_CASE("incompatible double types")
  {
    CHECK_FALSE(is_compatible_double_type(arrow::Type::INT32));
    CHECK_FALSE(is_compatible_double_type(arrow::Type::INT64));
    CHECK_FALSE(is_compatible_double_type(arrow::Type::STRING));
  }
}

TEST_SUITE("cast_to_double_array")
{
  TEST_CASE("cast double array passthrough")
  {
    arrow::DoubleBuilder builder;
    REQUIRE(builder.AppendValues({1.5, 2.5, 3.5}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_double_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);
    CHECK(result->Value(0) == doctest::Approx(1.5));
    CHECK(result->Value(1) == doctest::Approx(2.5));
    CHECK(result->Value(2) == doctest::Approx(3.5));
  }

  TEST_CASE("cast float array to double")
  {
    arrow::FloatBuilder builder;
    REQUIRE(builder.AppendValues({10.0F, 20.0F, 30.0F}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_double_array(array);
    REQUIRE(result != nullptr);
    CHECK(result->length() == 3);  //
    CHECK(result->Value(0) == doctest::Approx(10.0));
    CHECK(result->Value(1) == doctest::Approx(20.0));
    CHECK(result->Value(2) == doctest::Approx(30.0));
  }

  TEST_CASE("cast double incompatible type returns nullptr")
  {
    arrow::Int32Builder builder;
    REQUIRE(builder.AppendValues({1, 2}).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(builder.Finish(&array).ok());

    auto result = cast_to_double_array(array);
    CHECK(result == nullptr);
  }

  TEST_CASE("cast double null chunk returns nullptr")
  {
    auto result = cast_to_double_array(nullptr);
    CHECK(result == nullptr);
  }
}

// ── Multi-chunk + duplicate-UID regression tests ─────────────────────────
//
// Two pre-existing bugs that surfaced together when the duplicate-UID
// warning was promoted to a hard error on 2026-05-16:
//
// 1. `make_uid_column` historically returned only `column->chunk(0)`,
//    but `UidToArrowIdx<...>::make_arrow_uids_idx` then iterated
//    `table->num_rows()` calling `Value(i)`.  For multi-chunk columns
//    every `i >= chunk(0)->length()` read past the chunk buffer and
//    returned uninitialised memory (typically zero), producing spurious
//    `(Stage=0, Block=0)` keys that the now-strict duplicate check
//    flags as collisions.  The fix concatenates all chunks first.
//
// 2. The collision itself used to log a warning and silently shadow
//    the earlier row's index; now it throws ``std::runtime_error``
//    with the table columns + offending row in the error message.
//
// The tests below pin both: a multi-chunk table read produces the
// correct UIDs at every row index, and an honest duplicate (two rows
// sharing the same key) raises.

TEST_SUITE("UidToArrowIdx multi-chunk and duplicate-UID handling")
{
  TEST_CASE("make_uid_column concatenates a multi-chunk column")
  {
    using TestTraits = ArrowUidTraits<StageUid>;

    // Build two int32 arrays explicitly and assemble a Table with a
    // ChunkedArray that has TWO chunks.  Without the concat fix the
    // downstream reader returned values from chunk(0) only — indices
    // past `chunk0.length()` read past the end of the buffer.
    arrow::Int32Builder b0;
    REQUIRE(b0.AppendValues({1, 2, 3}).ok());
    std::shared_ptr<arrow::Array> a0;
    REQUIRE(b0.Finish(&a0).ok());

    arrow::Int32Builder b1;
    REQUIRE(b1.AppendValues({4, 5, 6, 7}).ok());
    std::shared_ptr<arrow::Array> a1;
    REQUIRE(b1.Finish(&a1).ok());

    auto chunked = std::make_shared<arrow::ChunkedArray>(
        arrow::ArrayVector {a0, a1}, arrow::int32());
    REQUIRE(chunked->num_chunks() == 2);

    auto schema = arrow::schema({arrow::field("stage", arrow::int32())});
    auto table = arrow::Table::Make(schema, {chunked});
    REQUIRE(table->num_rows() == 7);

    auto result = TestTraits::make_uid_column(table, "stage");
    REQUIRE(result.has_value());
    // Every index 0..6 must read the contiguous logical sequence
    // [1,2,3,4,5,6,7] — NOT zeros past chunk0's length.
    CHECK((*result)->length() == 7);
    CHECK((*result)->Value(0) == 1);
    CHECK((*result)->Value(1) == 2);
    CHECK((*result)->Value(2) == 3);
    CHECK((*result)->Value(3) == 4);
    CHECK((*result)->Value(4) == 5);
    CHECK((*result)->Value(5) == 6);
    CHECK((*result)->Value(6) == 7);
  }

  TEST_CASE(
      "UidToArrowIdx<StageUid, BlockUid> multi-chunk reads every row "
      "without spurious zero keys")
  {
    // Bug repro: a 2D (Stage, Block) table that splits into multiple
    // chunks used to produce `(Stage=0, Block=0)` for rows past
    // chunk(0)'s length.  With the concat fix every row index resolves
    // to its actual UID pair.
    using TestTraits = UidToArrowIdx<StageUid, BlockUid>;

    arrow::Int32Builder s0;
    REQUIRE(s0.AppendValues({1, 1, 1}).ok());
    std::shared_ptr<arrow::Array> sa0;
    REQUIRE(s0.Finish(&sa0).ok());
    arrow::Int32Builder s1;
    REQUIRE(s1.AppendValues({2, 2}).ok());
    std::shared_ptr<arrow::Array> sa1;
    REQUIRE(s1.Finish(&sa1).ok());
    auto stage_chunks = std::make_shared<arrow::ChunkedArray>(
        arrow::ArrayVector {sa0, sa1}, arrow::int32());

    arrow::Int32Builder b0;
    REQUIRE(b0.AppendValues({10, 11, 12}).ok());
    std::shared_ptr<arrow::Array> ba0;
    REQUIRE(b0.Finish(&ba0).ok());
    arrow::Int32Builder b1;
    REQUIRE(b1.AppendValues({13, 14}).ok());
    std::shared_ptr<arrow::Array> ba1;
    REQUIRE(b1.Finish(&ba1).ok());
    auto block_chunks = std::make_shared<arrow::ChunkedArray>(
        arrow::ArrayVector {ba0, ba1}, arrow::int32());

    auto schema = arrow::schema({
        arrow::field("stage", arrow::int32()),
        arrow::field("block", arrow::int32()),
    });
    auto table = arrow::Table::Make(schema, {stage_chunks, block_chunks});
    REQUIRE(table->num_rows() == 5);
    REQUIRE(stage_chunks->num_chunks() == 2);
    REQUIRE(block_chunks->num_chunks() == 2);

    auto [result, mask] = TestTraits::make_arrow_uids_idx(table);
    REQUIRE(result != nullptr);
    // Both `stage` and `block` columns are present → mask = 0b11.
    CHECK(mask == 3U);
    CHECK(result->size() == 5);
    // Every (Stage, Block) tuple from the contiguous logical sequence
    // must be present at the correct row index — and there must be
    // NO spurious `(0, 0)` entry from past-chunk-end reads.
    CHECK(result->at({make_uid<Stage>(1), make_uid<Block>(10)}) == 0);
    CHECK(result->at({make_uid<Stage>(1), make_uid<Block>(11)}) == 1);
    CHECK(result->at({make_uid<Stage>(1), make_uid<Block>(12)}) == 2);
    CHECK(result->at({make_uid<Stage>(2), make_uid<Block>(13)}) == 3);
    CHECK(result->at({make_uid<Stage>(2), make_uid<Block>(14)}) == 4);
    CHECK(!result->contains({make_uid<Stage>(0), make_uid<Block>(0)}));
  }

  TEST_CASE("UidToArrowIdx<StageUid, BlockUid> throws on duplicate key")
  {
    // Two rows sharing (Stage=1, Block=1) must raise: silently
    // shadowing the earlier row's index was the source of months of
    // confusing "warnings" on stale fixtures.  The new contract is a
    // hard error with a diagnostic that names the table columns.
    using TestTraits = UidToArrowIdx<StageUid, BlockUid>;

    arrow::Int32Builder sb;
    REQUIRE(sb.AppendValues({1, 1}).ok());  // both rows share Stage=1
    std::shared_ptr<arrow::Array> sa;
    REQUIRE(sb.Finish(&sa).ok());

    arrow::Int32Builder bb;
    REQUIRE(bb.AppendValues({1, 1}).ok());  // both rows share Block=1
    std::shared_ptr<arrow::Array> ba;
    REQUIRE(bb.Finish(&ba).ok());

    auto schema = arrow::schema({
        arrow::field("stage", arrow::int32()),
        arrow::field("block", arrow::int32()),
    });
    auto table = arrow::Table::Make(schema, {sa, ba});

    CHECK_THROWS_AS(TestTraits::make_arrow_uids_idx(table), std::runtime_error);
  }

  TEST_CASE(
      "UidToArrowIdx<ScenarioUid, StageUid, BlockUid> "
      "throws on duplicate key")
  {
    using TestTraits = UidToArrowIdx<ScenarioUid, StageUid, BlockUid>;

    arrow::Int32Builder scb;
    REQUIRE(scb.AppendValues({0, 0}).ok());
    std::shared_ptr<arrow::Array> sca;
    REQUIRE(scb.Finish(&sca).ok());

    arrow::Int32Builder sb;
    REQUIRE(sb.AppendValues({1, 1}).ok());
    std::shared_ptr<arrow::Array> sa;
    REQUIRE(sb.Finish(&sa).ok());

    arrow::Int32Builder bb;
    REQUIRE(bb.AppendValues({1, 1}).ok());
    std::shared_ptr<arrow::Array> ba;
    REQUIRE(bb.Finish(&ba).ok());

    auto schema = arrow::schema({
        arrow::field("scenario", arrow::int32()),
        arrow::field("stage", arrow::int32()),
        arrow::field("block", arrow::int32()),
    });
    auto table = arrow::Table::Make(schema, {sca, sa, ba});

    CHECK_THROWS_AS(TestTraits::make_arrow_uids_idx(table), std::runtime_error);
  }
}
