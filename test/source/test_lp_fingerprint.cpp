// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/lp_fingerprint.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

TEST_CASE("LpFingerprint basic computation")  // NOLINT
{
  std::vector<SparseCol> cols;
  std::vector<SparseRow> rows;

  // Add two generator columns with different UIDs but same type
  cols.push_back(SparseCol {
      .class_name = "Generator",
      .variable_name = "generation",
      .variable_uid = Uid {1},
      .context =
          make_block_context(make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
  });
  cols.push_back(SparseCol {
      .class_name = "Generator",
      .variable_name = "generation",
      .variable_uid = Uid {2},
      .context =
          make_block_context(make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
  });

  // Add a bus theta column (different type)
  cols.push_back(SparseCol {
      .class_name = "Bus",
      .variable_name = "theta",
      .variable_uid = Uid {1},
      .context =
          make_block_context(make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
  });

  // Add a bus balance row
  rows.push_back(SparseRow {
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {1},
      .context =
          make_block_context(make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
  });

  // Add a generator capacity row (stage context)
  rows.push_back(SparseRow {
      .class_name = "Generator",
      .constraint_name = "capacity",
      .variable_uid = Uid {1},
      .context = make_stage_context(make_uid<Scenario>(0), StageUid {0}),
  });

  auto fp = compute_lp_fingerprint(cols, rows);

  SUBCASE("template is sorted and deduplicated")
  {
    // Two Generator.generation cols should produce one template entry
    CHECK(fp.col_template.size() == 2);  // Bus.theta + Generator.generation
    CHECK(fp.row_template.size() == 2);  // Bus.balance + Generator.capacity

    // Check sorted order (Bus < Generator)
    CHECK(fp.col_template[0].class_name == "Bus");
    CHECK(fp.col_template[0].variable_name == "theta");
    CHECK(fp.col_template[1].class_name == "Generator");
    CHECK(fp.col_template[1].variable_name == "generation");
  }

  SUBCASE("context types are captured")
  {
    CHECK(fp.col_template[0].context_type == "BlockContext");
    CHECK(fp.col_template[1].context_type == "BlockContext");
    CHECK(fp.row_template[0].context_type == "BlockContext");
    CHECK(fp.row_template[1].context_type == "StageContext");
  }

  SUBCASE("stats are computed")
  {
    CHECK(fp.stats.total_cols == 3);
    CHECK(fp.stats.total_rows == 2);
    CHECK(fp.stats.cols_by_class.at("Generator") == 2);
    CHECK(fp.stats.cols_by_class.at("Bus") == 1);
    CHECK(fp.stats.rows_by_class.at("Bus") == 1);
    CHECK(fp.stats.rows_by_class.at("Generator") == 1);
  }

  SUBCASE("no untracked entries")
  {
    CHECK(fp.untracked_cols == 0);
    CHECK(fp.untracked_rows == 0);
  }

  SUBCASE("hashes are non-empty")
  {
    CHECK(!fp.col_hash.empty());
    CHECK(!fp.row_hash.empty());
    CHECK(!fp.structural_hash.empty());
    // SHA-256 produces 64 hex characters
    CHECK(fp.col_hash.size() == 64);
    CHECK(fp.row_hash.size() == 64);
    CHECK(fp.structural_hash.size() == 64);
  }
}

TEST_CASE("LpFingerprint is element-count independent")  // NOLINT
{
  // Build two LPs with same variable types but different element counts
  auto make_cols = [](int n_generators)
  {
    std::vector<SparseCol> cols;
    for (int i = 0; i < n_generators; ++i) {
      cols.push_back(SparseCol {
          .class_name = "Generator",
          .variable_name = "generation",
          .variable_uid = Uid {i},
          .context = make_block_context(
              make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      });
    }
    cols.push_back(SparseCol {
        .class_name = "Bus",
        .variable_name = "theta",
        .variable_uid = Uid {0},
        .context = make_block_context(
            make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
    });
    return cols;
  };

  std::vector<SparseRow> rows;  // empty rows for both

  auto fp5 = compute_lp_fingerprint(make_cols(5), rows);
  auto fp100 = compute_lp_fingerprint(make_cols(100), rows);

  CHECK(fp5.structural_hash == fp100.structural_hash);
  CHECK(fp5.col_hash == fp100.col_hash);
  CHECK(fp5.col_template == fp100.col_template);

  // But stats differ
  CHECK(fp5.stats.total_cols == 6);
  CHECK(fp100.stats.total_cols == 101);
  CHECK(fp5.stats.cols_by_class.at("Generator") == 5);
  CHECK(fp100.stats.cols_by_class.at("Generator") == 100);
}

TEST_CASE("LpFingerprint hash is deterministic")  // NOLINT
{
  std::vector<SparseCol> cols = {
      SparseCol {
          .class_name = "Line",
          .variable_name = "flow",
          .variable_uid = Uid {0},
          .context = make_block_context(
              make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      },
  };
  std::vector<SparseRow> rows;

  auto fp1 = compute_lp_fingerprint(cols, rows);
  auto fp2 = compute_lp_fingerprint(cols, rows);

  CHECK(fp1.structural_hash == fp2.structural_hash);
  CHECK(fp1.col_hash == fp2.col_hash);
  CHECK(fp1.row_hash == fp2.row_hash);
}

TEST_CASE("LpFingerprint detects structural changes")  // NOLINT
{
  std::vector<SparseCol> cols_base = {
      SparseCol {
          .class_name = "Generator",
          .variable_name = "generation",
          .variable_uid = Uid {0},
          .context = make_block_context(
              make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      },
  };
  std::vector<SparseRow> rows;

  auto fp_base = compute_lp_fingerprint(cols_base, rows);

  SUBCASE("adding a new variable type changes the hash")
  {
    auto cols_extended = cols_base;
    cols_extended.push_back(SparseCol {
        .class_name = "Generator",
        .variable_name = "capacity",
        .variable_uid = Uid {0},
        .context = make_stage_context(make_uid<Scenario>(0), StageUid {0}),
    });

    auto fp_extended = compute_lp_fingerprint(cols_extended, rows);
    CHECK(fp_base.structural_hash != fp_extended.structural_hash);
    CHECK(fp_extended.col_template.size() == 2);
  }

  SUBCASE("changing context type changes the hash")
  {
    std::vector<SparseCol> cols_stage = {
        SparseCol {
            .class_name = "Generator",
            .variable_name = "generation",
            .variable_uid = Uid {0},
            .context = make_stage_context(make_uid<Scenario>(0), StageUid {0}),
        },
    };

    auto fp_stage = compute_lp_fingerprint(cols_stage, rows);
    CHECK(fp_base.structural_hash != fp_stage.structural_hash);
  }
}

TEST_CASE("LpFingerprint untracked detection")  // NOLINT
{
  SUBCASE("empty class_name is untracked")
  {
    std::vector<SparseCol> cols = {
        SparseCol {
            .class_name = "",
            .variable_name = "mystery",
            .context = make_block_context(
                make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
        },
    };
    std::vector<SparseRow> rows;

    auto fp = compute_lp_fingerprint(cols, rows);
    CHECK(fp.untracked_cols == 1);
    CHECK(fp.col_template.empty());
  }

  SUBCASE("monostate context is untracked")
  {
    std::vector<SparseCol> cols = {
        SparseCol {
            .class_name = "Generator",
            .variable_name = "generation",
            .context = {},  // monostate
        },
    };
    std::vector<SparseRow> rows;

    auto fp = compute_lp_fingerprint(cols, rows);
    CHECK(fp.untracked_cols == 1);
    CHECK(fp.col_template.empty());
  }
}

TEST_CASE("context_type_name returns correct names")  // NOLINT
{
  CHECK(context_type_name(LpContext {}) == "monostate");
  CHECK(context_type_name(LpContext {
            make_stage_context(make_uid<Scenario>(0), StageUid {0}),
        })
        == "StageContext");
  CHECK(
      context_type_name(LpContext {
          make_block_context(make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      })
      == "BlockContext");
  CHECK(context_type_name(LpContext {
            make_block_context(
                make_uid<Scenario>(0), StageUid {0}, BlockUid {0}, 1),
        })
        == "BlockExContext");
  CHECK(context_type_name(LpContext {
            make_scene_phase_context(make_uid<Scene>(0), make_uid<Phase>(0)),
        })
        == "ScenePhaseContext");
}

TEST_CASE("LpFingerprint JSON output")  // NOLINT
{
  std::vector<SparseCol> cols = {
      SparseCol {
          .class_name = "Bus",
          .variable_name = "theta",
          .variable_uid = Uid {0},
          .context = make_block_context(
              make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      },
  };
  std::vector<SparseRow> rows = {
      SparseRow {
          .class_name = "Bus",
          .constraint_name = "balance",
          .variable_uid = Uid {0},
          .context = make_block_context(
              make_uid<Scenario>(0), StageUid {0}, BlockUid {0}),
      },
  };

  auto fp = compute_lp_fingerprint(cols, rows);

  const std::string filepath = "test_fingerprint_output.json";
  write_lp_fingerprint(fp, filepath, 0, 0);

  // Verify file was created and contains expected content
  std::ifstream file(filepath);
  REQUIRE(file.is_open());
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  file.close();

  CHECK(content.find("\"version\": 1") != std::string::npos);
  CHECK(content.find("\"structural\"") != std::string::npos);
  CHECK(content.find("\"stats\"") != std::string::npos);
  CHECK(content.find("\"Bus\"") != std::string::npos);
  CHECK(content.find("\"theta\"") != std::string::npos);
  CHECK(content.find("\"balance\"") != std::string::npos);
  CHECK(content.find("\"BlockContext\"") != std::string::npos);
  CHECK(content.find(fp.structural_hash) != std::string::npos);

  // Cleanup
  std::filesystem::remove(filepath);
}

}  // namespace
