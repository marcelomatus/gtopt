/**
 * @file      emission_lp.hpp
 * @brief     Parameter-carrier wrapper for the Emission pollutant tag
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionLP` is the LP-side wrapper for an `Emission` pollutant tag.
 * It is **passive** — no `add_to_lp` / `add_to_output` methods; the
 * visitor in `system_lp.cpp` gates on `AddToLP<T>` and skips this
 * type.  The constraint surface (cap / price / soft slack) lives on
 * `EmissionZone`; the per-generator contribution lives on
 * `EmissionSource`.  `EmissionLP` exists solely so other elements can
 * resolve a pollutant by uid/name via
 * `SystemContext::element<EmissionLP>(EmissionLPSId{...})`.
 *
 * @see Emission for the data struct
 * @see EmissionZone / EmissionSource for the LP-active constraint
 *      pieces
 */

#pragma once

#include <gtopt/emission.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

// Forward declarations
class InputContext;
class LinearProblem;
class OutputContext;
class SystemContext;

class EmissionLP : public ObjectLP<Emission>
{
public:
  using Base = ObjectLP<Emission>;

  explicit EmissionLP(const Emission& emission, const InputContext& ic);

  [[nodiscard]] constexpr auto&& emission(this auto&& self) noexcept
  {
    return self.object();
  }

  // Intentionally no `add_to_lp` / `add_to_output`.  Pure registry —
  // the visitor in `system_lp.cpp` gates on `AddToLP<T>` and skips
  // this type at compile time.
};

/// SingleId-style reference into `Collection<EmissionLP>`.  Consumed by
/// `EmissionZone.emissions[].emission` and `EmissionSource.emission`
/// resolvers.
using EmissionLPSId = ObjectSingleId<EmissionLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Emission::class_name` literal fails the build (LP row labels and
// output directory names depend on the exact string `"Emission"`).
static_assert(EmissionLP::Element::class_name == LPClassName {"Emission"},
              "Emission::class_name must remain \"Emission\"");

}  // namespace gtopt
