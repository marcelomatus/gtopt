// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_clone_invariants.cpp
 * @brief     Invariants that `LinearInterface::clone()` must preserve
 *            across the upcoming `shared_ptr<T>` + `CloneKind::shallow`
 *            refactor.
 *
 * These tests pin down the contract clone() satisfies TODAY (deep copy
 * of all metadata).  After Phase 4/5 lands, the same tests must still
 * pass for `clone(CloneKind::deep)` AND for `clone(CloneKind::shallow)`
 * — the difference between the two clone modes is supposed to be
 * memory-only, not behavioural.  If any of these tests fails after
 * the refactor, it points precisely at the regression: the wrong
 * member was missed when wiring up the shallow path, or the shared
 * metadata leaked a write across clones.
 *
 *   1. Clone preserves meta — `write_lp` on source and freshly-cloned
 *      LP yields byte-identical output.  Locks in label replication.
 *
 *   2. Clone + disposable adds + write_lp — the elastic-filter
 *      pattern, end-to-end on a cloned LP.  Adds disposable
 *      slack/fix on the clone and verifies the per-clone-local label
 *      fall-through works.  Fails first if Phase 4 forgets to wire
 *      `m_post_clone_*_metas_` on the clone or if Phase 5 breaks
 *      label generation for shallow clones.
 *
 *   3. Clone is independent — mutating bounds/objective on the clone
 *      does NOT affect the source.  Fundamental aperture-safety
 *      property; if shallow clone shares the wrong thing, this fails.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

std::string read_file(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Build a small labelled LP that mirrors a typical post-flatten
// state: a few production cols / rows with class_name + variable_name
// + variable_uid set, so `write_lp` exercises the
// `generate_labels_from_maps` synthesis path.
LinearInterface make_labelled_lp()
{
  LinearInterface li;
  std::ignore = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  std::ignore = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 2,
  });
  SparseRow r {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = 1,
  };
  r[ColIndex {0}] = 1.0;
  r[ColIndex {1}] = 1.0;
  std::ignore = li.add_row(r);
  return li;
}

}  // namespace

TEST_CASE("clone preserves shared metadata — write_lp byte-equivalent")
{
  const auto src = make_labelled_lp();
  const auto cloned = src.clone();

  REQUIRE(cloned.get_numcols() == src.get_numcols());
  REQUIRE(cloned.get_numrows() == src.get_numrows());

  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto src_stem = (tmpdir / "test_clone_src").string();
  const auto clone_stem = (tmpdir / "test_clone_clone").string();

  REQUIRE(src.write_lp(src_stem).has_value());
  REQUIRE(cloned.write_lp(clone_stem).has_value());

  const auto src_content = read_file(src_stem + ".lp");
  const auto clone_content = read_file(clone_stem + ".lp");

  // Solver-emitted .lp files contain a header line with the problem
  // name (which the backend may stamp differently per clone).  Strip
  // any line that starts with "\\Problem name" before comparing so
  // the test isn't sensitive to that.
  auto strip_problem_header = [](std::string s)
  {
    const std::string marker = "\\Problem name";
    auto p = s.find(marker);
    if (p == std::string::npos) {
      return s;
    }
    auto end = s.find('\n', p);
    if (end == std::string::npos) {
      return s.substr(0, p);
    }
    s.erase(p, end - p + 1);
    return s;
  };

  CHECK(strip_problem_header(src_content)
        == strip_problem_header(clone_content));

  std::error_code ec;
  std::filesystem::remove(src_stem + ".lp", ec);
  std::filesystem::remove(clone_stem + ".lp", ec);
}

