// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_label_maker_styles.cpp
 * @brief     Unit tests for `LabelMaker` × `LpLabelStyle` (issue #508).
 *
 * Covers:
 *   - Default ctor produces the compact (UID) form — byte-identical
 *     to pre-#508 fingerprints.
 *   - `LpLabelStyle::compact` without a cache pointer behaves
 *     identically to the legacy single-arg ctor.
 *   - `LpLabelStyle::extended` with a `nullptr` cache falls back to
 *     the compact form per-label.  Establishes the zero-cost
 *     invariant for runs that opt in but never wire a cache.
 *   - `LabelMaker` carries the style + cache pointer reflectively
 *     (`label_style()` / `ascii_name_cache()` accessors stay in sync
 *     with the ctor arguments).
 *
 * The cache-population path lives behind `SimulationLP` and is exercised
 * by the integration suite; this file is the unit-level guard for the
 * format-branch invariants.
 */

#include <doctest/doctest.h>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Wrap in a uniquely-named outer namespace so the CMake unity build does
// not collide with the same-named helpers in `test_label_maker.cpp`.  See
// `memory/feedback_unity_anon_namespace.md`.
namespace test_label_maker_styles_508
{
namespace
{

[[nodiscard]] auto make_col_styles(std::string_view class_name,
                                   std::string_view variable_name,
                                   Uid uid,
                                   LpContext ctx) -> SparseCol
{
  SparseCol col;
  col.class_name = class_name;
  col.variable_name = variable_name;
  col.variable_uid = uid;
  col.context = std::move(ctx);
  return col;
}

[[nodiscard]] auto make_row_styles(std::string_view class_name,
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

TEST_CASE("LabelMaker default style is compact and cache is null")
{
  const LabelMaker maker {LpNamesLevel::all};
  CHECK(maker.label_style() == LpLabelStyle::compact);
  CHECK(maker.ascii_name_cache() == nullptr);
}

TEST_CASE("LabelMaker compact + null cache is byte-identical to legacy ctor")
{
  // The hard fingerprint invariant: under `compact` (default), every
  // label produced by the new 3-arg ctor must match the label produced
  // by the legacy 1-arg ctor.  This is the test we look at first when
  // a fingerprint regression appears.
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(1));
  const auto col = make_col_styles("Line", "overloadp", Uid {307}, ctx);
  const auto row = make_row_styles("Line", "kvl", Uid {307}, ctx);

  const LabelMaker legacy {LpNamesLevel::all};
  const LabelMaker explicit_compact {
      LpNamesLevel::all, LpLabelStyle::compact, nullptr};

  CHECK(explicit_compact.make_col_label(col) == legacy.make_col_label(col));
  CHECK(explicit_compact.make_row_label(row) == legacy.make_row_label(row));
}

TEST_CASE("LabelMaker extended with null cache falls back to compact")
{
  // Establishes the per-label fallback rule from §4.4 of the design
  // doc: when the cache pointer is `nullptr` (the situation in every
  // unit test that does not wire a SimulationLP), `extended` produces
  // the compact form.  Same zero-leak guarantee applies whenever the
  // cache misses an `(class, uid)` pair.
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(1));
  const auto col = make_col_styles("Line", "overloadp", Uid {307}, ctx);

  const LabelMaker compact {LpNamesLevel::all, LpLabelStyle::compact, nullptr};
  const LabelMaker extended_null {
      LpNamesLevel::all, LpLabelStyle::extended, nullptr};

  CHECK(extended_null.make_col_label(col) == compact.make_col_label(col));
}

TEST_CASE("LabelMaker reflects ctor arguments")
{
  const LabelMaker maker {LpNamesLevel::all, LpLabelStyle::extended, nullptr};
  CHECK(maker.names_level() == LpNamesLevel::all);
  CHECK(maker.label_style() == LpLabelStyle::extended);
  CHECK(maker.ascii_name_cache() == nullptr);
}

TEST_CASE("LabelMaker compact label format pin (regression catch)")
{
  // Locks in the exact byte form of the compact label so any future
  // refactor that accidentally changes the id segment fails loudly
  // here, before fingerprint streams diverge silently.
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(1));
  const auto col = make_col_styles("Line", "overloadp", Uid {307}, ctx);

  const LabelMaker maker {LpNamesLevel::all, LpLabelStyle::compact, nullptr};
  CHECK(maker.make_col_label(col) == "line_overloadp_307_0_1");
}

}  // namespace test_label_maker_styles_508
