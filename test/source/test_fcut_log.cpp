// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_fcut_log.cpp
 * @brief     Tests for the PLP-style feasibility-cut debug log
 *            (`gtopt_fcut.log`, the plpfact.log analogue): the
 *            `FcutLogWriter` primitive, the `format_fcut_cut_lines`
 *            record formatter, and the end-to-end SDDP emission path
 *            (`sddp_options.fcut_log = true` on a forward pass that
 *            hits the elastic branch).
 */

#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/fcut_log.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

[[nodiscard]] std::string fcutlog_slurp(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("FcutLogWriter — disabled writer emits nothing")  // NOLINT
{
  const auto dir =
      std::filesystem::temp_directory_path() / "__fcutlog_disabled__";
  std::filesystem::remove_all(dir);

  FcutLogWriter log;
  log.configure(/*enabled=*/false, dir.string());
  CHECK_FALSE(log.enabled());
  log.write("INFEASIBLE iter=1 scene=1 phase=1 cycle=1 status=1");
  CHECK_FALSE(
      std::filesystem::exists(dir / std::string(FcutLogWriter::filename)));

  // An empty log directory disables the writer even when the flag is on.
  FcutLogWriter log2;
  log2.configure(/*enabled=*/true, "");
  CHECK_FALSE(log2.enabled());

  std::filesystem::remove_all(dir);
}

TEST_CASE("FcutLogWriter — records append atomically with newline")  // NOLINT
{
  const auto dir = std::filesystem::temp_directory_path() / "__fcutlog_write__";
  std::filesystem::remove_all(dir);

  FcutLogWriter log;
  log.configure(/*enabled=*/true, dir.string());
  REQUIRE(log.enabled());

  log.write("INFEASIBLE iter=1 scene=16 phase=25 cycle=2 status=1");
  log.write(
      "mode: state_repair iter=1 scene=16 phase=25 dest=24\n"
      "cut: Reservoir:64:CANUTILLAR efin col=7 coef=1\n"
      "rhs: >= 532.547\n"
      "INSTALLED iter=1 scene=16 phase=25 dest=24 rows=1");

  const auto path = dir / std::string(FcutLogWriter::filename);
  REQUIRE(std::filesystem::exists(path));
  const auto text = fcutlog_slurp(path);
  CHECK(text.starts_with("INFEASIBLE iter=1 scene=16 phase=25"));
  CHECK(text.find("\nmode: state_repair ") != std::string::npos);
  CHECK(text.find("\ncut: Reservoir:64:CANUTILLAR efin ") != std::string::npos);
  CHECK(text.find("\nrhs: >= 532.547\n") != std::string::npos);
  CHECK(text.ends_with("rows=1\n"));  // trailing newline auto-appended

  std::filesystem::remove_all(dir);
}

TEST_CASE("format_fcut_cut_lines — link identity + rhs sense")  // NOLINT
{
  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {7},
          .dependent_col = ColIndex {3},
          .trial_value = 528.705,
          .source_low = 0.0,
          .source_upp = 1000.0,
          .class_name = LPClassName {"Reservoir"},
          .col_name = "efin",
          .uid = Uid {64},
          .name = "CANUTILLAR",
      },
  };

  SUBCASE(">= cut with a known link resolves the element identity")
  {
    SparseRow cut;
    cut[ColIndex {7}] = 1.25;
    cut.greater_equal(532.54714644301191);

    const auto text = format_fcut_cut_lines(links, std::span(&cut, 1));
    CHECK(text.find("cut: Reservoir:64:CANUTILLAR efin col=7 coef=1.25")
          != std::string::npos);
    CHECK(text.find("rhs: >= 532.547146443011") != std::string::npos);
  }

  SUBCASE("<= cut reports uppb; unknown column falls back to the index")
  {
    SparseRow cut;
    cut[ColIndex {99}] = -2.0;
    cut.less_equal(42.0);

    const auto text = format_fcut_cut_lines(links, std::span(&cut, 1));
    CHECK(text.find("cut: ?:? ? col=99 coef=-2") != std::string::npos);
    CHECK(text.find("rhs: <= 42") != std::string::npos);
  }
}

TEST_CASE("SDDPMethod — fcut_log writes gtopt_fcut.log records")  // NOLINT
{
  // Forced-infeasibility fixture (same ground truth as the
  // `ElasticFilterMode comparison` test): phase 1's mandatory waterway
  // discharge cannot be met from the reservoir state phase 0's optimum
  // produces, so the very first forward pass lands in the elastic
  // branch and the fcut log must record the INFEASIBLE detection and
  // at least one mode/cut/rhs/INSTALLED block.
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto log_dir =
      std::filesystem::temp_directory_path() / "__fcutlog_e2e__";
  std::filesystem::remove_all(log_dir);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.enable_api = false;
  sddp_opts.fcut_log = true;
  sddp_opts.log_directory = log_dir.string();

  SDDPMethod sddp(planning_lp, sddp_opts);
  // Convergence is NOT the subject here (this aggressive fixture may
  // legitimately end non-converged) — only the emitted log records are.
  [[maybe_unused]] const auto results = sddp.solve();

  const auto path = log_dir / std::string(FcutLogWriter::filename);
  REQUIRE(std::filesystem::exists(path));
  const auto text = fcutlog_slurp(path);

  // Detection record (PLP "Problema Infactible ...").
  CHECK(text.find("INFEASIBLE iter=") != std::string::npos);
  CHECK(text.find(" status=") != std::string::npos);
  // Cut-emission record: mode header, per-coefficient `cut:` line with
  // the element identity, per-row `rhs:` line, install confirmation
  // naming the destination phase (PLP "Se agrego corte ... EtaDest").
  CHECK(text.find("mode: ") != std::string::npos);
  CHECK(text.find("cut: Reservoir:") != std::string::npos);
  CHECK(text.find("rhs: ") != std::string::npos);
  CHECK(text.find("INSTALLED iter=") != std::string::npos);
  CHECK(text.find(" dest=") != std::string::npos);
  CHECK(text.find(" rows=") != std::string::npos);

  std::filesystem::remove_all(log_dir);
}

TEST_CASE("SDDPMethod — fcut_log disabled leaves no file")  // NOLINT
{
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto log_dir =
      std::filesystem::temp_directory_path() / "__fcutlog_off__";
  std::filesystem::remove_all(log_dir);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-4;
  sddp_opts.enable_api = false;
  sddp_opts.log_directory = log_dir.string();
  // fcut_log left at its default (false).

  SDDPMethod sddp(planning_lp, sddp_opts);
  [[maybe_unused]] const auto results = sddp.solve();

  CHECK_FALSE(
      std::filesystem::exists(log_dir / std::string(FcutLogWriter::filename)));

  std::filesystem::remove_all(log_dir);
}