TEST_CASE(
    "clone + add_col_disposable + add_row_disposable + write_lp "
    "produces gtopt-formatted labels for the disposable inserts")
{
  // Mirrors the elastic-filter call shape: clone the source LP, add a
  // pair of slack cols and a fixing row on the clone via the
  // disposable APIs, write the clone's LP and verify the labels.
  const auto src = make_labelled_lp();
  auto cloned = src.clone();

  // Add disposable slacks (mirrors benders_cut.cpp:536, 544) — note
  // that the elastic filter uses `class_name = sddp_alpha_class_name`
  // ("sddp_alpha"), but for the test we use a distinct class so we
  // can grep the dump unambiguously.
  const auto sup = cloned.add_col_disposable(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
      .class_name = "ElasticTest",
      .variable_name = "elastic_sup",
      .variable_uid = 100,
  });
  const auto sdn = cloned.add_col_disposable(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
      .class_name = "ElasticTest",
      .variable_name = "elastic_sdn",
      .variable_uid = 100,
  });

  // Disposable fixing row referencing the slacks (mirrors
  // benders_cut.cpp:586-597).
  SparseRow fix {
      .lowb = 7.5,
      .uppb = 7.5,
      .class_name = "ElasticTest",
      .constraint_name = "elastic_fix",
      .variable_uid = 100,
  };
  fix[ColIndex {0}] = 1.0;  // dep — first production col
  fix[sup] = 1.0;
  fix[sdn] = -1.0;
  std::ignore = cloned.add_row_disposable(fix);

  CHECK(cloned.get_numcols() == src.get_numcols() + 2);
  CHECK(cloned.get_numrows() == src.get_numrows() + 1);

  // Source must NOT have grown — clone is independent.
  CHECK(src.get_numcols() == 2);
  CHECK(src.get_numrows() == 1);

  // Write the clone's LP and verify labels.
  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto stem = (tmpdir / "test_clone_disposable_dump").string();
  REQUIRE(cloned.write_lp(stem).has_value());

  const auto content = read_file(stem + ".lp");
  // Production cols/rows preserved on the clone via shared meta —
  // class_name "Gen" lowercased to "gen", "Bus" → "bus".
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("bus") != std::string::npos);
  // Disposable cols/rows formatted via per-clone-local extras —
  // "ElasticTest" lowercased to "elastictest".
  CHECK(content.find("elastictest") != std::string::npos);
  CHECK(content.find("elastic_sup") != std::string::npos);
  CHECK(content.find("elastic_sdn") != std::string::npos);
  CHECK(content.find("elastic_fix") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}

TEST_CASE("clone is independent — bound mutations on clone don't reach source")
{
  auto src = make_labelled_lp();
  auto cloned = src.clone();

  const auto src_low_before = src.get_col_low()[ColIndex {0}];
  const auto src_upp_before = src.get_col_upp()[ColIndex {0}];

  // Mutate bounds on the clone — this is exactly what aperture tasks
  // do via `e.update_aperture(clone, ...)` (sddp_aperture.cpp:294).
  cloned.set_col_low(ColIndex {0}, 3.0);
  cloned.set_col_upp(ColIndex {0}, 7.0);

  // Source must be untouched.
  CHECK(src.get_col_low()[ColIndex {0}] == doctest::Approx(src_low_before));
  CHECK(src.get_col_upp()[ColIndex {0}] == doctest::Approx(src_upp_before));

  // Clone reflects the mutation.
  CHECK(cloned.get_col_low()[ColIndex {0}] == doctest::Approx(3.0));
  CHECK(cloned.get_col_upp()[ColIndex {0}] == doctest::Approx(7.0));
}

TEST_CASE("clone(shallow) preserves all invariants of clone(deep)")
{
  // Phase 5 contract: shallow clone is supposed to differ from deep
  // clone ONLY in memory layout (shared shared_ptrs vs independent
  // copies).  Behaviourally the two must be indistinguishable.  This
  // test runs the same sequence (clone → mutate bounds → write_lp)
  // on both kinds and asserts byte-equivalent .lp output.
  using Kind = LinearInterface::CloneKind;
  const auto src = make_labelled_lp();

  auto cloned_deep = src.clone(Kind::deep);
  auto cloned_shal = src.clone(Kind::shallow);

  cloned_deep.set_col_low(ColIndex {0}, 2.0);
  cloned_deep.set_col_upp(ColIndex {0}, 8.0);
  cloned_shal.set_col_low(ColIndex {0}, 2.0);
  cloned_shal.set_col_upp(ColIndex {0}, 8.0);

  // Source must be untouched in both cases.
  CHECK(src.get_col_low()[ColIndex {0}] == doctest::Approx(0.0));
  CHECK(src.get_col_upp()[ColIndex {0}] == doctest::Approx(10.0));

  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto deep_stem = (tmpdir / "test_kind_deep").string();
  const auto shal_stem = (tmpdir / "test_kind_shallow").string();
  REQUIRE(cloned_deep.write_lp(deep_stem).has_value());
  REQUIRE(cloned_shal.write_lp(shal_stem).has_value());

  auto strip_problem_header = [](std::string s)
  {
    const std::string marker = "\\Problem name";
    auto p = s.find(marker);
    if (p == std::string::npos) {
      return s;
    }
    auto end = s.find('\n', p);
    if (end == std::string::npos) {
      return s.substr(0, p);
    }
    s.erase(p, end - p + 1);
    return s;
  };

  CHECK(strip_problem_header(read_file(deep_stem + ".lp"))
        == strip_problem_header(read_file(shal_stem + ".lp")));

  std::error_code ec;
  std::filesystem::remove(deep_stem + ".lp", ec);
  std::filesystem::remove(shal_stem + ".lp", ec);
}

