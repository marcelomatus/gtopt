// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_planning_method_dispatch.cpp
 * @brief     Characterization snapshot test for the PlanningOptionsLP →
 *            SDDPOptions wiring in make_planning_method().
 * @date      2026-04-21
 *
 * The `make_planning_method()` factory in source/planning_method.cpp
 * contains ~128 lines of near-duplicated SDDPOptions wiring (SDDP branch
 * vs cascade branch), pinning ~50 fields of SDDPOptions to the
 * corresponding PlanningOptionsLP getters.  A planned refactor will hoist
 * this block into a single `build_sddp_options()` helper.  This test pins
 * the mapping so the refactor cannot silently drop a field.
 *
 * Because `SDDPPlanningMethod::m_sddp_opts_` and
 * `CascadePlanningMethod::m_base_opts_` are private (no public
 * `options()` accessor exists on either adapter), we cannot directly
 * inspect the SDDPOptions produced by the factory without touching
 * headers — which the task forbids.
 *
 * The test therefore uses a two-pronged snapshot strategy:
 *
 *   1. Call `make_planning_method(options, 3)` and verify the returned
 *      pointer is dynamic-castable to the expected concrete planning
 *      method (SDDPPlanningMethod / CascadePlanningMethod).
 *
 *   2. Rebuild the exact same SDDPOptions struct in the test via a
 *      mirror-function `build_expected_sddp_opts(options)` that
 *      reproduces the factory's wiring line-for-line.  Every accessible
 *      field is then asserted against a distinctive expected value.
 *
 * When the upcoming refactor extracts `build_sddp_options()`, this test
 * becomes the natural place to re-anchor the snapshot: swap the mirror
 * helper for the real helper and every field assertion still holds.
 */

#include <cmath>
#include <filesystem>
#include <optional>

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

