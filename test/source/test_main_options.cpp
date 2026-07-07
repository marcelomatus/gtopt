/**
 * @file      test_main_options.hpp
 * @brief     Unit tests for main command-line option utilities
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the functions in main_options.hpp:
 * get_opt, make_options_description, apply_cli_options, and
 * make_lp_matrix_options.
 */

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/main_options.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-argument-comment,bugprone-unchecked-optional-access)

// ---- Helper to parse command-line args into a variables_map ----
namespace
{
po::variables_map parse_args(const std::vector<std::string>& args,
                             const po::options_description& desc)
{
  po::positional_options_description pos_desc;
  pos_desc.add("system-file", -1);

  po::variables_map vm;
  auto parser = po::command_line_parser(args)
                    .options(desc)
                    .allow_unregistered()
                    .positional(pos_desc);
  po::store(parser, vm);
  po::notify(vm);
  return vm;
}
}  // namespace
// ---- Tests for get_opt ----

TEST_CASE("get_opt - returns value when present")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--output-directory", "/tmp/out"}, desc);

  auto result = get_opt<std::string>(vm, "output-directory");
  REQUIRE(result.has_value());
  CHECK((result && *result == "/tmp/out"));
}

TEST_CASE("get_opt - returns nullopt when absent")
{
  auto desc = make_options_description();
  auto vm = parse_args({}, desc);

  auto result = get_opt<std::string>(vm, "output-directory");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("get_opt - works with bool type")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-single-bus"}, desc);

  auto result = get_opt<bool>(vm, "use-single-bus");
  REQUIRE(result.has_value());
  CHECK((result && *result == true));
}

TEST_CASE("get_opt - works with double type")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--matrix-eps", "0.001"}, desc);

  auto result = get_opt<double>(vm, "matrix-eps");
  REQUIRE(result.has_value());
  CHECK(result.value_or(-1.0) == doctest::Approx(0.001));
}

TEST_CASE("get_opt - implicit bool value")
{
  auto desc = make_options_description();
  auto vm = parse_args({"--use-kirchhoff"}, desc);

  auto result = get_opt<bool>(vm, "use-kirchhoff");
  REQUIRE(result.has_value());
  CHECK(result.value_or(false) == true);
}

// ---- Tests for make_options_description ----

TEST_CASE("make_options_description - contains expected options")
{
  auto desc = make_options_description();

  // Verify key options are registered by parsing them
  CHECK_NOTHROW(parse_args({"--help"}, desc));
  CHECK_NOTHROW(parse_args({"--version"}, desc));
  CHECK_NOTHROW(parse_args({"--verbose"}, desc));
  CHECK_NOTHROW(parse_args({"--quiet"}, desc));
  CHECK_NOTHROW(parse_args({"--use-single-bus"}, desc));
  CHECK_NOTHROW(parse_args({"--use-kirchhoff"}, desc));
  CHECK_NOTHROW(parse_args({"--lp-only"}, desc));
}

TEST_CASE("make_options_description - short options work")
{
  auto desc = make_options_description();

  auto vm = parse_args({"-b"}, desc);
  CHECK(vm.contains("use-single-bus"));

  vm = parse_args({"-k"}, desc);
  CHECK(vm.contains("use-kirchhoff"));

  vm = parse_args({"-e", "0.01"}, desc);
  CHECK(vm.contains("matrix-eps"));
}

TEST_CASE("make_options_description - positional system-file")
{
  auto desc = make_options_description();
  auto vm = parse_args({"my_system.json"}, desc);

  REQUIRE(vm.contains("system-file"));
  auto files = vm["system-file"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "my_system.json");
}

TEST_CASE("make_options_description - multiple positional files")
{
  auto desc = make_options_description();
  auto vm = parse_args({"file1.json", "file2.json"}, desc);

  REQUIRE(vm.contains("system-file"));
  auto files = vm["system-file"].as<std::vector<std::string>>();
  REQUIRE(files.size() == 2);
  CHECK(files[0] == "file1.json");
  CHECK(files[1] == "file2.json");
}

TEST_CASE("make_options_description - string options")
{
  auto desc = make_options_description();
  auto vm = parse_args(
      {
          "--input-directory",
          "/in",
          "--output-directory",
          "/out",
          "--input-format",
          "parquet",
          "--output-format",
          "csv",
          "--output-compression",
          "gzip",
          "--lp-file",
          "model.lp",
          "--json-file",
          "model.json",
      },
      desc);

  CHECK(get_opt<std::string>(vm, "input-directory").value_or("") == "/in");
  CHECK(get_opt<std::string>(vm, "output-directory").value_or("") == "/out");
  CHECK(get_opt<std::string>(vm, "input-format").value_or("") == "parquet");
  CHECK(get_opt<std::string>(vm, "output-format").value_or("") == "csv");
  CHECK(get_opt<std::string>(vm, "output-compression").value_or("") == "gzip");
  CHECK(get_opt<std::string>(vm, "lp-file").value_or("") == "model.lp");
  CHECK(get_opt<std::string>(vm, "json-file").value_or("") == "model.json");
}

