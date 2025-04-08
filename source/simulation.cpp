/**
 * @file      simulation.cpp
 * @brief     Header of
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <expected>
#include <filesystem>
#include <string>

#include <gtopt/simulation.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{

auto Simulation::run_lp(System system,
                        const std::optional<std::string>& lp_file,
                        const std::optional<int>& use_lp_names,
                        const std::optional<double>& matrix_eps,
                        const std::optional<bool>& just_create) -> result_type
{
  spdlog::stopwatch sw;

  SystemLP system_lp {std::move(system)};
  SPDLOG_INFO("system_lp creation {}", sw);

  LinearInterface lp_interface;
  {
    // Create the linear problem
    constexpr size_t reserve_size = 1'024;
    LinearProblem linear_problem(system_lp.name(), reserve_size);
    system_lp.add_to_lp(linear_problem);
    SPDLOG_INFO("lp creation {}", sw);

    // Configure options for flattening
    const auto eps = matrix_eps.value_or(0);
    const auto lp_names = use_lp_names.value_or(0);
    FlatOptions flat_opts;
    flat_opts.eps = eps;
    flat_opts.col_with_names = lp_names > 0;
    flat_opts.row_with_names = lp_names > 0;
    flat_opts.col_with_name_map = lp_names > 1;
    flat_opts.row_with_name_map = lp_names > 1;
    flat_opts.reserve_matrix = false;
    flat_opts.reserve_factor = 2;

    // Flatten the problem and load it
    auto flat_lp = linear_problem.to_flat(flat_opts);
    SPDLOG_INFO("lp flattening {}", sw);

    lp_interface.load_flat(flat_lp);
    SPDLOG_INFO("lp loading {}", sw);
  }

  if (lp_file) {
    const std::filesystem::path lpath {lp_file.value()};
    lp_interface.write_lp(lpath.stem());

    SPDLOG_INFO("lp writing {}", sw);
  }

  if (just_create.value_or(false)) {
    SPDLOG_INFO("just creating the problem, exiting now");
    return {0};
  }

  //
  // solve the problem
  //
  {
    const LPOptions lp_opts {};
    const auto status = lp_interface.resolve(lp_opts);

    if (!status) {
      lp_interface.write_lp("error");

      auto e =
          std::unexpected("problem is not feasible, check the error.lp file");

      SPDLOG_CRITICAL(e.error());

      return e;
    }

    SPDLOG_INFO("lp solving {}", sw);
  }

  //
  // write the output
  //
  {
    system_lp.write_out(lp_interface);

    SPDLOG_INFO("write output {}", sw);
  }

  return {0};
}

}  // namespace gtopt
