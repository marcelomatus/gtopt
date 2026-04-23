/**
 * @file      inertia_provision.hpp
 * @brief     Defines InertiaProvision – generator contribution to an inertia
 *            zone
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the InertiaProvision structure which specifies how a
 * synchronous generator contributes inertia to one or more inertia zones.
 *
 * The provision creates a variable r_inertia in [0, provision_max] with
 * a downward coupling constraint: p >= r_inertia (the generator must be
 * producing at least the inertia provision).
 *
 * The effectiveness factor (provision_factor) converts the MW provision
 * into MWs of inertia: FE = H * S / Pmin [MWs/MW].  The user may
 * specify either:
 * - `inertia_constant` (H [s]) + `rated_power` (S [MVA]) — the LP class
 *   computes FE = H * S / Pmin automatically.
 * - `provision_factor` directly [MWs/MW].
 *
 * When a SimpleCommitment exists for the same generator, the binary
 * coupling (p >= Pmin*u) already ensures r_inertia → 0 when u = 0.
 * When relaxed (u continuous), this gives PLP's Model B automatically.
 *
 * @see InertiaZone for zone-level requirements
 * @see InertiaProvisionLP for the LP formulation
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct InertiaProvision
 * @brief Generator contribution to inertia zone(s)
 *
 * Links a @ref Generator to one or more @ref InertiaZone entries and
 * defines the provision parameters.
 */
struct InertiaProvision
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  SingleId generator {unknown_uid};  ///< FK to the providing generator
  String inertia_zones {};  ///< Colon-separated list of InertiaZone IDs/names

  OptReal inertia_constant {};  ///< Machine inertia constant H [seconds]
  OptReal rated_power {};  ///< Rated apparent power S [MVA]

  OptTBRealFieldSched provision_max {};  ///< Max inertia provision [MW]
  OptTRealFieldSched provision_factor {};  ///< Effectiveness factor FE [MWs/MW]
  OptTRealFieldSched cost {};  ///< Provision cost [$/MW]
};

}  // namespace gtopt