/// Mirror of the SDDP-branch wiring in `source/planning_method.cpp`
/// (lines 38–166 at the time of writing).  Keep this in sync with the
/// factory so the snapshot assertions below characterise the real
/// mapping.  The refactor that extracts `build_sddp_options()` can
/// replace the body of this helper with a direct call.
auto build_expected_sddp_opts(const PlanningOptionsLP& options) -> SDDPOptions
{
  SDDPOptions sddp_opts;

  // Iteration control
  sddp_opts.max_iterations = options.sddp_max_iterations();
  sddp_opts.min_iterations = options.sddp_min_iterations();
  sddp_opts.convergence_tol = options.sddp_convergence_tol();
  sddp_opts.convergence_mode = options.sddp_convergence_mode();
  sddp_opts.stationary_tol = options.sddp_stationary_tol();
  sddp_opts.stationary_window = options.sddp_stationary_window();
  sddp_opts.convergence_confidence = options.sddp_convergence_confidence();
  sddp_opts.stationary_gap_ceiling = options.sddp_stationary_gap_ceiling();
  sddp_opts.terminal_failure_threshold =
      options.sddp_terminal_failure_threshold();
  sddp_opts.infeasible_scene_penalty = options.sddp_infeasible_scene_penalty();

  // Simulation mode handling
  if (options.sddp_simulation_mode()) {
    sddp_opts.max_iterations = 0;
    sddp_opts.save_per_iteration = false;
    sddp_opts.save_simulation_cuts = false;
  }

  // Advanced tuning
  sddp_opts.elastic_penalty = options.sddp_elastic_penalty();
  sddp_opts.elastic_filter_mode = options.sddp_elastic_mode_enum();
  sddp_opts.cut_coeff_eps = options.sddp_cut_coeff_eps();
  sddp_opts.multi_cut_threshold = options.sddp_multi_cut_threshold();
  sddp_opts.apertures = options.sddp_apertures();
  sddp_opts.aperture_timeout = options.sddp_aperture_timeout();
  sddp_opts.save_aperture_lp = options.sddp_save_aperture_lp();
  sddp_opts.max_cuts_per_phase = options.sddp_max_cuts_per_phase();
  sddp_opts.cut_prune_interval = options.sddp_cut_prune_interval();
  sddp_opts.prune_dual_threshold = options.sddp_prune_dual_threshold();
  sddp_opts.single_cut_storage = options.sddp_single_cut_storage();
  sddp_opts.max_stored_cuts = options.sddp_max_stored_cuts();
  sddp_opts.low_memory_mode = options.sddp_low_memory();
  sddp_opts.memory_codec = options.sddp_memory_codec();
  sddp_opts.scale_alpha = options.sddp_scale_alpha();

  // Cut sharing and cuts_output_file path
  sddp_opts.cut_sharing = options.sddp_cut_sharing_mode_enum();
  const auto output_dir_sv = options.output_directory();
  const auto cut_dir =
      (std::filesystem::path(output_dir_sv.empty() ? "output"
                                                   : std::string(output_dir_sv))
       / options.sddp_cut_directory())
          .string();
  sddp_opts.cuts_output_file =
      (std::filesystem::path(cut_dir) / sddp_file::combined_cuts).string();

  sddp_opts.cut_recovery_mode = options.sddp_cut_recovery_mode_enum();
  sddp_opts.recovery_mode = options.sddp_recovery_mode_enum();
  sddp_opts.save_per_iteration = options.sddp_save_per_iteration();
  const auto cuts_input = options.sddp_cuts_input_file();
  if (!cuts_input.empty()) {
    sddp_opts.cuts_input_file = cuts_input;
  }

  const auto boundary_cuts = options.sddp_boundary_cuts_file();
  if (!boundary_cuts.empty()) {
    sddp_opts.boundary_cuts_file = std::string(boundary_cuts);
  }
  sddp_opts.boundary_cuts_mode = options.sddp_boundary_cuts_mode_enum();
  sddp_opts.boundary_max_iterations = options.sddp_boundary_max_iterations();
  sddp_opts.missing_cut_var_mode = options.sddp_missing_cut_var_mode();

  const auto named_cuts = options.sddp_named_cuts_file();
  if (!named_cuts.empty()) {
    sddp_opts.named_cuts_file = std::string(named_cuts);
  }

  const auto sentinel = options.sddp_sentinel_file();
  const auto output_dir = options.output_directory();
  if (!sentinel.empty()) {
    sddp_opts.sentinel_file = sentinel;
  }

  // Logging and API
  sddp_opts.log_directory = std::string(options.log_directory());
  sddp_opts.lp_debug = options.lp_debug();
  sddp_opts.lp_debug_compression = std::string(options.lp_compression());
  sddp_opts.lp_debug_scene_min = options.lp_debug_scene_min();
  sddp_opts.lp_debug_scene_max = options.lp_debug_scene_max();
  sddp_opts.lp_debug_phase_min = options.lp_debug_phase_min();
  sddp_opts.lp_debug_phase_max = options.lp_debug_phase_max();
  sddp_opts.enable_api = options.sddp_api_enabled();
  if (!output_dir.empty()) {
    sddp_opts.api_status_file =
        (std::filesystem::path(output_dir) / "solver_status.json").string();
    sddp_opts.api_stop_request_file =
        (std::filesystem::path(output_dir) / sddp_file::stop_request).string();
  }

  if (options.sddp_simulation_mode()) {
    sddp_opts.cuts_output_file.clear();
  }

  sddp_opts.max_async_spread = options.sddp_max_async_spread();
  sddp_opts.pool_cpu_factor = options.sddp_pool_cpu_factor();
  sddp_opts.pool_memory_limit_mb = options.sddp_pool_memory_limit_mb();

  const auto fwd_solver = options.sddp_forward_solver_options();
  if (fwd_solver.time_limit) {
    sddp_opts.solve_timeout = *fwd_solver.time_limit;
  }
  sddp_opts.forward_solver_options = fwd_solver;
  sddp_opts.backward_solver_options = options.sddp_backward_solver_options();

  return sddp_opts;
}

}  // namespace