// ---- Tests for apply_cli_options ----

TEST_CASE("apply_cli_options - no options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt);

  CHECK_FALSE(planning.options.model_options.use_single_bus.has_value());
  CHECK_FALSE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK_FALSE(planning.options.input_directory.has_value());
  CHECK_FALSE(planning.options.output_directory.has_value());
  CHECK_FALSE(planning.options.output_format.has_value());
  CHECK_FALSE(planning.options.output_compression.has_value());
}

TEST_CASE("apply_cli_options - all options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    true,
                    false,
                    std::optional<std::string>("/input"),
                    std::optional<std::string>("parquet"),
                    std::optional<std::string>("/output"),
                    std::optional<std::string>("csv"),
                    std::optional<std::string>("gzip"));

  REQUIRE(planning.options.model_options.use_single_bus.has_value());
  CHECK((planning.options.model_options.use_single_bus
         && *planning.options.model_options.use_single_bus == true));

  REQUIRE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK((planning.options.model_options.use_kirchhoff
         && *planning.options.model_options.use_kirchhoff == false));

  REQUIRE(planning.options.input_directory.has_value());
  CHECK((planning.options.input_directory
         && *planning.options.input_directory == "/input"));

  REQUIRE(planning.options.input_format.has_value());
  CHECK((planning.options.input_format
         && *planning.options.input_format == DataFormat::parquet));

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/output"));

  REQUIRE(planning.options.output_format.has_value());
  CHECK((planning.options.output_format
         && *planning.options.output_format == DataFormat::csv));

  REQUIRE(planning.options.output_compression.has_value());
  CHECK((planning.options.output_compression
         && *planning.options.output_compression == CompressionCodec::gzip));
}

TEST_CASE("apply_cli_options - partial options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    true,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::optional<std::string>("/output"),
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.model_options.use_single_bus.has_value());
  CHECK((planning.options.model_options.use_single_bus
         && *planning.options.model_options.use_single_bus == true));

  CHECK_FALSE(planning.options.model_options.use_kirchhoff.has_value());

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/output"));

  CHECK_FALSE(planning.options.input_directory.has_value());
}

TEST_CASE("apply_cli_options - does not overwrite existing when nullopt")
{
  Planning planning {};
  planning.options.output_directory = "original_dir";
  planning.options.model_options.use_kirchhoff = true;

  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "original_dir"));

  REQUIRE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK((planning.options.model_options.use_kirchhoff
         && *planning.options.model_options.use_kirchhoff == true));
}

TEST_CASE("apply_cli_options - overwrites existing when value provided")
{
  Planning planning {};
  planning.options.output_directory = "original_dir";

  apply_cli_options(planning,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::optional<std::string>("new_dir"),
                    std::nullopt,
                    std::nullopt);

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "new_dir"));
}

// ---- Tests for apply_cli_options(Planning&, const MainOptions&) overload ----

TEST_CASE("apply_cli_options(MainOptions) - no options applied")
{
  Planning planning {};
  apply_cli_options(planning, MainOptions {});

  CHECK_FALSE(planning.options.model_options.use_single_bus.has_value());
  CHECK_FALSE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK_FALSE(planning.options.input_directory.has_value());
  CHECK_FALSE(planning.options.output_directory.has_value());
  CHECK_FALSE(planning.options.output_format.has_value());
  CHECK_FALSE(planning.options.output_compression.has_value());
}

TEST_CASE("apply_cli_options(MainOptions) - all options applied")
{
  Planning planning {};
  apply_cli_options(planning,
                    MainOptions {
                        .input_directory = "/in",
                        .input_format = "parquet",
                        .output_directory = "/out",
                        .output_format = "csv",
                        .output_compression = "gzip",
                        .use_single_bus = true,
                        .use_kirchhoff = false,
                    });

  REQUIRE(planning.options.model_options.use_single_bus.has_value());
  CHECK((planning.options.model_options.use_single_bus
         && *planning.options.model_options.use_single_bus == true));

  REQUIRE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK((planning.options.model_options.use_kirchhoff
         && *planning.options.model_options.use_kirchhoff == false));

  REQUIRE(planning.options.input_directory.has_value());
  CHECK((planning.options.input_directory
         && *planning.options.input_directory == "/in"));

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "/out"));

  REQUIRE(planning.options.output_format.has_value());
  CHECK((planning.options.output_format
         && *planning.options.output_format == DataFormat::csv));

  REQUIRE(planning.options.output_compression.has_value());
  CHECK((planning.options.output_compression
         && *planning.options.output_compression == CompressionCodec::gzip));
}

