#pragma once

#include <optional>

#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

struct Stage
{
  Uid uid {};
  Size first_block {};
  Size count_block {};
  OptReal discount_factor {1};
  OptBool active {};
  OptName name {};

  static constexpr std::string_view column_name = "stage";

  [[nodiscard]] constexpr auto id() const -> Id
  {
    if (name.has_value()) {
      return {uid, name.value()};
    }
    return {uid, as_label(column_name, uid)};
  }
};

using StageUid = StrongUidType<struct tuid_>;
using StageIndex = StrongIndexType<Stage>;
using OptStageIndex = std::optional<StageIndex>;

}  // namespace gtopt
