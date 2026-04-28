// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_elastic_clone_dump.cpp
 * @brief     End-to-end test of the elastic-filter shallow-clone path:
 *            run `elastic_filter_solve`, take the returned cloned LP,
 *            write it as `.lp` and verify the labels of BOTH the
 *            production cols (from shared meta) AND the disposable
 *            slack/fixing-row cols (from per-clone-local extras).
 *
 * This is the integration test the user asked for in
 * `test-coverage-critic` style: it walks the full
 *
 *     `clone(shallow)` → `add_col_disposable` × N → `add_row_disposable`
 *     → `resolve` → `write_lp` (post-mortem dump path)
 *
 * pipeline against a real labelled LP, so any regression in the
 * disposable-meta fall-through inside `generate_labels_from_maps`
 * will be caught here even if the unit-level tests pass.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

std::string elastic_dump_read_file(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE(
    "elastic_filter_solve produces a writable shallow clone "
    "with valid labels for both production and disposable cells")
{
  // Labelled LP: min x1 + 1000*alpha  s.t.  x1 >= 5, alpha >= 0.
  // The dependent col `x1` will be fixed at trial_value = 5.0 to
  // force `relax_fixed_state_variable` to add slack cols + fixing
  // row via the disposable APIs on the cloned LP.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  const auto alpha = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = 1000.0,
      .class_name = "BendersAlpha",
      .variable_name = "alpha",
      .variable_uid = 1,
  });
  std::ignore = alpha;

  SparseRow row {
      .lowb = 5.0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Demand",
      .constraint_name = "min",
      .variable_uid = 1,
  };
  row[x1] = 1.0;
  std::ignore = li.add_row(row);

  // Pre-solve so warm state exists.
  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());

  // Fix the dependent column at trial_value to force relaxation.
  li.set_col(x1, 5.0);

  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {99},
          .dependent_col = x1,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 5.0,
          .source_low = 0.0,
          .source_upp = 100.0,
      },
  };

  const auto elastic = elastic_filter_solve(li, links, 1e6, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->link_infos.size() == 1);
  REQUIRE(elastic->link_infos[0].relaxed);

  // Source `li` must NOT have been mutated by the shallow-clone
  // elastic path — disposable adds stay on the clone.  The clone
  // is still alive inside `elastic->clone`, so the source's
  // shared_ptr use_count is 2 — that's the *desired* state and
  // proves the clone is sharing rather than deep-copying.
  CHECK(li.get_numcols() == 2);
  CHECK(li.col_labels_meta_use_count() == 2);

  // The clone has +2 slack cols (sup, sdn) and +1 fixing row.
  CHECK(elastic->clone.get_numcols() == 4);
  CHECK(elastic->clone.get_numrows() == 2);

  // Write the clone's LP and verify the labels.  Both production
  // cells (Gen, Demand) and disposable cells
  // (BendersAlpha:elastic_sup / elastic_sdn / elastic_fix — using
  // the actual `sddp_alpha_class_name`) must appear.
  const auto stem =
      (std::filesystem::temp_directory_path() / "test_elastic_clone_dump")
          .string();
  REQUIRE(elastic->clone.write_lp(stem).has_value());

  const auto content = elastic_dump_read_file(stem + ".lp");
  // Production cols / row visible via shared metadata.
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("demand") != std::string::npos);
  // Slack cols added via `add_col_disposable` — labels synthesised
  // from `m_post_clone_col_metas_`.
  CHECK(content.find("elastic_sup") != std::string::npos);
  CHECK(content.find("elastic_sdn") != std::string::npos);
  // Fixing row added via `add_row_disposable` — label from
  // `m_post_clone_row_metas_`.
  CHECK(content.find("elastic_fix") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}