TEST_CASE("apply_cli_options(MainOptions) - does not overwrite when nullopt")
{
  Planning planning {};
  planning.options.output_directory = "existing";
  planning.options.model_options.use_kirchhoff = true;

  apply_cli_options(planning, MainOptions {});

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "existing"));

  REQUIRE(planning.options.model_options.use_kirchhoff.has_value());
  CHECK((planning.options.model_options.use_kirchhoff
         && *planning.options.model_options.use_kirchhoff == true));
}

TEST_CASE("apply_cli_options(MainOptions) - overwrites existing when provided")
{
  Planning planning {};
  planning.options.output_directory = "original";

  apply_cli_options(planning, MainOptions {.output_directory = "replaced"});

  REQUIRE(planning.options.output_directory.has_value());
  CHECK((planning.options.output_directory
         && *planning.options.output_directory == "replaced"));
}

TEST_CASE(
    "apply_cli_options(MainOptions) - --no-mip sets continuous_phases=all")
{
  // Pin the --no-mip shortcut at the apply_cli_options layer.  The flag
  // must materialise as `model_options.continuous_phases = "all"`, which
  // SimulationLP::create_phase_array then propagates to every phase
  // (`p.continuous = true`), causing CommitmentLP / SimpleCommitmentLP /
  // CapacityObjectLP to skip integer-variable setup.  The same path is
  // taken by every PlanningMethod (monolithic, sddp, cascade), so this
  // single CLI hook covers all three.
  Planning planning {};
  apply_cli_options(planning, MainOptions {.no_mip = true});

  REQUIRE(planning.options.model_options.continuous_phases.has_value());
  CHECK(planning.options.model_options.continuous_phases.value() == "all");
}

TEST_CASE(
    "apply_cli_options(MainOptions) - --no-mip overrides JSON "
    "continuous_phases")
{
  // --no-mip is uniformly authoritative when set: a JSON case that
  // requests a partial LP relaxation ("0,2:5") is overridden by the
  // CLI shortcut.  Users who want a partial relaxation should drop the
  // flag and use `--set model_options.continuous_phases=<expr>` directly.
  Planning planning {};
  planning.options.model_options.continuous_phases = OptName {"0,2:5"};

  apply_cli_options(planning, MainOptions {.no_mip = true});

  REQUIRE(planning.options.model_options.continuous_phases.has_value());
  CHECK(planning.options.model_options.continuous_phases.value() == "all");
}

TEST_CASE(
    "apply_cli_options(MainOptions) - --mip-gap sets solver_options.mip_gap")
{
  // The CLI shortcut must land at `solver_options.mip_gap` so the
  // existing backend wiring (CPLEX EPGAP / HiGHS mip_rel_gap / Gurobi
  // MIPGap, each guarded by `*gap > 0`) picks it up uniformly.
  Planning planning {};
  apply_cli_options(planning, MainOptions {.mip_gap = 0.01});

  REQUIRE(planning.options.solver_options.mip_gap.has_value());
  CHECK(planning.options.solver_options.mip_gap.value()
        == doctest::Approx(0.01));
}

TEST_CASE(
    "apply_cli_options(MainOptions) - --mip-gap overrides JSON "
    "solver_options.mip_gap")
{
  // CLI wins over JSON when both are set, consistent with --no-mip.
  Planning planning {};
  planning.options.solver_options.mip_gap = 0.05;

  apply_cli_options(planning, MainOptions {.mip_gap = 0.001});

  REQUIRE(planning.options.solver_options.mip_gap.has_value());
  CHECK(planning.options.solver_options.mip_gap.value()
        == doctest::Approx(0.001));
}

TEST_CASE(
    "apply_cli_options(MainOptions) - --time-limit sets "
    "solver_options.time_limit")
{
  // The CLI shortcut drives solver_options.time_limit so every
  // backend's per-solve wall-clock guard picks it up.  Backends apply
  // only when *value > 0; we pass the user's value through verbatim.
  Planning planning {};
  CHECK_FALSE(planning.options.solver_options.time_limit.has_value());

  apply_cli_options(planning, MainOptions {.time_limit = 60.0});

  REQUIRE(planning.options.solver_options.time_limit.has_value());
  CHECK(planning.options.solver_options.time_limit.value()
        == doctest::Approx(60.0));
}

// ---- Tests for make_lp_matrix_options ----

TEST_CASE("make_lp_matrix_options - defaults when both nullopt")
{
  auto opts = make_lp_matrix_options(false, std::nullopt);

  CHECK(opts.eps == doctest::Approx(0.0));
  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
}

