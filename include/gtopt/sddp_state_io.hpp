/**
 * @file      sddp_state_io.hpp
 * @brief     State variable column I/O for SDDP solver
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Save/load state variable column solutions and reduced costs.
 * Extracted from sddp_cut_io.hpp to improve modularity.
 */

#pragma once

#include <expected>
#include <string>

#include <gtopt/error.hpp>

namespace gtopt
{

// Forward declarations
class PlanningLP;

// ─── State variable column I/O ──────────────────────────────────────────────

/// Save state variable column solutions and reduced costs to a CSV file.
///
/// Writes one row per state-variable column (efin, eini, sini, etc.) with
/// name, phase UID, scene UID, LP value (in physical units), and reduced
/// cost.  The file is self-describing via column names, so it is portable
/// across LP structure changes.  The file is overwritten each iteration;
/// the producing iteration is identified by the surrounding log line at
/// `sddp_cut_store.cpp` and the on-disk mtime, not by an in-file marker.
///
/// @param planning_lp  The PlanningLP (for LP access and scaling)
/// @param filepath     Output CSV file path
[[nodiscard]] auto save_state_csv(PlanningLP& planning_lp,
                                  const std::string& filepath)
    -> std::expected<void, Error>;

}  // namespace gtopt
