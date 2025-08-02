/**
 * @file      filtration.hpp
 * @brief     Filtration system model definition
 * @date      Thu Jul 31 23:22:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Filtration class representing a water filtration system in
 * hydrological modeling.
 */

#pragma once

#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Water filtration system model
 *
 * Represents a filtration system with connections to waterways and reservoirs,
 * including physical properties like slope and constant.
 */
struct Filtration
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status

  SingleId waterway {unknown_uid};  ///< Connected waterway identifier
  SingleId reservoir {unknown_uid};  ///< Connected reservoir identifier
  Real slope {0.0};  ///< Slope coefficient
  Real constant {0.0};  ///< Constant term
};

}  // namespace gtopt
