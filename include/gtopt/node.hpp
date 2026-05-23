/**
 * @file      node.hpp
 * @brief     Generic carrier-tagged balance-node template
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines ``Node<Carrier>`` — a typed balance node for any energy
 * carrier.  Concrete instantiations (``HydrogenNode``,
 * ``ThermalNode``) inherit from this template, get their own
 * ``class_name`` constant for LP row labels, and carry no other
 * fields — they're pure "balance-row identifier" objects.
 *
 * A ``Node<C>`` is the carrier-side analog of an electrical
 * ``Bus``: every flow with carrier ``C`` (storage charge/discharge,
 * generator output, demand) sums to zero at the node.  No voltage
 * angle, no Kirchhoff's law, no reactance — those are
 * electric-specific concepts that stay on ``Bus`` (which will later
 * inherit from ``Node<Carrier::Electric>`` in a separate
 * refactor).
 */

#pragma once

#include <gtopt/carrier.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/object_utils.hpp>

namespace gtopt
{

/**
 * @brief Generic carrier-tagged balance node.
 *
 * The template parameter ``C`` (a ``Carrier`` enum value) imprints
 * the carrier identity onto the concrete type at compile time.  Two
 * nodes with different ``C`` are distinct C++ types, so any storage
 * / generator / converter that holds a ``SingleId<Node<C>>``
 * reference is automatically restricted to nodes of carrier ``C``.
 *
 * @tparam C Energy carrier this node operates on.
 *
 * @note The compile-time member ``carrier`` exposes the tag to
 *       callers that need to dispatch on it (e.g. validators).
 *       Concrete subclasses (``HydrogenNode``, ``ThermalNode``) add
 *       a ``class_name`` constant for LP row labels.
 */
template<Carrier C>
struct Node
{
  /// Carrier tag — compile-time accessible.
  static constexpr Carrier carrier = C;

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)
};

}  // namespace gtopt