TEST_CASE("make_lp_matrix_options - enable_names false disables all names")
{
  auto opts = make_lp_matrix_options(false, std::nullopt);

  // At minimal, dense col/row name vectors are not built — state variable
  // I/O uses the state variable map (ColIndex-based) directly.
  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
}

TEST_CASE("make_lp_matrix_options - enable_names true enables all names")
{
  auto opts = make_lp_matrix_options(true, std::nullopt);

  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == true);
  CHECK(opts.row_with_name_map == true);
}

TEST_CASE("make_lp_matrix_options - custom eps value")
{
  auto opts = make_lp_matrix_options(false, std::optional<double>(0.001));

  CHECK(opts.eps == doctest::Approx(0.001));
}

TEST_CASE("make_lp_matrix_options - both parameters provided")
{
  auto opts = make_lp_matrix_options(true, std::optional<double>(1e-6));

  CHECK(opts.eps == doctest::Approx(1e-6));
  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == true);
  CHECK(opts.row_with_name_map == true);
}

// ---- Integration-style tests combining get_opt with options description ----

TEST_CASE("Integration - parse and extract all option types")
{
  auto desc = make_options_description();
  auto vm = parse_args(
      {
          "system.json",
          "--use-single-bus",
          "--matrix-eps",
          "0.01",
          "--output-directory",
          "/results",
          "--output-format",
          "parquet",
          "--output-compression",
          "zstd",
      },
      desc);

  auto use_single_bus = get_opt<bool>(vm, "use-single-bus");
  auto matrix_eps = get_opt<double>(vm, "matrix-eps");
  auto output_directory = get_opt<std::string>(vm, "output-directory");
  auto output_format = get_opt<std::string>(vm, "output-format");
  auto output_compression = get_opt<std::string>(vm, "output-compression");

  REQUIRE(use_single_bus.has_value());
  CHECK((use_single_bus && *use_single_bus == true));

  REQUIRE(matrix_eps.has_value());
  CHECK((matrix_eps && *matrix_eps == doctest::Approx(0.01)));

  REQUIRE(output_directory.has_value());
  CHECK((output_directory && *output_directory == "/results"));

  REQUIRE(output_format.has_value());
  CHECK((output_format && *output_format == "parquet"));

  REQUIRE(output_compression.has_value());
  CHECK((output_compression && *output_compression == "zstd"));

  // Apply to planning
  Planning planning {};
  apply_cli_options(planning,
                    use_single_bus,
                    get_opt<bool>(vm, "use-kirchhoff"),
                    get_opt<std::string>(vm, "input-directory"),
                    get_opt<std::string>(vm, "input-format"),
                    output_directory,
                    output_format,
                    output_compression);

  REQUIRE(planning.options.model_options.use_single_bus.has_value());
  CHECK((planning.options.model_options.use_single_bus
         && *planning.options.model_options.use_single_bus == true));
  CHECK_FALSE(planning.options.model_options.use_kirchhoff.has_value());

  // Build flat options (enable_names is internal-only, set directly)
  auto flat_opts = make_lp_matrix_options(true, matrix_eps);
  CHECK(flat_opts.eps == doctest::Approx(0.01));
  CHECK(flat_opts.col_with_names == true);
  CHECK(flat_opts.col_with_name_map == true);
}

// ---- Tests for LPAlgo NamedEnum (solver_options.hpp) ----

TEST_CASE("LPAlgo enum_from_name - recognises all valid names")  // NOLINT
{
  CHECK(enum_from_name<LPAlgo>("default").value_or(LPAlgo::barrier)
        == LPAlgo::default_algo);
  CHECK(enum_from_name<LPAlgo>("primal").value_or(LPAlgo::default_algo)
        == LPAlgo::primal);
  CHECK(enum_from_name<LPAlgo>("dual").value_or(LPAlgo::default_algo)
        == LPAlgo::dual);
  CHECK(enum_from_name<LPAlgo>("barrier").value_or(LPAlgo::default_algo)
        == LPAlgo::barrier);
}

TEST_CASE("LPAlgo enum_from_name - returns nullopt for unknown name")  // NOLINT
{
  CHECK_FALSE(enum_from_name<LPAlgo>("interior").has_value());
  CHECK_FALSE(enum_from_name<LPAlgo>("").has_value());
  CHECK_FALSE(enum_from_name<LPAlgo>("bogus").has_value());
}

TEST_CASE("LPAlgo enum_from_name - ASCII case-insensitive")  // NOLINT
{
  // enum_from_name folds ASCII case, so these all match the lowercase
  // "barrier" entry in the table.
  CHECK(enum_from_name<LPAlgo>("Barrier").value_or(LPAlgo::default_algo)
        == LPAlgo::barrier);
  CHECK(enum_from_name<LPAlgo>("BARRIER").value_or(LPAlgo::default_algo)
        == LPAlgo::barrier);
  CHECK(enum_from_name<LPAlgo>("Primal").value_or(LPAlgo::default_algo)
        == LPAlgo::primal);
}

