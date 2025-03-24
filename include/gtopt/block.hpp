#pragma once

#include <iterator>

#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

struct Block
{
  Uid uid {};
  double duration {};
  OptName name {};

  static constexpr std::string_view column_name = "block";

  [[nodiscard]] constexpr auto id() const -> Id
  {
    if (name.has_value()) {
      return {uid, name.value()};
    }
    return {uid, as_label(Block::column_name, uid)};
  }
};

using BlockUid = StrongUidType<struct buid_>;
using BlockIndex = StrongIndexType<Block>;

}  // namespace gtopt

namespace std
{

template<>
struct incrementable_traits<gtopt::BlockIndex>
{
  using difference_type = int;
};

}  // namespace std
