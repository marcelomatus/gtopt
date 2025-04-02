#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/generator_profile.hpp>
#include <gtopt/json/json_generator.hpp>

namespace daw::json
{
using gtopt::GeneratorProfile;

template<>
struct json_data_contract<GeneratorProfile>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"generator", GeneratorVar, jvtl_GeneratorVar>,
      json_variant<"profile", STBRealFieldSched, jvtl_STBRealFieldSched>,
      json_variant_null<"scost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(GeneratorProfile const& generator_profile)
  {
    return std::forward_as_tuple(generator_profile.uid,
                                 generator_profile.name,
                                 generator_profile.active,
                                 generator_profile.generator,
                                 generator_profile.profile,
                                 generator_profile.scost);
  }
};
}  // namespace daw::json
