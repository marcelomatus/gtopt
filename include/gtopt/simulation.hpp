/**
 * @file      simulation.hpp
 * @brief     Header of
 * @date      Sun Apr  6 18:18:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <expected>

#include <boost/multi_array.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

class Simulation
{
public:
  using result_type = std::expected<int, std::string>;

  static result_type run_lp(System system,
                            const std::optional<std::string>& lp_file,
                            const std::optional<int>& use_lp_names,
                            const std::optional<double>& matrix_eps,
                            const std::optional<bool>& just_create);

  void create_lp();

private:
  SystemLP system_lp;

  using lp_matrix_t = boost::multi_array<LinearProblem, 2>;
  lp_matrix_t lp_matrix;
};

}  // namespace gtopt
