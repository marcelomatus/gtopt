// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the long-direct index builders
// `UidToArrowIdx<...>::make_arrow_uids_idx_long`, which group long-layout
// rows by element uid into per-uid (axis…) -> row submaps in one scan.  These
// are the low-level structural guards complementing the end-to-end oracle in
// test_long_input_schedule.cpp.  Helpers are prefixed `lii_` for unity builds.

#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <arrow/api.h>
#include <doctest/doctest.h>
#include <gtopt/uididx_traits.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] auto lii_i32(const std::vector<int32_t>& v) -> ArrowArray
{
  arrow::Int32Builder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

[[nodiscard]] auto lii_f64(const std::vector<double>& v) -> ArrowArray
{
  arrow::DoubleBuilder b;
  REQUIRE(b.AppendValues(v).ok());
  std::shared_ptr<arrow::Array> a;
  REQUIRE(b.Finish(&a).ok());
  return a;
}

}  // namespace

TEST_CASE(
    "make_arrow_uids_idx_long STB — per-uid submaps + sparse bit")  // NOLINT
{
  // Two uids over 3 (s,t,b) cells, interleaved; uid 2 missing the last cell.
  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("scenario", arrow::int32()),
                                  arrow::field("stage", arrow::int32()),
                                  arrow::field("block", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::float64()),
                              }),
                              {
                                  lii_i32({1, 1, 1, 1, 1}),
                                  lii_i32({1, 2, 2, 1, 2}),
                                  lii_i32({1, 2, 3, 1, 2}),
                                  lii_i32({10, 10, 10, 20, 20}),
                                  lii_f64({1.0, 2.0, 3.0, 4.0, 5.0}),
                              });

  auto [groups, mask] =
      UidToArrowIdx<ScenarioUid, StageUid, BlockUid>::make_arrow_uids_idx_long(
          t);

  // Sparse-zero-fill flag set; all three axis bits present.
  CHECK((mask & kArrowSparseZeroFillBit) != 0);
  CHECK((mask & 0b111U) == 0b111U);

  REQUIRE(groups.size() == 2);
  REQUIRE(groups.contains(10));
  REQUIRE(groups.contains(20));

  const auto& g10 = groups.at(10);
  const auto& g20 = groups.at(20);
  REQUIRE(g10);
  REQUIRE(g20);

  // uid 10 has all three cells; row indices are the table row of each cell.
  using K = std::tuple<ScenarioUid, StageUid, BlockUid>;
  CHECK(
      g10->at(K {make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1)})
      == 0);
  CHECK(
      g10->at(K {make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3)})
      == 2);

  // uid 20 has only two cells; the missing one is simply absent (the runtime
  // resolves it to 0 via the sparse-zero-fill flag).
  CHECK(g20->size() == 2);
  CHECK(g20->contains(
      K {make_uid<Scenario>(1), make_uid<Stage>(1), make_uid<Block>(1)}));
  CHECK_FALSE(g20->contains(
      K {make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3)}));
}

TEST_CASE(
    "make_arrow_uids_idx_long STB — duplicate (uid,key) throws")  // NOLINT
{
  // Same (uid, scenario, stage, block) twice → duplicate within a uid.
  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("scenario", arrow::int32()),
                                  arrow::field("stage", arrow::int32()),
                                  arrow::field("block", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::float64()),
                              }),
                              {
                                  lii_i32({1, 1}),
                                  lii_i32({1, 1}),
                                  lii_i32({1, 1}),
                                  lii_i32({10, 10}),
                                  lii_f64({1.0, 2.0}),
                              });

  CHECK_THROWS_AS(
      (UidToArrowIdx<ScenarioUid, StageUid, BlockUid>::make_arrow_uids_idx_long(
          t)),
      std::runtime_error);
}

TEST_CASE(
    "make_arrow_uids_idx_long STB — broadcast (no block column)")  // NOLINT
{
  // No block column → block axis bit unset; per-stage value broadcasts.
  auto t = arrow::Table::Make(arrow::schema({
                                  arrow::field("scenario", arrow::int32()),
                                  arrow::field("stage", arrow::int32()),
                                  arrow::field("uid", arrow::int32()),
                                  arrow::field("value", arrow::float64()),
                              }),
                              {
                                  lii_i32({1, 1}),
                                  lii_i32({1, 2}),
                                  lii_i32({7, 7}),
                                  lii_f64({5.0, 6.0}),
                              });

  auto [groups, mask] =
      UidToArrowIdx<ScenarioUid, StageUid, BlockUid>::make_arrow_uids_idx_long(
          t);

  // Scenario + stage present, block absent.
  CHECK((mask & 0b001U) != 0);  // scenario
  CHECK((mask & 0b010U) != 0);  // stage
  CHECK((mask & 0b100U) == 0);  // block absent
  CHECK((mask & kArrowSparseZeroFillBit) != 0);

  REQUIRE(groups.contains(7));
  const auto& g = groups.at(7);
  // Keys carry a default (broadcast) block UID.
  using K = std::tuple<ScenarioUid, StageUid, BlockUid>;
  CHECK(
      g->contains(K {make_uid<Scenario>(1), make_uid<Stage>(1), BlockUid {}}));
  CHECK(
      g->contains(K {make_uid<Scenario>(1), make_uid<Stage>(2), BlockUid {}}));
}

TEST_CASE(
    "make_arrow_uids_idx_long TB / T arities build per-uid submaps")  // NOLINT
{
  SUBCASE("(Stage, Block)")
  {
    auto t = arrow::Table::Make(arrow::schema({
                                    arrow::field("stage", arrow::int32()),
                                    arrow::field("block", arrow::int32()),
                                    arrow::field("uid", arrow::int32()),
                                    arrow::field("value", arrow::float64()),
                                }),
                                {lii_i32({1, 2}),
                                 lii_i32({1, 2}),
                                 lii_i32({3, 3}),
                                 lii_f64({1.0, 2.0})});

    auto [groups, mask] =
        UidToArrowIdx<StageUid, BlockUid>::make_arrow_uids_idx_long(t);
    CHECK((mask & kArrowSparseZeroFillBit) != 0);
    REQUIRE(groups.contains(3));
    using K = std::tuple<StageUid, BlockUid>;
    CHECK(groups.at(3)->at(K {make_uid<Stage>(2), make_uid<Block>(2)}) == 1);
  }

  SUBCASE("(Stage)")
  {
    auto t = arrow::Table::Make(
        arrow::schema({
            arrow::field("stage", arrow::int32()),
            arrow::field("uid", arrow::int32()),
            arrow::field("value", arrow::float64()),
        }),
        {lii_i32({1, 2}), lii_i32({9, 9}), lii_f64({42.0, 99.0})});

    auto [groups, mask] = UidToArrowIdx<StageUid>::make_arrow_uids_idx_long(t);
    CHECK((mask & kArrowSparseZeroFillBit) != 0);
    REQUIRE(groups.contains(9));
    using K = std::tuple<StageUid>;
    CHECK(groups.at(9)->at(K {make_uid<Stage>(1)}) == 0);
    CHECK(groups.at(9)->at(K {make_uid<Stage>(2)}) == 1);
  }
}
