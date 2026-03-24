#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reservoir_production_factor.hpp>

namespace daw::json
{

using gtopt::ProductionFactorSegment;
using gtopt::ReservoirProductionFactor;

template<>
struct json_data_contract<ProductionFactorSegment>
{
  using type = json_member_list<json_number<"volume", Real>,
                                json_number<"slope", Real>,
                                json_number<"constant", Real>>;

  static constexpr auto to_json_data(ProductionFactorSegment const& seg)
  {
    return std::forward_as_tuple(seg.volume, seg.slope, seg.constant);
  }
};

template<>
struct json_data_contract<ReservoirProductionFactor>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"turbine", SingleId>,
      json_variant<"reservoir", SingleId>,
      json_number_null<"mean_production_factor", Real>,
      json_array_null<"segments",
                      std::vector<ProductionFactorSegment>,
                      ProductionFactorSegment>,
      json_number_null<"sddp_production_factor_update_skip", OptInt>>;

  static constexpr auto to_json_data(ReservoirProductionFactor const& re)
  {
    return std::forward_as_tuple(re.uid,
                                 re.name,
                                 re.active,
                                 re.turbine,
                                 re.reservoir,
                                 re.mean_production_factor,
                                 re.segments,
                                 re.sddp_production_factor_update_skip);
  }
};

}  // namespace daw::json
