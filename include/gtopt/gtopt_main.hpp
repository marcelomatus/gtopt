/**
 * @file      gtopt_main.hpp
 * @brief     Core application entry point for the gtopt optimizer
 * @date      Thu Feb 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the main optimization entry point that parses
 * planning JSON files, applies command-line options, builds and solves
 * the linear programming model, and writes the results.
 */

#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>

namespace gtopt
{

/**
 * @brief Run the gtopt power-system optimizer
 *
 * Reads one or more planning JSON files, applies the supplied command-line
 * options, constructs a @c PlanningLP model, optionally solves it, and
 * writes the solution output.
 *
 * @param planning_files     Paths to the input planning JSON files
 * @param input_directory    Optional override for the input data directory
 * @param input_format       Optional input format (e.g. "parquet", "csv")
 * @param output_directory   Optional override for the output directory
 * @param output_format      Optional output format (e.g. "parquet", "csv")
 * @param compression_format Optional compression for parquet output
 * @param use_single_bus     Optional single-bus mode flag
 * @param use_kirchhoff      Optional Kirchhoff-constraint mode flag
 * @param lp_file            Optional path to write the LP file
 * @param use_lp_names       Optional LP naming level (0=none, 1=names,
 *                           2=names+map)
 * @param matrix_eps         Optional epsilon tolerance for LP matrix
 *                           coefficients
 * @param json_file          Optional path to write the planning JSON
 * @param just_create        If true, build the LP but skip solving
 * @param fast_parsing       If true, use fast (non-strict) JSON parsing
 * @param print_stats        If true, print system and solution statistics
 * @return 0 on success, 1 on infeasibility, or an error message string
 */
[[nodiscard]] std::expected<int, std::string> gtopt_main(
    std::span<const std::string> planning_files,
    const std::optional<std::string>& input_directory,
    const std::optional<std::string>& input_format,
    const std::optional<std::string>& output_directory,
    const std::optional<std::string>& output_format,
    const std::optional<std::string>& compression_format,
    const std::optional<bool>& use_single_bus,
    const std::optional<bool>& use_kirchhoff,
    const std::optional<std::string>& lp_file,
    const std::optional<int>& use_lp_names,
    const std::optional<double>& matrix_eps,
    const std::optional<std::string>& json_file,
    const std::optional<bool>& just_create,
    const std::optional<bool>& fast_parsing,
    const std::optional<bool>& print_stats = std::nullopt);

}  // namespace gtopt