TEST_CASE("LPAlgo enum_name - round-trips all enumerators")  // NOLINT
{
  CHECK(enum_name(LPAlgo::default_algo) == "default");
  CHECK(enum_name(LPAlgo::primal) == "primal");
  CHECK(enum_name(LPAlgo::dual) == "dual");
  CHECK(enum_name(LPAlgo::barrier) == "barrier");
}

TEST_CASE("LPAlgo enum_name - unknown value returns 'unknown'")  // NOLINT
{
  CHECK(enum_name(LPAlgo::last_algo) == "unknown");
}

// ---- Tests for parse_lp_algorithm ----

TEST_CASE("parse_lp_algorithm - accepts algorithm names")  // NOLINT
{
  CHECK(parse_lp_algorithm("default") == LPAlgo::default_algo);
  CHECK(parse_lp_algorithm("primal") == LPAlgo::primal);
  CHECK(parse_lp_algorithm("dual") == LPAlgo::dual);
  CHECK(parse_lp_algorithm("barrier") == LPAlgo::barrier);
}

TEST_CASE("parse_lp_algorithm - accepts numeric strings")  // NOLINT
{
  CHECK(parse_lp_algorithm("0") == LPAlgo::default_algo);
  CHECK(parse_lp_algorithm("1") == LPAlgo::primal);
  CHECK(parse_lp_algorithm("2") == LPAlgo::dual);
  CHECK(parse_lp_algorithm("3") == LPAlgo::barrier);
}

TEST_CASE(
    "parse_lp_algorithm - rejects unknown names and out-of-range numbers")  // NOLINT
{
  const auto throws = [](const std::string& s)
  { [[maybe_unused]] auto r = parse_lp_algorithm(s); };
  CHECK_THROWS_AS(throws("interior"), cli::parse_error);
  // "Barrier" (mixed case) is now accepted because enum_from_name is
  // ASCII case-insensitive — use a genuinely unknown token instead.
  CHECK_THROWS_AS(throws("bogus"), cli::parse_error);
  CHECK_THROWS_AS(throws("4"), cli::parse_error);
  CHECK_THROWS_AS(throws("-1"), cli::parse_error);
  CHECK_THROWS_AS(throws("abc"), cli::parse_error);
}

TEST_CASE("parse_lp_algorithm - accepts mixed-case names")  // NOLINT
{
  // Round-trip verification that the case-insensitive lookup flows all
  // the way through parse_lp_algorithm and produces the same int as the
  // lowercase name.
  CHECK(parse_lp_algorithm("Barrier") == parse_lp_algorithm("barrier"));
  CHECK(parse_lp_algorithm("PRIMAL") == parse_lp_algorithm("primal"));
  CHECK(parse_lp_algorithm("Dual") == parse_lp_algorithm("dual"));
  CHECK(parse_lp_algorithm("Default") == parse_lp_algorithm("default"));
}

// ---- Tests for parse_aperture_chunk_size ----

TEST_CASE("parse_aperture_chunk_size — 'auto' maps to 0")  // NOLINT
{
  // "auto" is the user-friendly alias for the JSON sentinel 0; the
  // downstream `SDDPMethod::initialize_solver` resolves 0 via
  // `compute_auto_aperture_chunk_size` (formula + power-of-2 round).
  CHECK(parse_aperture_chunk_size("auto") == 0);
  // Case-insensitive.
  CHECK(parse_aperture_chunk_size("AUTO") == 0);
  CHECK(parse_aperture_chunk_size("Auto") == 0);
  CHECK(parse_aperture_chunk_size("aUtO") == 0);
}

TEST_CASE("parse_aperture_chunk_size — integer literals")  // NOLINT
{
  CHECK(parse_aperture_chunk_size("0") == 0);
  CHECK(parse_aperture_chunk_size("1") == 1);
  CHECK(parse_aperture_chunk_size("4") == 4);
  CHECK(parse_aperture_chunk_size("16") == 16);
  // Negative sentinel for "fully serial".
  CHECK(parse_aperture_chunk_size("-1") == -1);
}

TEST_CASE("parse_aperture_chunk_size — invalid input → nullopt")  // NOLINT
{
  CHECK_FALSE(parse_aperture_chunk_size("").has_value());
  CHECK_FALSE(parse_aperture_chunk_size("bogus").has_value());
  CHECK_FALSE(parse_aperture_chunk_size("auto7").has_value());
  CHECK_FALSE(parse_aperture_chunk_size("7auto").has_value());
}

// ---- Tests for --aperture-chunk-size CLI option ----

