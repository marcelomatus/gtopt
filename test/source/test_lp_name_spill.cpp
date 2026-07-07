// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/lp_context.hpp>
#include <gtopt/lp_name_spill.hpp>

using namespace gtopt;

namespace test_lp_name_spill_ns
{

namespace
{
[[nodiscard]] std::filesystem::path scratch_dir(const char* tag)
{
  auto d = std::filesystem::temp_directory_path()
      / (std::string("gtopt_lp_name_spill_") + tag);
  std::error_code ec;
  std::filesystem::remove_all(d, ec);
  return d;
}

[[nodiscard]] SparseColLabel col_label(std::string_view cls,
                                       std::string_view var,
                                       int uid,
                                       const LpContext& ctx)
{
  return SparseColLabel {
      .class_name = cls,
      .variable_name = var,
      .variable_uid = Uid {uid},
      .context = ctx,
  };
}

[[nodiscard]] SparseRowLabel row_label(std::string_view cls,
                                       std::string_view con,
                                       int uid,
                                       const LpContext& ctx)
{
  return SparseRowLabel {
      .class_name = cls,
      .constraint_name = con,
      .variable_uid = Uid {uid},
      .context = ctx,
  };
}
}  // namespace

TEST_CASE("LpNameSpillStore round-trips col/row label metadata")  // NOLINT
{
  const auto dir = scratch_dir("rt");

  const LpContext ctx_a {
      make_stage_context(make_uid<Scenario>(1), make_uid<Stage>(2))};
  const LpContext ctx_b {make_block_context(
      make_uid<Scenario>(1), make_uid<Stage>(2), make_uid<Block>(3))};

  LpLabelMeta meta;
  meta.col_labels = {
      col_label("Generator", "pmax", 1, ctx_a),
      col_label("Generator", "pmax", 2, ctx_b),
      col_label("Line", "flow", 3, {}),
  };
  meta.row_labels = {
      row_label("Bus", "balance", 1, ctx_a),
      row_label("Generator", "capacity", 2, ctx_b),
  };

  {
    LpNameSpillStore store(dir);
    // The directory is created eagerly.
    CHECK(std::filesystem::exists(dir));

    store.spill("s0_p1_a0", meta);
    store.drain();  // force the async write to flush

    const auto* loaded = store.load("s0_p1_a0");
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->col_labels.size() == 3);
    REQUIRE(loaded->row_labels.size() == 2);
    // Field-by-field equality (string_view, uid, and the opaque context blob).
    CHECK(loaded->col_labels[0] == meta.col_labels[0]);
    CHECK(loaded->col_labels[1] == meta.col_labels[1]);
    CHECK(loaded->col_labels[2] == meta.col_labels[2]);
    CHECK(loaded->row_labels[0] == meta.row_labels[0]);
    CHECK(loaded->row_labels[1] == meta.row_labels[1]);

    // Second load is served from cache (still equal).
    const auto* again = store.load("s0_p1_a0");
    REQUIRE(again != nullptr);
    CHECK(again->col_labels[0] == meta.col_labels[0]);

    // An unknown key never spilled returns nullptr.
    CHECK(store.load("never_spilled") == nullptr);
  }

  // Destruction removes the spill directory.
  CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE(
    "LpNameSpillStore load blocks until the async spill flushes")  // NOLINT
{
  const auto dir = scratch_dir("async");
  LpNameSpillStore store(dir);

  LpLabelMeta a;
  a.col_labels = {col_label("A", "x", 10, {}), col_label("A", "y", 11, {})};
  a.row_labels = {row_label("Ar", "rx", 12, {})};
  LpLabelMeta b;
  b.col_labels = {col_label("B", "z", 20, {})};
  b.row_labels = {
      row_label("Br", "r0", 21, {}),
      row_label("Br", "r1", 22, {}),
      row_label("Br", "r2", 23, {}),
  };

  // No drain(): load() must wait for the worker to finish each key.
  store.spill("k_a", a);
  store.spill("k_b", b);

  const auto* la = store.load("k_a");
  REQUIRE(la != nullptr);
  CHECK(la->col_labels.size() == 2);
  CHECK(la->row_labels.size() == 1);
  CHECK(la->col_labels[1] == a.col_labels[1]);

  const auto* lb = store.load("k_b");
  REQUIRE(lb != nullptr);
  CHECK(lb->col_labels.size() == 1);
  CHECK(lb->row_labels.size() == 3);
  CHECK(lb->row_labels[2] == b.row_labels[2]);
}

TEST_CASE("LpNameSpillStore handles empty and large metadata sets")  // NOLINT
{
  const auto dir = scratch_dir("edge");
  LpNameSpillStore store(dir);

  // Empty metadata round-trips to an empty (but non-null) result.
  store.spill("empty", LpLabelMeta {});
  const auto* e = store.load("empty");
  REQUIRE(e != nullptr);
  CHECK(e->empty());

  // A larger, highly-redundant set (repeated class/name — Parquet's
  // dictionary encoding + zstd should shrink it).
  LpLabelMeta big;
  for (int i = 0; i < 5000; ++i) {
    big.col_labels.push_back(col_label("Generator", "generation_cost", i, {}));
    big.row_labels.push_back(row_label("Bus", "balance", i, {}));
  }
  store.spill("big", big);
  const auto* lb = store.load("big");
  REQUIRE(lb != nullptr);
  CHECK(lb->col_labels.size() == 5000);
  CHECK(lb->row_labels.size() == 5000);
  CHECK(lb->col_labels.front() == big.col_labels.front());
  CHECK(lb->col_labels.back() == big.col_labels.back());
  CHECK(lb->row_labels.back() == big.row_labels.back());
  // The interned string_views must remain valid after the cache move.
  CHECK(lb->col_labels.front().class_name == "Generator");
  CHECK(lb->row_labels.back().constraint_name == "balance");
}

}  // namespace test_lp_name_spill_ns
