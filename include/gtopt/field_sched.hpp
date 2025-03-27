/**
 * @file      schedule.hpp
 * @brief     Header of FieldSched type
 * @date      Sat Mar 22 05:49:49 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides FieldSched types.
 */

#pragma once

#include <variant>

#include <gtopt/basic_types.hpp>

namespace gtopt
{
using FileSched = String;

template<typename Type, typename Vector = std::vector<Type>>
using FieldSched = std::variant<Type, Vector, FileSched>;

using RealFieldSched = FieldSched<Real>;
using OptRealFieldSched = std::optional<RealFieldSched>;

using IntFieldSched = FieldSched<Int>;
using OptIntFieldSched = std::optional<IntFieldSched>;

using BoolFieldSched = FieldSched<Bool>;
using OptBoolFieldSched = std::optional<BoolFieldSched>;

using IntBoolFieldSched = FieldSched<IntBool>;
using OptIntBoolFieldSched = std::optional<IntBoolFieldSched>;

using Active = IntBoolFieldSched;
using OptActive = OptIntBoolFieldSched;

}  // namespace gtopt