TEST_CASE(
    "--aperture-chunk-size — 'auto' string parses to sentinel 0")  // NOLINT
{
  auto desc = make_options_description();
  auto vm = parse_args({"--aperture-chunk-size", "auto"}, desc);
  REQUIRE(vm.contains("aperture-chunk-size"));
  const auto opts = parse_main_options(vm, {});
  REQUIRE(opts.sddp_aperture_chunk_size.has_value());
  CHECK(*opts.sddp_aperture_chunk_size == 0);
}

TEST_CASE("--aperture-chunk-size — integer literal parses directly")  // NOLINT
{
  auto desc = make_options_description();

  SUBCASE("K=4")
  {
    auto vm = parse_args({"--aperture-chunk-size", "4"}, desc);
    REQUIRE(vm.contains("aperture-chunk-size"));
    const auto opts = parse_main_options(vm, {});
    REQUIRE(opts.sddp_aperture_chunk_size.has_value());
    CHECK(*opts.sddp_aperture_chunk_size == 4);
  }

  SUBCASE("K=-1 (fully serial)")
  {
    auto vm = parse_args({"--aperture-chunk-size", "-1"}, desc);
    REQUIRE(vm.contains("aperture-chunk-size"));
    const auto opts = parse_main_options(vm, {});
    REQUIRE(opts.sddp_aperture_chunk_size.has_value());
    CHECK(*opts.sddp_aperture_chunk_size == -1);
  }
}

// ---- Tests for --algorithm CLI option ----

TEST_CASE("--algorithm - accepts name via CLI")  // NOLINT
{
  auto desc = make_options_description();

  SUBCASE("barrier name")
  {
    auto vm = parse_args({"--algorithm", "barrier"}, desc);
    REQUIRE(vm.contains("algorithm"));
    const auto opts = parse_main_options(vm, {});
    REQUIRE(opts.algorithm.has_value());
    CHECK(*opts.algorithm == LPAlgo::barrier);
  }

  SUBCASE("primal name")
  {
    auto vm = parse_args({"--algorithm", "primal"}, desc);
    const auto opts = parse_main_options(vm, {});
    CHECK(*opts.algorithm == LPAlgo::primal);
  }

  SUBCASE("dual name")
  {
    auto vm = parse_args({"--algorithm", "dual"}, desc);
    const auto opts = parse_main_options(vm, {});
    CHECK(*opts.algorithm == LPAlgo::dual);
  }

  SUBCASE("default name")
  {
    auto vm = parse_args({"--algorithm", "default"}, desc);
    const auto opts = parse_main_options(vm, {});
    CHECK(*opts.algorithm == LPAlgo::default_algo);
  }
}

TEST_CASE("--algorithm - accepts numeric value via CLI")  // NOLINT
{
  auto desc = make_options_description();
  auto vm = parse_args({"--algorithm", "2"}, desc);
  const auto opts = parse_main_options(vm, {});
  REQUIRE(opts.algorithm.has_value());
  CHECK(*opts.algorithm == LPAlgo::dual);
}

TEST_CASE("-a short option - accepts name")  // NOLINT
{
  auto desc = make_options_description();
  auto vm = parse_args({"-a", "barrier"}, desc);
  const auto opts = parse_main_options(vm, {});
  REQUIRE(opts.algorithm.has_value());
  CHECK(*opts.algorithm == LPAlgo::barrier);
}

TEST_CASE("--algorithm - invalid name throws at parse_main_options")  // NOLINT
{
  auto desc = make_options_description();
  auto vm = parse_args({"--algorithm", "unknown_algo"}, desc);
  // Wrap in lambda to avoid [[nodiscard]] warning on the throw path
  CHECK_THROWS_AS([&]
                  { [[maybe_unused]] auto r = parse_main_options(vm, {}); }(),
                  cli::parse_error);
}

// ---- Tests for --recover CLI flag ----

TEST_CASE("--recover flag - parsed via CLI")  // NOLINT
{
  auto desc = make_options_description();

  SUBCASE("not passed → recover is nullopt")
  {
    auto vm = parse_args({}, desc);
    const auto opts = parse_main_options(vm, {});
    CHECK_FALSE(opts.recover.has_value());
  }

  SUBCASE("--recover → recover is true")
  {
    auto vm = parse_args({"--recover"}, desc);
    const auto opts = parse_main_options(vm, {});
    REQUIRE(opts.recover.has_value());
    CHECK(opts.recover.value_or(false) == true);
  }

  SUBCASE("--recover false → recover is false")
  {
    auto vm = parse_args({"--recover", "false"}, desc);
    const auto opts = parse_main_options(vm, {});
    REQUIRE(opts.recover.has_value());
    CHECK(opts.recover.value_or(true) == false);
  }
}

