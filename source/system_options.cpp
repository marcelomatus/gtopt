#include <gtopt/system_options.hpp>

namespace
{

template<typename T>
void merge_opt(std::optional<T>& a, const std::optional<T>& b)
{
  if (!b.has_value()) {
    return;
  }

  a = b;
}

template<typename T>
void merge_opt(std::optional<T>& a, std::optional<T>&& b)
{
  if (!b.has_value()) {
    return;
  }

  a = std::move(b);
}

}  // namespace

namespace gtopt
{

SystemOptions& SystemOptions::merge(SystemOptions& sys)
{
  merge_opt(input_directory, std::move(sys.input_directory));
  merge_opt(input_format, std::move(sys.input_format));
  merge_opt(output_directory, std::move(sys.output_directory));
  merge_opt(output_format, std::move(sys.output_format));
  merge_opt(annual_discount_rate, sys.annual_discount_rate);
  merge_opt(demand_fail_cost, sys.demand_fail_cost);
  merge_opt(reserve_fail_cost, sys.reserve_fail_cost);
  merge_opt(use_line_losses, sys.use_line_losses);
  merge_opt(use_kirchhoff, sys.use_kirchhoff);
  merge_opt(use_single_bus, sys.use_single_bus);
  merge_opt(kirchhoff_threshold, sys.kirchhoff_threshold);
  merge_opt(scale_objective, sys.scale_objective);
  merge_opt(scale_theta, sys.scale_theta);
  merge_opt(use_lp_names, sys.use_lp_names);
  merge_opt(use_uid_fname, sys.use_uid_fname);

  return *this;
}
}  // namespace gtopt
