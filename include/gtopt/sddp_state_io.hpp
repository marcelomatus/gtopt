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
#include <gtopt/iteration.hpp>

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
/// across LP structure changes.
///
/// @param planning_lp  The PlanningLP (for LP access and scaling)
/// @param filepath     Output CSV file path
/// @param iteration    Current iteration index (written as comment)
[[nodiscard]] auto save_state_csv(const PlanningLP& planning_lp,
                                  const std::string& filepath,
                                  IterationIndex iteration)
    -> std::expected<void, Error>;

/// Load state variable column solutions from a CSV file.
///
/// Reads the CSV and builds a warm column solution vector per (scene, phase)
/// LinearInterface.  After loading, physical_eini/physical_efin will use
/// these warm values as fallback when the LP has not been solved yet.
///
/// @param planning_lp  The PlanningLP to inject warm solutions into
/// @param filepath     Input CSV file path
[[nodiscard]] auto load_state_csv(PlanningLP& planning_lp,
                                  const std::string& filepath)
    -> std::expected<void, Error>;

}  // namespace gtopt
