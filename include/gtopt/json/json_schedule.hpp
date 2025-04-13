#pragma once

#include <string>

#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::Block;
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

template<>
struct json_data_contract<Stage>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_number<"first_block", Size>,
                                json_number<"count_block", Size>,
                                json_number_null<"discount_factor", OptReal>,
                                json_number_null<"active", OptBool>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(Stage const& stage)
  {
    return std::forward_as_tuple(stage.uid,
                                 stage.first_block,
                                 stage.count_block,
                                 stage.discount_factor,
                                 stage.active,
                                 stage.name);
  }
};

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
