/**
 * @file      junction.hpp
 * @brief     Header for junction components in power systems
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing junctions in power
 * system models. Junctions are connection points that can aggregate or
 * distribute flows.
 *
 * @details Junctions serve as topological nodes where multiple branches
 * connect. They can optionally model drain effects where energy is lost at the
 * junction.
 */

#pragma once

#include <gtopt/field_sched.hpp>

namespace gtopt
{

/**
 * @struct Junction
 * @brief Represents a junction point in a power system network
 *
 * @details This structure defines a connection point where multiple branches
 * (transmission lines, transformers etc.) meet. Junctions can model:
 * - Basic connectivity between components
 * - Optional drain effects (energy losses)
 * - Activation status for scenario modeling
 *
 */
struct Junction
{
  Uid uid {unknown_uid};  ///< Unique identifier for database references
  Name name {};  ///< Human-readable junction name
  OptActive active {};  ///< Activation status (whether junction is modeled)

  OptBool drain {};  ///< Whether junction has energy drain/loss effects
};

}  // namespace gtopt