TEST_CASE("--recover gates recovery_mode in apply_cli_options")  // NOLINT
{
  SUBCASE("without --recover, recovery_mode forced to none")
  {
    Planning planning {};
    planning.options.sddp_options.recovery_mode = RecoveryMode::full;
    apply_cli_options(planning, MainOptions {});
    REQUIRE(planning.options.sddp_options.recovery_mode.has_value());
    CHECK(
        planning.options.sddp_options.recovery_mode.value_or(RecoveryMode::full)
        == RecoveryMode::none);
  }

  SUBCASE("with --recover=true, recovery_mode preserved from JSON")
  {
    Planning planning {};
    planning.options.sddp_options.recovery_mode = RecoveryMode::full;
    apply_cli_options(planning,
                      MainOptions {
                          .recover = true,
                      });
    REQUIRE(planning.options.sddp_options.recovery_mode.has_value());
    CHECK(
        planning.options.sddp_options.recovery_mode.value_or(RecoveryMode::none)
        == RecoveryMode::full);
  }

  SUBCASE("with --recover=true and no JSON recovery_mode, default applies")
  {
    Planning planning {};
    apply_cli_options(planning,
                      MainOptions {
                          .recover = true,
                      });
    // recovery_mode not set in JSON → stays nullopt, PlanningOptionsLP default
    // is "full"
    CHECK_FALSE(planning.options.sddp_options.recovery_mode.has_value());
  }

  SUBCASE("with --recover=false, recovery_mode forced to none")
  {
    Planning planning {};
    planning.options.sddp_options.recovery_mode = RecoveryMode::cuts;
    apply_cli_options(planning,
                      MainOptions {
                          .recover = false,
                      });
    REQUIRE(planning.options.sddp_options.recovery_mode.has_value());
    CHECK(
        planning.options.sddp_options.recovery_mode.value_or(RecoveryMode::full)
        == RecoveryMode::none);
  }
}

TEST_CASE("--build-mode routes into PlanningOptions::build_mode")  // NOLINT
{
  SUBCASE("no --build-mode leaves build_mode unset (→ default scene_parallel)")
  {
    Planning planning {};
    apply_cli_options(planning, MainOptions {});
    CHECK_FALSE(planning.options.build_mode.has_value());
  }

  SUBCASE("--build-mode=serial sets BuildMode::serial")
  {
    Planning planning {};
    apply_cli_options(planning,
                      MainOptions {
                          .build_mode = std::string {"serial"},
                      });
    REQUIRE(planning.options.build_mode.has_value());
    CHECK(planning.options.build_mode.value_or(BuildMode::full_parallel)
          == BuildMode::serial);
  }

  SUBCASE("--build-mode=scene-parallel sets BuildMode::scene_parallel")
  {
    Planning planning {};
    apply_cli_options(planning,
                      MainOptions {
                          .build_mode = std::string {"scene-parallel"},
                      });
    REQUIRE(planning.options.build_mode.has_value());
    CHECK(planning.options.build_mode.value_or(BuildMode::full_parallel)
          == BuildMode::scene_parallel);
  }

  SUBCASE("--build-mode=full-parallel sets BuildMode::full_parallel")
  {
    Planning planning {};
    apply_cli_options(planning,
                      MainOptions {
                          .build_mode = std::string {"full-parallel"},
                      });
    REQUIRE(planning.options.build_mode.has_value());
    CHECK(planning.options.build_mode.value_or(BuildMode::serial)
          == BuildMode::full_parallel);
  }

  SUBCASE("--build-mode accepts underscore alias")
  {
    Planning planning {};
    apply_cli_options(planning,
                      MainOptions {
                          .build_mode = std::string {"scene_parallel"},
                      });
    REQUIRE(planning.options.build_mode.has_value());
    CHECK(planning.options.build_mode.value_or(BuildMode::full_parallel)
          == BuildMode::scene_parallel);
  }
}

// ---- Tests for parse_memory_size ----
//
// `parse_memory_size` is the CLI helper that converts strings like "300M"
// or "5G" into megabytes for `--memory-limit` and SDDP pool sizing.  All
// numeric paths feed straight into solver memory bounds, so silent
// mis-parsing would yield wildly wrong limits — hence we cover every
// suffix variant plus the error path.

TEST_CASE("parse_memory_size - empty string returns zero")
{
  CHECK(parse_memory_size("") == doctest::Approx(0.0));
}

TEST_CASE("parse_memory_size - bare number is interpreted as MB")
{
  CHECK(parse_memory_size("4096") == doctest::Approx(4096.0));
  CHECK(parse_memory_size("0") == doctest::Approx(0.0));
  CHECK(parse_memory_size("1.5") == doctest::Approx(1.5));
}

TEST_CASE("parse_memory_size - M and MB suffixes are megabytes")
{
  CHECK(parse_memory_size("300M") == doctest::Approx(300.0));
  CHECK(parse_memory_size("300MB") == doctest::Approx(300.0));
  CHECK(parse_memory_size("300m") == doctest::Approx(300.0));
  CHECK(parse_memory_size("300mb") == doctest::Approx(300.0));
}

