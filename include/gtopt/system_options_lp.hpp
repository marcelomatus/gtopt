#pragma once

#include <gtopt/system_options.hpp>

namespace gtopt
{

constexpr auto default_input_directory = "input";
constexpr auto default_input_format = "parquet";
constexpr auto default_output_directory = "output";
constexpr auto default_output_format = "parquet";
constexpr auto default_compression_format = "gzip";
constexpr Real default_annual_discount_rate = 0.0;
constexpr Bool default_use_lp_names = false;
constexpr Bool default_use_line_losses = true;
constexpr Bool default_use_kirchhoff = true;
constexpr Bool default_use_single_bus = false;
constexpr Real default_kirchhoff_threshold = 0;
constexpr Real default_scale_objective = 1'000;
constexpr Real default_scale_theta = 10 * 100 * 100;
constexpr Bool default_use_uid_fname = false;

class SystemOptionsLP
{
public:
  SystemOptionsLP() = default;

  explicit SystemOptionsLP(SystemOptions psystem_options)
      : system_options(std::move(psystem_options))
  {
  }

  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    return system_options.demand_fail_cost;
  }

  [[nodiscard]] constexpr auto reserve_fail_cost() const
  {
    return system_options.reserve_fail_cost;
  }

  [[nodiscard]] constexpr bool use_lp_names() const
  {
    return system_options.use_lp_names.value_or(default_use_lp_names);
  }

  [[nodiscard]] constexpr bool use_line_losses() const
  {
    return system_options.use_line_losses.value_or(default_use_line_losses);
  }

  [[nodiscard]] constexpr bool use_kirchhoff() const
  {
    return system_options.use_kirchhoff.value_or(default_use_kirchhoff);
  }

  [[nodiscard]] constexpr bool use_single_bus() const
  {
    return system_options.use_single_bus.value_or(default_use_single_bus);
  }

  [[nodiscard]] constexpr auto scale_objective() const
  {
    return system_options.scale_objective.value_or(default_scale_objective);
  }

  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    return system_options.kirchhoff_threshold.value_or(
        default_kirchhoff_threshold);
  }

  [[nodiscard]] constexpr auto scale_theta() const
  {
    return system_options.scale_theta.value_or(default_scale_theta);
  }

  [[nodiscard]] constexpr bool use_uid_fname() const
  {
    return system_options.use_uid_fname.value_or(default_use_uid_fname);
  }

  [[nodiscard]] auto input_directory() const
  {
    return system_options.input_directory.value_or(default_input_directory);
  }

  [[nodiscard]] auto input_format() const
  {
    return system_options.input_format.value_or(default_input_format);
  }

  [[nodiscard]] auto output_directory() const
  {
    return system_options.output_directory.value_or(default_output_directory);
  }

  [[nodiscard]] auto output_format() const
  {
    return system_options.output_format.value_or(default_output_format);
  }

  [[nodiscard]] auto compression_format() const
  {
    return system_options.compression_format.value_or(
        default_compression_format);
  }

  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    return system_options.annual_discount_rate.value_or(
        default_annual_discount_rate);
  }

private:
  SystemOptions system_options;
};

using OptSystemOptionsLP = std::optional<SystemOptionsLP>;

}  // namespace gtopt
