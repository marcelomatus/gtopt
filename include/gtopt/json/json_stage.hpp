/**
 * @file      json_stage.hpp
 * @brief     JSON serialization for Stage
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the daw::json data contract specialization for
 * serializing and deserializing Stage objects to and from JSON.
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/stage.hpp>

namespace daw::json
{
using gtopt::MonthType;
using gtopt::Stage;

/// Custom constructor: converts JSON month string → MonthType enum.
struct StageConstructor
{
  [[nodiscard]] Stage operator()(Uid uid,
                                 OptName name,
                                 OptBool active,
                                 Size first_block,
                                 Size count_block,
                                 OptReal discount_factor,
                                 OptName month_str) const
  {
    Stage s;
    s.uid = uid;
    s.name = std::move(name);
    s.active = active;
    s.first_block = first_block;
    s.count_block = count_block;
    s.discount_factor = discount_factor;
    if (month_str) {
      s.month = gtopt::enum_from_name<MonthType>(*month_str);
    }
    return s;
  }
};

template<>
struct json_data_contract<Stage>
{
  using constructor_t = StageConstructor;

  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number<"first_block", Size>,
                                json_number<"count_block", Size>,
                                json_number_null<"discount_factor", OptReal>,
                                json_string_null<"month", OptName>>;

  static auto to_json_data(Stage const& stage)
  {
    return std::make_tuple(stage.uid,
                           stage.name,
                           stage.active,
                           stage.first_block,
                           stage.count_block,
                           stage.discount_factor,
                           detail::enum_to_opt_name(stage.month));
  }
};
}  // namespace daw::json
