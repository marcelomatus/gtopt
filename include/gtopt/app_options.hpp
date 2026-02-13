/**
 * @file      app_options.hpp
 * @brief     Application command-line option parsing and configuration
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides utility functions for parsing command-line options
 * using a modern C++ command-line parser, applying parsed options to Planning
 * configurations, and building FlatOptions from command-line parameters.
 */

#pragma once

#include <optional>
#include <string>

#include <gtopt/cli_options.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning.hpp>

namespace gtopt
{

namespace po = cli;

/**
 * @brief Extract an optional value from a variables_map
 *
 * @tparam T The type of the value to extract
 * @param vm The variables map containing parsed options
 * @param name The name of the option to look up
 * @return std::optional<T> containing the value if present, std::nullopt
 * otherwise
 */
template<typename T>
[[nodiscard]] std::optional<T> get_opt(const po::variables_map& vm,
                                       const std::string& name)
{
  if (vm.contains(name)) {
    return vm[name].as<T>();
  }
  return std::nullopt;
}

/**
 * @brief Create the command-line options description for the gtopt application
 *
 * @return po::options_description The options description containing all
 * supported options
 */
[[nodiscard]] inline po::options_description make_options_description()
{
  po::options_description desc("Gtoptp options");
  desc.add_options()("help,h", "describes arguments")  //
      ("verbose,v", "activates maximum verbosity")  //
      ("quiet,q",
       po::value<bool>().implicit_value(/*v=*/true),
       "do not log in the stdout")  //
      ("version,V", "shows program version")  //
      ("system-file,s",
       po::value<std::vector<std::string>>(),
       "name of the system file")  //
      ("lp-file,l",
       po::value<std::string>(),
       "name of the lp file to save")  //
      ("json-file,j",
       po::value<std::string>(),
       "name of the json file to save")  //
      ("input-directory,D", po::value<std::string>(), "input directory")  //
      ("input-format,F", po::value<std::string>(), "input format")  //
      ("output-directory,d", po::value<std::string>(), "output directory")  //
      ("output-format,f",
       po::value<std::string>(),
       "output format [parquet, csv]")  //
      ("compression-format,C",
       po::value<std::string>(),
       "compression format in parquet [uncompressed, gzip, zstd, lzo]")  //
      ("use-single-bus,b",
       po::value<bool>().implicit_value(/*v=*/true),
       "use single bus mode")  //
      ("use-kirchhoff,k",
       po::value<bool>().implicit_value(/*v=*/true),
       "use kirchhoff mode")  //
      ("use-lp-names,n",
       po::value<int>().implicit_value(1),
       "use real col/row names in the lp file")  //
      ("matrix-eps,e",
       po::value<double>(),
       "eps value to define A matrix non-zero values")  //
      ("just-create,c",
       po::value<bool>().implicit_value(/*v=*/true),
       "just create the problem, then exit")  //
      ("fast-parsing,p",
       po::value<bool>().implicit_value(/*v=*/true),
       "use fast (non strict) json parsing");
  return desc;
}

/**
 * @brief Apply command-line options to a Planning object
 *
 * Updates the planning options based on parsed command-line values.
 *
 * @param planning The Planning object to update
 * @param use_single_bus Optional single-bus mode flag
 * @param use_kirchhoff Optional Kirchhoff mode flag
 * @param use_lp_names Optional LP names level (0=none, 1=names, 2=names+map)
 * @param input_directory Optional input directory path
 * @param input_format Optional input format string
 * @param output_directory Optional output directory path
 * @param output_format Optional output format string
 * @param compression_format Optional compression format string
 */
inline void apply_cli_options(
    Planning& planning,
    const std::optional<bool>& use_single_bus,
    const std::optional<bool>& use_kirchhoff,
    const std::optional<int>& use_lp_names,
    const std::optional<std::string>& input_directory,
    const std::optional<std::string>& input_format,
    const std::optional<std::string>& output_directory,
    const std::optional<std::string>& output_format,
    const std::optional<std::string>& compression_format)
{
  if (use_single_bus) {
    planning.options.use_single_bus = use_single_bus;
  }

  if (use_kirchhoff) {
    planning.options.use_kirchhoff = use_kirchhoff;
  }

  if (use_lp_names) {
    planning.options.use_lp_names = use_lp_names.value();
  }

  if (output_directory) {
    planning.options.output_directory = output_directory.value();
  }

  if (input_directory) {
    planning.options.input_directory = input_directory.value();
  }

  if (output_format) {
    planning.options.output_format = output_format.value();
  }

  if (compression_format) {
    planning.options.compression_format = compression_format.value();
  }

  if (input_format) {
    planning.options.input_format = input_format.value();
  }
}

/**
 * @brief Build FlatOptions from command-line parameters
 *
 * @param use_lp_names Optional LP names level (0=none, 1=names, 2=names+map)
 * @param matrix_eps Optional epsilon tolerance for matrix coefficients
 * @return FlatOptions configured according to the parameters
 */
[[nodiscard]] inline FlatOptions make_flat_options(
    const std::optional<int>& use_lp_names,
    const std::optional<double>& matrix_eps)
{
  const auto eps = matrix_eps.value_or(0);
  const auto lp_names = use_lp_names.value_or(true);

  FlatOptions flat_opts;
  flat_opts.eps = eps;
  flat_opts.col_with_names = lp_names > 0;
  flat_opts.row_with_names = lp_names > 0;
  flat_opts.col_with_name_map = lp_names > 1;
  flat_opts.row_with_name_map = lp_names > 1;
  flat_opts.reserve_matrix = false;
  flat_opts.reserve_factor = 2;

  return flat_opts;
}

}  // namespace gtopt
