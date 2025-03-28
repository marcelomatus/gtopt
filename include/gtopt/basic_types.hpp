/**
 * @file      basic_types.hpp
 * @brief     Header of element basic types
 * @date      Tue Mar 18 13:10:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Basic types for element definitions.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "strong_type/formattable.hpp"

#define STRONG_HAS_FMT_FORMAT 1
#include <strong_type/strong_type.hpp>

namespace gtopt
{

using uid_t = int16_t;

using Uid = uid_t;
using OptUid = std::optional<Uid>;

using String = std::string;

using Name = std::string;
using OptName = std::optional<Name>;

using NameView = std::string_view;
using OptNameView = std::optional<Name>;

using Id = std::pair<Uid, NameView>;

using Real = double;
using OptReal = std::optional<Real>;

using Int = int;
using OptInt = std::optional<Int>;

using size_t = std::size_t;
using Size = size_t;
using OptSize = std::optional<Size>;

using Bool = bool;
using OptBool = std::optional<Bool>;

using IntBool = std::int8_t;
using OptIntBool = std::optional<IntBool>;

constexpr IntBool True = static_cast<IntBool>(true);
constexpr IntBool False = static_cast<IntBool>(false);

template<typename Type>
using Array = std::vector<Type>;

template<typename Type>
using StrongUidType = strong::type<uid_t,
                                   Type,
                                   strong::formattable,
                                   strong::regular,
                                   strong::hashable,
                                   strong::arithmetic,
                                   strong::implicitly_convertible_to<uid_t>>;

template<typename Type>
using StrongIndexType = strong::type<size_t,
                                     Type,
                                     strong::formattable,
                                     strong::regular,
                                     strong::hashable,
                                     strong::arithmetic,
                                     strong::bicrementable,
                                     strong::implicitly_convertible_to<size_t>>;

// basic constants

// 365*24 + 24/4; // includes leap year
constexpr double avg_year_hours = (365 * 24) + (24 * 0.25);

}  // namespace gtopt