TEST_CASE("parse_memory_size - G and GB suffixes are gigabytes (×1024 MB)")
{
  CHECK(parse_memory_size("5G") == doctest::Approx(5.0 * 1024.0));
  CHECK(parse_memory_size("5GB") == doctest::Approx(5.0 * 1024.0));
  CHECK(parse_memory_size("5g") == doctest::Approx(5.0 * 1024.0));
  CHECK(parse_memory_size("5gb") == doctest::Approx(5.0 * 1024.0));
  CHECK(parse_memory_size("1.5GB") == doctest::Approx(1536.0));
}

TEST_CASE("parse_memory_size - whitespace before suffix is tolerated")
{
  // The implementation strips leading spaces between the number and
  // the suffix so values like "300 M" round-trip cleanly.
  CHECK(parse_memory_size("300 M") == doctest::Approx(300.0));
  CHECK(parse_memory_size("2 GB") == doctest::Approx(2048.0));
}

TEST_CASE("parse_memory_size - invalid input throws cli::parse_error")
{
  CHECK_THROWS_AS([[maybe_unused]] auto v = parse_memory_size("abc"),
                  cli::parse_error);
}

TEST_CASE("parse_memory_size - unknown suffix throws cli::parse_error")
{
  // Only M/MB/G/GB (any case) are recognised; other unit letters must be
  // rejected so a typo does not silently get treated as megabytes.
  CHECK_THROWS_AS([[maybe_unused]] auto v = parse_memory_size("100K"),
                  cli::parse_error);
  CHECK_THROWS_AS([[maybe_unused]] auto v = parse_memory_size("100T"),
                  cli::parse_error);
  CHECK_THROWS_AS([[maybe_unused]] auto v = parse_memory_size("100kb"),
                  cli::parse_error);
}

// ---- Task 2B: --constraint-mode CLI shorthand --------------------------

TEST_CASE("apply_cli_options - --constraint-mode debug routes to options")
{
  // Round-trip the shorthand: MainOptions.constraint_mode='debug' must
  // land as planning.options.constraint_mode == ConstraintMode::debug,
  // mirroring the existing `--set planning_options.constraint_mode=debug`
  // path.  This is the matching gtopt-side leniency to
  // `plexos2gtopt --lax-uc-refs` — when iterating on parser bugs that
  // leave dangling LHS refs in the planning JSON.
  Planning planning {};
  MainOptions opts {
      .constraint_mode = "debug",
  };
  apply_cli_options(planning, opts);
  REQUIRE(planning.options.constraint_mode.has_value());
  CHECK(planning.options.constraint_mode.value() == ConstraintMode::debug);
}

TEST_CASE("apply_cli_options - --constraint-mode strict round-trips")
{
  // Strict is the gtopt default but must still round-trip as an explicit
  // override (so a user passing `--constraint-mode strict` on top of a
  // JSON that set `debug` is HONOURED — the CLI wins over JSON).
  Planning planning {};
  MainOptions opts {
      .constraint_mode = "strict",
  };
  apply_cli_options(planning, opts);
  REQUIRE(planning.options.constraint_mode.has_value());
  CHECK(planning.options.constraint_mode.value() == ConstraintMode::strict);
}

TEST_CASE("apply_cli_options - --constraint-mode normal round-trips")
{
  Planning planning {};
  MainOptions opts {
      .constraint_mode = "normal",
  };
  apply_cli_options(planning, opts);
  REQUIRE(planning.options.constraint_mode.has_value());
  CHECK(planning.options.constraint_mode.value() == ConstraintMode::normal);
}

TEST_CASE("apply_cli_options - --constraint-mode unknown throws")
{
  // Unknown values must hard-fail rather than silently fall back to the
  // default — same contract as every other CLI enum (output-format,
  // sddp-elastic-mode, …).
  Planning planning {};
  MainOptions opts {
      .constraint_mode = "loose",
  };
  CHECK_THROWS(apply_cli_options(planning, opts));
}

TEST_CASE(
    "apply_cli_options - --constraint-mode unset leaves options unchanged")
{
  // No CLI override → JSON / library defaults survive.  Important so a
  // user who runs `gtopt case.json` without `--constraint-mode` does
  // NOT have any JSON-shipped value silently overwritten.
  Planning planning {};
  planning.options.constraint_mode = ConstraintMode::debug;
  MainOptions opts {};  // constraint_mode left as nullopt
  apply_cli_options(planning, opts);
  REQUIRE(planning.options.constraint_mode.has_value());
  CHECK(planning.options.constraint_mode.value() == ConstraintMode::debug);
}

// NOLINTEND(bugprone-argument-comment,bugprone-unchecked-optional-access)