TEST_CASE("clone(shallow) + add_*_disposable + write_lp on cloned LP")
{
  // The actual hot-path scenario: shallow clone (no metadata copy),
  // add disposable cols/rows on the clone, write_lp produces
  // gtopt-formatted labels for both the shared production cols/rows
  // AND the clone-local disposable adds.
  using Kind = LinearInterface::CloneKind;
  const auto src = make_labelled_lp();
  auto cloned = src.clone(Kind::shallow);

  const auto sup = cloned.add_col_disposable(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
      .class_name = "ShalDisp",
      .variable_name = "elastic_sup",
      .variable_uid = 200,
  });

  SparseRow fix {
      .lowb = 7.5,
      .uppb = 7.5,
      .class_name = "ShalDisp",
      .constraint_name = "elastic_fix",
      .variable_uid = 200,
  };
  fix[ColIndex {0}] = 1.0;
  fix[sup] = 1.0;
  std::ignore = cloned.add_row_disposable(fix);

  CHECK(src.get_numcols() == 2);
  CHECK(src.get_numrows() == 1);
  CHECK(cloned.get_numcols() == 3);
  CHECK(cloned.get_numrows() == 2);

  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto stem = (tmpdir / "test_shallow_disposable_dump").string();
  REQUIRE(cloned.write_lp(stem).has_value());

  const auto content = read_file(stem + ".lp");
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("bus") != std::string::npos);
  CHECK(content.find("shaldisp") != std::string::npos);
  CHECK(content.find("elastic_sup") != std::string::npos);
  CHECK(content.find("elastic_fix") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}

TEST_CASE("clone(shallow) + bound mutation does not leak into source")
{
  using Kind = LinearInterface::CloneKind;
  auto src = make_labelled_lp();
  auto a = src.clone(Kind::shallow);
  auto b = src.clone(Kind::shallow);

  a.set_col_low(ColIndex {0}, 1.0);
  a.set_col_upp(ColIndex {0}, 4.0);
  b.set_col_low(ColIndex {0}, 5.0);
  b.set_col_upp(ColIndex {0}, 9.0);

  CHECK(a.get_col_low()[ColIndex {0}] == doctest::Approx(1.0));
  CHECK(a.get_col_upp()[ColIndex {0}] == doctest::Approx(4.0));
  CHECK(b.get_col_low()[ColIndex {0}] == doctest::Approx(5.0));
  CHECK(b.get_col_upp()[ColIndex {0}] == doctest::Approx(9.0));
  CHECK(src.get_col_low()[ColIndex {0}] == doctest::Approx(0.0));
  CHECK(src.get_col_upp()[ColIndex {0}] == doctest::Approx(10.0));
}

TEST_CASE(
    "clone is independent — disposable adds on one clone don't bleed "
    "into a sibling clone")
{
  // Two concurrent clones from the same source mirror the aperture
  // pattern (multiple aperture tasks active).  Disposable cols added
  // on clone A must not be visible to clone B (or to the source).
  const auto src = make_labelled_lp();
  auto a = src.clone();
  auto b = src.clone();

  const auto a_disp = a.add_col_disposable(SparseCol {
      .uppb = 1.0,
      .class_name = "DispA",
      .variable_name = "x",
      .variable_uid = 1,
  });
  const auto b_disp = b.add_col_disposable(SparseCol {
      .uppb = 2.0,
      .class_name = "DispB",
      .variable_name = "y",
      .variable_uid = 1,
  });

  CHECK(a.get_numcols() == src.get_numcols() + 1);
  CHECK(b.get_numcols() == src.get_numcols() + 1);
  CHECK(src.get_numcols() == 2);

  // Both clones got col index = src.numcols (== 2), since each was
  // appended on its own backend.
  CHECK(a_disp == ColIndex {2});
  CHECK(b_disp == ColIndex {2});

  // Critical: clone A's disposable label must not appear in clone B's
  // dump (and vice-versa).  Both clones have a disposable at col 2,
  // but each carries its own per-clone-local meta.
  const auto tmpdir = std::filesystem::temp_directory_path();
  const auto a_stem = (tmpdir / "test_clone_indep_a").string();
  const auto b_stem = (tmpdir / "test_clone_indep_b").string();
  REQUIRE(a.write_lp(a_stem).has_value());
  REQUIRE(b.write_lp(b_stem).has_value());

  const auto a_content = read_file(a_stem + ".lp");
  const auto b_content = read_file(b_stem + ".lp");

  CHECK(a_content.find("dispa") != std::string::npos);
  CHECK(a_content.find("dispb") == std::string::npos);
  CHECK(b_content.find("dispb") != std::string::npos);
  CHECK(b_content.find("dispa") == std::string::npos);

  std::error_code ec;
  std::filesystem::remove(a_stem + ".lp", ec);
  std::filesystem::remove(b_stem + ".lp", ec);
}
