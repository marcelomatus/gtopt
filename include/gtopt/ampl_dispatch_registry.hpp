/**
 * @file      ampl_dispatch_registry.hpp
 * @brief     One-shot population of the AMPL param + iterator tables
 * @date      2026-05-19
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each `*LP` class exposes its `param_*` accessors and its
 * `Collection<LP>` enumeration to user constraints through a pair of
 * function-pointer registries on `SimulationLP`
 * (`AmplParamMap`, `AmplIterMap`).  The two helpers declared here are
 * called once from `system_lp.cpp::register_all_ampl_element_names`
 * inside its `std::call_once` block — populating the tables out of the
 * `SystemLP` constructor's parallel scene-build loop.  After
 * registration both maps are read-only.
 *
 * The bodies live in `source/ampl_dispatch_registry.cpp` so that
 * `system_lp.cpp` stays focused on system orchestration and the
 * boilerplate `sc.get_element(...).param_X(s, b)` shims live with their
 * peers.
 */

#pragma once

namespace gtopt
{

class SimulationLP;

/// Populate `(class_name, attribute) -> AmplParamFn` on @p sim with one
/// entry per user-facing PAMPL attribute that resolves to a scalar
/// parameter (i.e., a `param_X` accessor on the matching `*LP` class).
void register_ampl_param_dispatchers(SimulationLP& sim);

/// Populate `class_name -> AmplIterFn` on @p sim with one entry per LP
/// class that supports `sum(class(all)...)` enumeration.
void register_ampl_iterator_dispatchers(SimulationLP& sim);

}  // namespace gtopt
