#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

struct Scenery
{
  Uid uid {};
  OptReal probability_factor {1};
  OptBool active {};
  OptName name {};

  static constexpr std::string_view column_name = "scenery";

  [[nodiscard]] constexpr auto id() const -> Id
  {
    if (name.has_value()) {
      return {uid, name.value()};
    }
    return {uid, as_label(column_name, uid)};
  }
};

using SceneryUid = StrongUidType<struct suid_>;
using SceneryIndex = StrongIndexType<Scenery>;
}  // namespace gtopt
