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

#define STRONG_HAS_FMT_FORMAT 1
#include <strong_type/strong_type.hpp>

namespace gtopt
{

using uid_t = std::int16_t;

template<typename Type>
using StrongUidType = strong::type<uid_t,
                                   Type,
                                   strong::regular,
                                   strong::hashable,
                                   strong::arithmetic,
                                   strong::implicitly_convertible_to<uid_t>>;

using Uid = uid_t;
using OptUid = std::optional<Uid>;

using String = std::string;

using Name = std::string;
using OptName = std::optional<Name>;

using Real = double;
using OptReal = std::optional<Real>;

using Int = int;
using OptInt = std::optional<Int>;

using Bool = bool;
using OptBool = std::optional<Bool>;

using IntBool = std::int8_t;
using OptIntBool = std::optional<IntBool>;

}  // namespace gtopt
