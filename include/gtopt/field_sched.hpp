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

template<typename Type, typename Vector2 = std::vector<std::vector<Type>>>
using FieldSched2 = FieldSched<Type, Vector2>;

using RealFieldSched2 = FieldSched2<double>;
using OptRealFieldSched2 = std::optional<RealFieldSched2>;

using OptRealFieldSched2 = std::optional<RealFieldSched2>;

using RealFieldSched2 = FieldSched2<double>;
using OptRealFieldSched2 = std::optional<RealFieldSched2>;

template<typename Type,
         typename Vector3 = std::vector<std::vector<std::vector<Type>>>>
using FieldSched3 = FieldSched<Type, Vector3>;

using RealFieldSched3 = FieldSched3<double>;
using OptRealFieldSched3 = std::optional<RealFieldSched3>;

using TRealFieldSched = RealFieldSched;
using TBRealFieldSched = RealFieldSched2;
using STRealFieldSched = RealFieldSched2;
using STBRealFieldSched = RealFieldSched3;

using OptTRealFieldSched = OptRealFieldSched;
using OptTBRealFieldSched = OptRealFieldSched2;
using OptSTRealFieldSched = OptRealFieldSched2;
using OptSTBRealFieldSched = OptRealFieldSched3;

}  // namespace gtopt
