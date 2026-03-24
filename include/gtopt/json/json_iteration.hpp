/**
 * @file      json_iteration.hpp
 * @brief     JSON serialization for Iteration objects
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides JSON serialization and deserialization for Iteration objects
 * using the daw::json library.
 */

#pragma once

#include <gtopt/iteration.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{
using gtopt::Index;
using gtopt::Iteration;

template<>
struct json_data_contract<Iteration>
{
  using type = json_member_list<json_number<"index", Index>,
                                json_number_null<"update_lp", OptBool>>;

  [[nodiscard]] constexpr static auto to_json_data(Iteration const& iteration)
  {
    return std::forward_as_tuple(iteration.index, iteration.update_lp);
  }
};
}  // namespace daw::json
