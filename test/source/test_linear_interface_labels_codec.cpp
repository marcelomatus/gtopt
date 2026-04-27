/**
 * @file      test_linear_interface_labels_codec.cpp
 * @brief     Direct unit tests for the LP label-meta byte codec.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Drives `serialize_labels_meta` / `deserialize_*_labels_meta` head-on,
 * with all `LpContext` variant arms.  These tests fire even when no
 * LinearInterface / solver backend is involved — they are the tightest
 * regression net we can wrap around the byte layout without spinning up
 * a full LP.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface_labels_codec.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
// Build a vector of column labels covering every `LpContext` variant arm
// plus a bare monostate entry.  Each label uses distinct strings so that
// the deserializer's per-entry string-pool indexing is also exercised.
std::vector<SparseColLabel> make_col_labels()
{
  std::vector<SparseColLabel> v;
  v.push_back(SparseColLabel {
      .class_name = "Bare",
      .variable_name = "alpha",
      .variable_uid = Uid {1},
  });
  v.push_back(SparseColLabel {
      .class_name = "Stage",
      .variable_name = "p",
      .variable_uid = Uid {2},
      .context = make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7)),
  });
  v.push_back(SparseColLabel {
      .class_name = "Block",
      .variable_name = "p",
      .variable_uid = Uid {3},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11)),
  });
  v.push_back(SparseColLabel {
      .class_name = "BlockEx",
      .variable_name = "p",
      .variable_uid = Uid {4},
      .context = make_block_context(make_uid<Scenario>(3),
                                    make_uid<Stage>(7),
                                    make_uid<Block>(11),
                                    /*extra=*/2),
  });
  v.push_back(SparseColLabel {
      .class_name = "ScenePhase",
      .variable_name = "alpha",
      .variable_uid = Uid {5},
      .context =
          make_scene_phase_context(make_uid<Scene>(1), make_uid<Phase>(2)),
  });
  v.push_back(SparseColLabel {
      .class_name = "Iteration",
      .variable_name = "cut",
      .variable_uid = Uid {6},
      .context = make_iteration_context(make_uid<Scene>(1),
                                        make_uid<Phase>(2),
                                        make_uid<Iteration>(4),
                                        /*extra=*/0),
  });
  v.push_back(SparseColLabel {
      .class_name = "Aperture",
      .variable_name = "aperture",
      .variable_uid = Uid {7},
      // ApertureUid is an alias for ScenarioUid in the codebase.
      .context = make_aperture_context(make_uid<Scene>(1),
                                       make_uid<Phase>(2),
                                       make_uid<Scenario>(3),
                                       /*extra=*/0),
  });
  return v;
}

std::vector<SparseRowLabel> make_row_labels()
{
  std::vector<SparseRowLabel> v;
  v.push_back(SparseRowLabel {
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {100},
  });
  v.push_back(SparseRowLabel {
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {101},
      .context = make_stage_context(make_uid<Scenario>(3), make_uid<Stage>(7)),
  });
  v.push_back(SparseRowLabel {
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {102},
      .context = make_block_context(
          make_uid<Scenario>(3), make_uid<Stage>(7), make_uid<Block>(11)),
  });
  return v;
}
}  // namespace

TEST_CASE("labels_codec — empty vector round-trips to empty buffer")  // NOLINT
{
  const std::vector<SparseColLabel> empty_cols;
  const auto bytes_cols = serialize_labels_meta(empty_cols);
  CHECK(bytes_cols.empty());

  std::vector<std::string> pool;
  const auto out_cols = deserialize_col_labels_meta({bytes_cols.data(), 0},
                                                    /*num_entries=*/0,
                                                    pool);
  CHECK(out_cols.empty());
  CHECK(pool.empty());

  const std::vector<SparseRowLabel> empty_rows;
  const auto bytes_rows = serialize_labels_meta(empty_rows);
  CHECK(bytes_rows.empty());

  pool.clear();
  const auto out_rows = deserialize_row_labels_meta({bytes_rows.data(), 0},
                                                    /*num_entries=*/0,
                                                    pool);
  CHECK(out_rows.empty());
  CHECK(pool.empty());
}

