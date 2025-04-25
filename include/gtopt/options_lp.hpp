/**
 * @file      options_lp.hpp
 * @brief     Header of
 * @date      Tue Apr 22 03:21:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/options.hpp>

namespace gtopt
{

class OptionsLP
{
public:
  static constexpr auto default_input_directory = "input";
  static constexpr auto default_input_format = "parquet";
  static constexpr Bool default_use_line_losses = true;
  static constexpr Bool default_use_kirchhoff = true;
  static constexpr Bool default_use_single_bus = false;
  static constexpr Real default_kirchhoff_threshold = 0;
  static constexpr Real default_scale_objective = 1'000;
  static constexpr Real default_scale_theta = 10 * 100 * 100;

  static constexpr auto default_output_directory = "output";
  static constexpr auto default_output_format = "parquet";
  static constexpr auto default_compression_format = "gzip";
  static constexpr Bool default_use_lp_names = false;
  static constexpr Bool default_use_uid_fname = false;
  static constexpr Real default_annual_discount_rate = 0.0;

  explicit OptionsLP(Options poptions = {})
      : m_options_(std::move(poptions))
  {
  }

  [[nodiscard]] constexpr auto input_directory() const
  {
    return m_options_.input_directory.value_or(default_input_directory);
  }

  [[nodiscard]] constexpr auto input_format() const
  {
    return m_options_.input_format.value_or(default_input_format);
  }

  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    return m_options_.demand_fail_cost;
  }

  [[nodiscard]] constexpr auto reserve_fail_cost() const
  {
    return m_options_.reserve_fail_cost;
  }

  [[nodiscard]] constexpr auto use_line_losses() const
  {
    return m_options_.use_line_losses.value_or(default_use_line_losses);
  }

  [[nodiscard]] constexpr auto use_kirchhoff() const
  {
    return m_options_.use_kirchhoff.value_or(default_use_kirchhoff);
  }

  [[nodiscard]] constexpr auto use_single_bus() const
  {
    return m_options_.use_single_bus.value_or(default_use_single_bus);
  }

  [[nodiscard]] constexpr auto scale_objective() const
  {
    return m_options_.scale_objective.value_or(default_scale_objective);
  }

  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    return m_options_.kirchhoff_threshold.value_or(default_kirchhoff_threshold);
  }

  [[nodiscard]] constexpr auto scale_theta() const
  {
    return m_options_.scale_theta.value_or(default_scale_theta);
  }

  [[nodiscard]] constexpr auto use_lp_names() const
  {
    return m_options_.use_lp_names.value_or(default_use_lp_names);
  }

  [[nodiscard]] constexpr auto use_uid_fname() const
  {
    return m_options_.use_uid_fname.value_or(default_use_uid_fname);
  }

  [[nodiscard]] constexpr auto output_directory() const
  {
    return m_options_.output_directory.value_or(default_output_directory);
  }

  [[nodiscard]] constexpr auto output_format() const
  {
    return m_options_.output_format.value_or(default_output_format);
  }

  [[nodiscard]] constexpr auto compression_format() const
  {
    return m_options_.compression_format.value_or(default_compression_format);
  }

  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    return m_options_.annual_discount_rate.value_or(
        default_annual_discount_rate);
  }

private:
  Options m_options_;
};

}  // namespace gtopt