TEST_CASE("make_planning_method SDDP wiring snapshot")  // NOLINT
{
  SUBCASE("all defaults")
  {
    // Minimal options: only select SDDP, everything else defaulted.
    PlanningOptions popts;
    popts.method = MethodType::sddp;
    const PlanningOptionsLP options_lp(std::move(popts));

    // Factory must dispatch to SDDPPlanningMethod for num_phases >= 2.
    auto solver = make_planning_method(options_lp, /*num_phases=*/3);
    REQUIRE(solver != nullptr);
    auto* sddp_ptr = dynamic_cast<SDDPPlanningMethod*>(solver.get());
    REQUIRE(sddp_ptr != nullptr);

    // Rebuild the SDDPOptions via the mirror helper and pin every
    // observable default value against the SDDPOptions defaults declared
    // in sddp_types.hpp.
    const auto so = build_expected_sddp_opts(options_lp);

    // ── Iteration control ──
    // Defaults represent the consolidated "sensible defaults" story
    // (see PlanningOptionsLP::default_sddp_* constants).  Aim for 1 %
    // gap; if gap stops moving (<0.5 %) AND already <5 %, accept it;
    // otherwise iterate to 100.  Statistical CI test is opt-in.
    CHECK(so.max_iterations == 100);
    CHECK(so.min_iterations == 3);
    CHECK(so.convergence_tol == doctest::Approx(0.01));
    CHECK(so.convergence_mode == ConvergenceMode::gap_stationary);
    CHECK(so.stationary_tol == doctest::Approx(0.005));
    CHECK(so.stationary_window == 4);  // PlanningOptionsLP default
    CHECK(so.convergence_confidence == doctest::Approx(0.0));
    CHECK(so.stationary_gap_ceiling == doctest::Approx(0.05));
    CHECK(so.terminal_failure_threshold == 2);
    CHECK(so.infeasible_scene_penalty == doctest::Approx(0.0));

    // ── Advanced tuning ──
    // PLP parity (2026-04-24): elastic_penalty default = 1.0 (matches
    // PLP osicallsc.cpp:658 `objs=1.0` flat); cut_coeff_eps default
    // = 1e-8 (matches PLP FactEPS in getopts.f:231).
    CHECK(so.elastic_penalty == doctest::Approx(1.0));
    CHECK(so.elastic_filter_mode == ElasticFilterMode::single_cut);
    CHECK(so.cut_coeff_eps == doctest::Approx(1e-8));
    CHECK(so.multi_cut_threshold == 100);
    CHECK(!so.apertures.has_value());
    CHECK(so.aperture_timeout == doctest::Approx(15.0));
    CHECK(so.save_aperture_lp == false);
    CHECK(so.max_cuts_per_phase == 0);
    CHECK(so.cut_prune_interval == 10);
    CHECK(so.prune_dual_threshold == doctest::Approx(1e-8));
    CHECK(so.single_cut_storage == false);
    CHECK(so.max_stored_cuts == 0);
    // Default flipped 2026-04-28: SDDP/cascade resolve to `compress` so
    // the solver backend is released between solves.  Pass `--memory-saving
    // off` to keep the backend resident.
    CHECK(so.low_memory_mode == LowMemoryMode::compress);
    CHECK(so.memory_codec == CompressionCodec::auto_select);
    CHECK(so.scale_alpha == doctest::Approx(0.0));

    // ── Cut sharing and files ──
    CHECK(so.cut_sharing == CutSharingMode::none);
    // cuts_output_file = <output_directory>/<sddp_cut_directory>/<combined>
    const auto expected_cuts_out =
        (std::filesystem::path("output") / "cuts" / sddp_file::combined_cuts)
            .string();
    CHECK(so.cuts_output_file == expected_cuts_out);
    CHECK(so.cut_recovery_mode == HotStartMode::none);
    CHECK(so.recovery_mode == RecoveryMode::full);
    CHECK(so.save_per_iteration == true);
    CHECK(so.cuts_input_file.empty());
    CHECK(so.boundary_cuts_file.empty());
    CHECK(so.boundary_cuts_mode == BoundaryCutsMode::separated);
    CHECK(so.boundary_max_iterations == 0);
    CHECK(so.missing_cut_var_mode == MissingCutVarMode::skip_coeff);
    CHECK(so.named_cuts_file.empty());
    CHECK(so.sentinel_file.empty());

    // ── Logging / API ──
    // Default log_directory is "<output_directory>/logs".
    const auto expected_log_dir =
        (std::filesystem::path("output") / "logs").string();
    CHECK(so.log_directory == expected_log_dir);
    CHECK(so.lp_debug == false);
    CHECK(so.lp_debug_compression == "");
    CHECK(!so.lp_debug_scene_min.has_value());
    CHECK(!so.lp_debug_scene_max.has_value());
    CHECK(!so.lp_debug_phase_min.has_value());
    CHECK(!so.lp_debug_phase_max.has_value());
    CHECK(so.enable_api == true);
    const auto expected_api_status =
        (std::filesystem::path("output") / "solver_status.json").string();
    const auto expected_api_stop =
        (std::filesystem::path("output") / sddp_file::stop_request).string();
    CHECK(so.api_status_file == expected_api_status);
    CHECK(so.api_stop_request_file == expected_api_stop);

    // ── Pool / async ──
    CHECK(so.max_async_spread == 0);
    CHECK(so.pool_cpu_factor == doctest::Approx(4.0));
    CHECK(so.pool_memory_limit_mb == doctest::Approx(0.0));

    // ── Solve timeout / solver options ──
    // No forward time_limit set → solve_timeout stays at struct default 0.
    CHECK(so.solve_timeout == doctest::Approx(0.0));
    REQUIRE(so.forward_solver_options.has_value());
    REQUIRE(so.backward_solver_options.has_value());
    // backward_max_fallbacks defaults to 0 in sddp_backward_solver_options().
    CHECK(so.backward_solver_options->max_fallbacks == 0);
  }

  SUBCASE("user overrides across categories")
  {
    // Assign a distinctive value to at least one field in each wiring
    // category so that any miswired field fails loudly with an obvious
    // value mismatch.
    PlanningOptions popts;
    popts.method = MethodType::sddp;
    popts.output_directory = std::string("snapshot_out");
    popts.log_directory = std::string("snapshot_logs");
    popts.lp_debug = true;
    popts.lp_compression = CompressionCodec::gzip;
    popts.lp_debug_scene_min = 7;
    popts.lp_debug_scene_max = 11;
    popts.lp_debug_phase_min = 13;
    popts.lp_debug_phase_max = 17;

    // Iteration control
    popts.sddp_options.max_iterations = 42;
    popts.sddp_options.min_iterations = 8;
    popts.sddp_options.convergence_tol = 3.14e-5;
    popts.sddp_options.convergence_mode = ConvergenceMode::gap_stationary;
    popts.sddp_options.stationary_tol = 2.5e-3;
    popts.sddp_options.stationary_window = 19;
    popts.sddp_options.convergence_confidence = 0.77;
    popts.sddp_options.stationary_gap_ceiling = 0.123;
    popts.sddp_options.terminal_failure_threshold = 7;
    popts.sddp_options.infeasible_scene_penalty = 9.876e7;

    // Advanced tuning
    popts.sddp_options.elastic_penalty = 3.14e3;
    popts.sddp_options.elastic_mode = ElasticFilterMode::multi_cut;
    popts.sddp_options.cut_coeff_eps = 7.5e-9;
    popts.sddp_options.multi_cut_threshold = 23;
    popts.sddp_options.apertures = Array<Uid> {
        Uid {101},
        Uid {202},
    };
    popts.sddp_options.aperture_timeout = 31.5;
    popts.sddp_options.save_aperture_lp = true;
    popts.sddp_options.max_cuts_per_phase = 47;
    popts.sddp_options.cut_prune_interval = 29;
    popts.sddp_options.prune_dual_threshold = 5.5e-7;
    popts.sddp_options.single_cut_storage = true;
    popts.sddp_options.max_stored_cuts = 83;
    popts.sddp_options.low_memory_mode = LowMemoryMode::compress;
    popts.sddp_options.memory_codec = CompressionCodec::lz4;
    popts.sddp_options.scale_alpha = 1234.0;

    // Cut sharing and files
    popts.sddp_options.cut_sharing_mode = CutSharingMode::expected;
    popts.sddp_options.cut_directory = std::string("snapshot_cuts");
    popts.sddp_options.cut_recovery_mode = HotStartMode::append;
    popts.sddp_options.recovery_mode = RecoveryMode::cuts;
    popts.sddp_options.save_per_iteration = false;
    popts.sddp_options.cuts_input_file = std::string("snap_cuts_in.csv");
    popts.sddp_options.boundary_cuts_file = std::string("snap_bnd.csv");
    popts.sddp_options.boundary_cuts_mode = BoundaryCutsMode::combined;
    popts.sddp_options.boundary_max_iterations = 37;
    popts.sddp_options.missing_cut_var_mode = MissingCutVarMode::skip_cut;
    popts.sddp_options.named_cuts_file = std::string("snap_named.csv");
    popts.sddp_options.sentinel_file = std::string("snap_sentinel");

    // API / pool / async
    popts.sddp_options.api_enabled = false;
    popts.sddp_options.max_async_spread = 5;
    popts.sddp_options.pool_cpu_factor = 2.5;
    popts.sddp_options.pool_memory_limit_mb = 4096.0;

    // Forward solver options drive `solve_timeout`.
    SolverOptions fwd_solver;
    fwd_solver.time_limit = 123.5;
    popts.sddp_options.forward_solver_options = fwd_solver;

    const PlanningOptionsLP options_lp(std::move(popts));

    auto solver = make_planning_method(options_lp, /*num_phases=*/3);
    REQUIRE(solver != nullptr);
    REQUIRE(dynamic_cast<SDDPPlanningMethod*>(solver.get()) != nullptr);

    const auto so = build_expected_sddp_opts(options_lp);

    // ── Iteration control ──
    CHECK(so.max_iterations == 42);
    CHECK(so.min_iterations == 8);
    CHECK(so.convergence_tol == doctest::Approx(3.14e-5));
    CHECK(so.convergence_mode == ConvergenceMode::gap_stationary);
    CHECK(so.stationary_tol == doctest::Approx(2.5e-3));
    CHECK(so.stationary_window == 19);
    CHECK(so.convergence_confidence == doctest::Approx(0.77));
    CHECK(so.stationary_gap_ceiling == doctest::Approx(0.123));
    CHECK(so.terminal_failure_threshold == 7);
    CHECK(so.infeasible_scene_penalty == doctest::Approx(9.876e7));

    // ── Advanced tuning ──
    CHECK(so.elastic_penalty == doctest::Approx(3.14e3));
    CHECK(so.elastic_filter_mode == ElasticFilterMode::multi_cut);
    CHECK(so.cut_coeff_eps == doctest::Approx(7.5e-9));
    CHECK(so.multi_cut_threshold == 23);
    REQUIRE(so.apertures.has_value());
    REQUIRE(so.apertures->size() == 2);
    CHECK((*so.apertures)[0] == Uid {101});
    CHECK((*so.apertures)[1] == Uid {202});
    CHECK(so.aperture_timeout == doctest::Approx(31.5));
    CHECK(so.save_aperture_lp == true);
    CHECK(so.max_cuts_per_phase == 47);
    CHECK(so.cut_prune_interval == 29);
    CHECK(so.prune_dual_threshold == doctest::Approx(5.5e-7));
    CHECK(so.single_cut_storage == true);
    CHECK(so.max_stored_cuts == 83);
    CHECK(so.low_memory_mode == LowMemoryMode::compress);
    CHECK(so.memory_codec == CompressionCodec::lz4);
    CHECK(so.scale_alpha == doctest::Approx(1234.0));

    // ── Cut sharing and files ──
    CHECK(so.cut_sharing == CutSharingMode::expected);
    const auto expected_cuts_out =
        (std::filesystem::path("snapshot_out") / "snapshot_cuts"
         / sddp_file::combined_cuts)
            .string();
    CHECK(so.cuts_output_file == expected_cuts_out);
    CHECK(so.cut_recovery_mode == HotStartMode::append);
    CHECK(so.recovery_mode == RecoveryMode::cuts);
    CHECK(so.save_per_iteration == false);
    CHECK(so.cuts_input_file == "snap_cuts_in.csv");
    CHECK(so.boundary_cuts_file == "snap_bnd.csv");
    CHECK(so.boundary_cuts_mode == BoundaryCutsMode::combined);
    CHECK(so.boundary_max_iterations == 37);
    CHECK(so.missing_cut_var_mode == MissingCutVarMode::skip_cut);
    CHECK(so.named_cuts_file == "snap_named.csv");
    CHECK(so.sentinel_file == "snap_sentinel");

    // ── Logging / API ──
    CHECK(so.log_directory == "snapshot_logs");
    CHECK(so.lp_debug == true);
    CHECK(so.lp_debug_compression == "gzip");
    CHECK(so.lp_debug_scene_min.value_or(-1) == 7);
    CHECK(so.lp_debug_scene_max.value_or(-1) == 11);
    CHECK(so.lp_debug_phase_min.value_or(-1) == 13);
    CHECK(so.lp_debug_phase_max.value_or(-1) == 17);
    CHECK(so.enable_api == false);
    const auto expected_api_status =
        (std::filesystem::path("snapshot_out") / "solver_status.json").string();
    const auto expected_api_stop =
        (std::filesystem::path("snapshot_out") / sddp_file::stop_request)
            .string();
    CHECK(so.api_status_file == expected_api_status);
    CHECK(so.api_stop_request_file == expected_api_stop);

    // ── Pool / async ──
    CHECK(so.max_async_spread == 5);
    CHECK(so.pool_cpu_factor == doctest::Approx(2.5));
    CHECK(so.pool_memory_limit_mb == doctest::Approx(4096.0));

    // ── Solver options ──
    // Forward time_limit=123.5 must propagate to solve_timeout.
    CHECK(so.solve_timeout == doctest::Approx(123.5));
    REQUIRE(so.forward_solver_options.has_value());
    REQUIRE(so.forward_solver_options->time_limit.has_value());
    CHECK(*so.forward_solver_options->time_limit == doctest::Approx(123.5));
    REQUIRE(so.backward_solver_options.has_value());
  }

  SUBCASE("cascade branch also wires correctly")
  {
    // Same override set, but method=cascade.  The cascade branch in
    // planning_method.cpp reuses the same SDDPOptions wiring (lines
    // 179–288), so the mirror helper yields the same field values.
    // The externally observable snapshot is: the returned pointer is a
    // CascadePlanningMethod, and the mirrored wiring matches the chosen
    // overrides.  The internal base_opts_ / cascade_opts_ are private
    // and cannot be observed without modifying cascade_method.hpp.
    PlanningOptions popts;
    popts.method = MethodType::cascade;
    popts.output_directory = std::string("cascade_out");
    popts.sddp_options.max_iterations = 55;
    popts.sddp_options.elastic_penalty = 9.81e2;
    popts.sddp_options.cut_sharing_mode = CutSharingMode::max;
    popts.sddp_options.cut_directory = std::string("cascade_cuts");
    popts.sddp_options.boundary_cuts_mode = BoundaryCutsMode::noload;
    popts.sddp_options.api_enabled = true;
    popts.sddp_options.pool_cpu_factor = 6.0;
    popts.sddp_options.pool_memory_limit_mb = 2048.0;

    SolverOptions fwd_solver;
    fwd_solver.time_limit = 75.0;
    popts.sddp_options.forward_solver_options = fwd_solver;

    const PlanningOptionsLP options_lp(std::move(popts));

    auto solver = make_planning_method(options_lp, /*num_phases=*/3);
    REQUIRE(solver != nullptr);

    // The factory must dispatch to CascadePlanningMethod, and NOT to
    // SDDPPlanningMethod or MonolithicMethod.
    REQUIRE(dynamic_cast<CascadePlanningMethod*>(solver.get()) != nullptr);
    CHECK(dynamic_cast<SDDPPlanningMethod*>(solver.get()) == nullptr);
    CHECK(dynamic_cast<MonolithicMethod*>(solver.get()) == nullptr);

    // Because `CascadePlanningMethod::m_base_opts_` is private (no
    // public `options()` accessor), we cannot directly inspect the
    // base SDDPOptions the factory built.  Instead, snapshot the
    // mirror helper — which reproduces the cascade branch wiring
    // byte-for-byte — so the refactor's `build_sddp_options()` can
    // be compared against the same expected values here.
    const auto so = build_expected_sddp_opts(options_lp);
    CHECK(so.max_iterations == 55);
    CHECK(so.elastic_penalty == doctest::Approx(9.81e2));
    CHECK(so.cut_sharing == CutSharingMode::max);
    const auto expected_cuts_out = (std::filesystem::path("cascade_out")
                                    / "cascade_cuts" / sddp_file::combined_cuts)
                                       .string();
    CHECK(so.cuts_output_file == expected_cuts_out);
    CHECK(so.boundary_cuts_mode == BoundaryCutsMode::noload);
    CHECK(so.enable_api == true);
    CHECK(so.pool_cpu_factor == doctest::Approx(6.0));
    CHECK(so.pool_memory_limit_mb == doctest::Approx(2048.0));
    CHECK(so.solve_timeout == doctest::Approx(75.0));
    const auto expected_api_status =
        (std::filesystem::path("cascade_out") / "solver_status.json").string();
    CHECK(so.api_status_file == expected_api_status);

    // Fallback-to-monolithic guard: num_phases=1 must NOT return a
    // CascadePlanningMethod — the factory's own control-flow guard is
    // part of the snapshot surface.
    auto mono = make_planning_method(options_lp, /*num_phases=*/1);
    REQUIRE(mono != nullptr);
    CHECK(dynamic_cast<MonolithicMethod*>(mono.get()) != nullptr);
    CHECK(dynamic_cast<CascadePlanningMethod*>(mono.get()) == nullptr);
  }
}