TEST_CASE("labels_codec — col labels round-trip across every LpContext arm")
// NOLINT
{
  const auto cols = make_col_labels();
  const auto bytes = serialize_labels_meta(cols);
  REQUIRE_FALSE(bytes.empty());

  // Pool sizing per the contract: 2 string_views per entry must not
  // realloc; reserve generously so push_back never invalidates.
  std::vector<std::string> pool;
  pool.reserve(cols.size() * 2);

  const auto round_tripped = deserialize_col_labels_meta(
      {bytes.data(), bytes.size()}, cols.size(), pool);

  REQUIRE(round_tripped.size() == cols.size());
  for (size_t i = 0; i < cols.size(); ++i) {
    CAPTURE(i);
    CHECK(round_tripped[i].class_name == cols[i].class_name);
    CHECK(round_tripped[i].variable_name == cols[i].variable_name);
    CHECK(round_tripped[i].variable_uid == cols[i].variable_uid);
    CHECK(round_tripped[i].context == cols[i].context);
  }
}

TEST_CASE("labels_codec — row labels round-trip across context arms")  // NOLINT
{
  const auto rows = make_row_labels();
  const auto bytes = serialize_labels_meta(rows);
  REQUIRE_FALSE(bytes.empty());

  std::vector<std::string> pool;
  pool.reserve(rows.size() * 2);

  const auto round_tripped = deserialize_row_labels_meta(
      {bytes.data(), bytes.size()}, rows.size(), pool);

  REQUIRE(round_tripped.size() == rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    CAPTURE(i);
    CHECK(round_tripped[i].class_name == rows[i].class_name);
    CHECK(round_tripped[i].constraint_name == rows[i].constraint_name);
    CHECK(round_tripped[i].variable_uid == rows[i].variable_uid);
    CHECK(round_tripped[i].context == rows[i].context);
  }
}

TEST_CASE("labels_codec — pool stays non-empty for non-empty input")  // NOLINT
{
  // Pool growth is observable: every entry contributes 2 strings.
  const auto cols = make_col_labels();
  const auto bytes = serialize_labels_meta(cols);

  std::vector<std::string> pool;
  pool.reserve(cols.size() * 2);

  const auto out = deserialize_col_labels_meta(
      {bytes.data(), bytes.size()}, cols.size(), pool);
  REQUIRE(out.size() == cols.size());
  // class_name + variable_name → 2 entries per label.
  CHECK(pool.size() == cols.size() * 2);
}

TEST_CASE("labels_codec — is_empty_*_label classifies sentinel rows")  // NOLINT
{
  // Default-constructed labels have unknown_uid + monostate context and
  // empty strings — `is_empty_*_label` must report `true` so the
  // duplicate-detection maps skip them.
  CHECK(is_empty_col_label(SparseColLabel {}));
  CHECK(is_empty_row_label(SparseRowLabel {}));

  // Any single field populated breaks the empty-label predicate.
  SparseColLabel with_class;
  with_class.class_name = "Gen";
  CHECK_FALSE(is_empty_col_label(with_class));

  SparseColLabel with_var;
  with_var.variable_name = "p";
  CHECK_FALSE(is_empty_col_label(with_var));

  SparseColLabel with_uid;
  with_uid.variable_uid = Uid {1};
  CHECK_FALSE(is_empty_col_label(with_uid));

  SparseColLabel with_context;
  with_context.context =
      make_stage_context(make_uid<Scenario>(1), make_uid<Stage>(1));
  CHECK_FALSE(is_empty_col_label(with_context));

  SparseRowLabel row_with_constraint;
  row_with_constraint.constraint_name = "balance";
  CHECK_FALSE(is_empty_row_label(row_with_constraint));
}
