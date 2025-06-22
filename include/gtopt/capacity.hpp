/**
 * @file      capacity.hpp
 * @brief     Defines capacity-related structures for planning models
 * @date      Thu Mar 27 10:45:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines structures representing capacity attributes and entities
 * in planning models, including current capacity, expansion capabilities,
 * and associated costs. Capacity is a foundational concept used by many
 * components in the system modeling framework.
 *
 * @details Capacity modeling includes both fixed capacity and investment
 * decisions, providing the framework for generation, transmission, and storage
 * expansion planning. Time-scheduled parameters allow representing capacity
 * changes over the planning horizon.
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Capacity
 * @brief Complete capacity entity with identification and attributes
 *
 * @details This structure represents a full capacity entity with unique
 * identification and all capacity-related attributes.
 *
 * Capacity entities can be used directly or as base components for more
 * specialized system elements like generators, lines, or batteries.
 *
 * @see CapacityLP for planning model formulation
 */
struct Capacity
{
  Uid uid {unknown_uid};  ///< Unique identifier for database references
  Name name {};  ///< Human-readable descriptive name
  OptActive active {};  ///< Activation status (whether this entity is modeled)

  // Capacity attributes
  OptTRealFieldSched
      capacity {};  ///< Current operational capacity (installed units)
  OptTRealFieldSched expcap {};  ///< Potential expansion capacity (additional
                                 ///< units that can be built)
  OptTRealFieldSched expmod {};  ///< Expansion module size (minimum capacity
                                 ///< addition increment)
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity limit (upper
                                 ///< bound on total capacity)
  OptTRealFieldSched
      annual_capcost {};  ///< Annual cost of capacity (per unit per year)
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor (capacity
                                          ///< degradation rate)
};

}  // namespace gtopt
