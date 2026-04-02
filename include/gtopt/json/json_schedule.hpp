#pragma once

#include <string>

#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/stage_enums.hpp>

namespace daw::json
{
using gtopt::Block;
using gtopt::MonthType;
using gtopt::Scenario;
using gtopt::Stage;

template<>
struct json_data_contract<Block>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_number<"duration", Real>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(Block const& block)
  {
    return std::forward_as_tuple(block.uid, block.duration, block.name);
  }
};

// NOTE: Stage contract is defined in json_stage.hpp (the canonical location).
// This duplicate was kept for backward compatibility but is no longer included
// anywhere.  If you need to serialize Stage, include json_stage.hpp instead.

template<>
struct json_data_contract<Scenario>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_number_null<"probability_factor", OptReal>,
                                json_number_null<"active", OptBool>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(Scenario const& scenario)
  {
    return std::forward_as_tuple(scenario.uid,
                                 scenario.probability_factor,
                                 scenario.active,
                                 scenario.name);
  }
};

}  // namespace daw::json
