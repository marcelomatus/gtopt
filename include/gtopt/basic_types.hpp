/**
 * @file      basic_types.hpp
 * @brief     Fundamental type definitions for power system planning
 * @date      Tue Mar 18 13:10:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the basic types and type aliases used throughout the
 * gtopt codebase for power system planning modeling. It includes numeric
 * types, string types, optional variants, and strong types for type safety.
 */

#pragma once

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#define STRONG_HAS_STD_FORMAT 1
#include <strong_type/formattable.hpp>
#include <strong_type/strong_type.hpp>

namespace gtopt
{

/**
 * @brief Base type for unique identifiers
 * @details Used for entity identification in the power system model
 */
using uid_t = std::int32_t;

/** @brief Unique identifier type */
using Uid = uid_t;

constexpr Uid unknown_uid = -1;

/** @brief Optional unique identifier type */
using OptUid = std::optional<Uid>;

/** @brief String type for general textual data */
using String = std::string;

/** @brief Name type for entity identification */
using Name = std::string;
/** @brief Optional name type */
using OptName = std::optional<Name>;

/** @brief String view type for non-owning name references */
using NameView = std::string_view;

/** @brief Combined identifier with both numeric ID and name */
using Id = std::pair<Uid, Name>;

/** @brief Real number type for physical quantities and costs */
using Real = double;
/** @brief Optional real number type */
using OptReal = std::optional<Real>;

/** @brief Integer type for counting and discrete quantities */
using Int = int;
/** @brief Optional integer type */
using OptInt = std::optional<Int>;

/** @brief Size type for container dimensions and indices */
using Size = std::size_t;

/** @brief Index type for large-scale sparse matrices and arrays */
using Index = std::int32_t;

constexpr Index unknown_index = std::numeric_limits<Index>::min();

/** @brief Boolean type for logical conditions */
using Bool = bool;
/** @brief Optional boolean type */
using OptBool = std::optional<Bool>;

/** @brief Integer boolean for space-efficient storage */
using IntBool = std::int32_t;
/** @brief Optional integer boolean type */
using OptIntBool = std::optional<IntBool>;

/** @brief True value for integer boolean */
constexpr IntBool True = static_cast<IntBool>(true);
/** @brief False value for integer boolean */
constexpr IntBool False = static_cast<IntBool>(false);

/**
 * @brief Array type template for consistent vector usage
 * @tparam Type The element type to store in the array
 */
template<typename Type>
using Array = std::vector<Type>;

/**
 * @brief Strong type for count-like indices (LP rows, LP columns).
 * @tparam Type The tag type for compile-time type checking
 *
 * Retains full arithmetic because row/column positions may need to
 * shift after deletions (`row -= RowIndex{k}`) and to participate in
 * offset bookkeeping.  For positional indices that should never take
 * part in free-form arithmetic (scene, phase, stage, scenario, block)
 * use `StrongPositionIndexType` instead.
 */
template<typename Type>
using StrongIndexType = strong::type<Index,
                                     Type,
                                     strong::formattable,
                                     strong::regular,
                                     strong::hashable,
                                     strong::arithmetic,
                                     strong::bicrementable,
                                     strong::implicitly_convertible_to<Index>>;

/**
 * @brief Strong type for positional indices into a logical sequence
 *        (scene, phase, stage, scenario, block).
 * @tparam Type The tag type for compile-time type checking
 *
 * Unlike `StrongIndexType`, these indices do **not** support free-form
 * arithmetic (`idx + idx`, `idx * k`, …) — stepping is done through the
 * `next()` / `previous()` helpers and through `iota_range`.  They are
 * contextually convertible to bool so that `!idx` detects the zeroth
 * element (`if (!phase_index)` ≡ "first phase").
 */
template<typename Type>
using StrongPositionIndexType =
    strong::type<Index,
                 Type,
                 strong::formattable,
                 strong::regular,
                 strong::ordered,
                 strong::hashable,
                 strong::bicrementable,
                 strong::boolean,
                 strong::implicitly_convertible_to<Index>>;

}  // namespace gtopt

/// Specialise std::incrementable_traits for StrongIndexType so that
/// it satisfies std::weakly_incrementable and can be used with
/// IotaRange and other range adaptors.
template<typename Tag>
// NOLINTNEXTLINE(cert-dcl58-cpp) - valid std template specialization for
// user-defined type
struct std::incrementable_traits<gtopt::StrongIndexType<Tag>>
{
  using difference_type = gtopt::Index;
};

/// Same specialisation for `StrongPositionIndexType` so positional
/// indices remain usable with `IotaRange` and other range adaptors.
template<typename Tag>
// NOLINTNEXTLINE(cert-dcl58-cpp) - valid std template specialization for
// user-defined type
struct std::incrementable_traits<gtopt::StrongPositionIndexType<Tag>>
{
  using difference_type = gtopt::Index;
};

namespace gtopt
{

/** @brief Days in a standard year (non-leap) */
constexpr double days_per_year = 365.0;

/** @brief Hours in a standard day */
constexpr double hours_per_day = 24.0;

/** @brief Hours in a standard year */
constexpr double hours_per_year = days_per_year * hours_per_day;

}  // namespace gtopt
