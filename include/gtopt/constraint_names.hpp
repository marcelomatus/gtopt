/**
 * @file      constraint_names.hpp
 * @brief     Canonical constexpr name constants used in LP row labels.
 * @copyright BSD-3-Clause
 *
 * Centralises the `class_name` and `constraint_name` string_view values
 * used by the structural-build LP layers so callers reference one
 * canonical constant per row type instead of duplicating string
 * literals at use sites.
 *
 *   * Per-element-class names (e.g. `"Bus"`, `"Reservoir"`,
 *     `"LngTerminal"`) live on each data struct as
 *     `static constexpr LPClassName class_name {"…"}`.  Reach them via
 *     `Bus::class_name.full_name()` etc. (or
 *     `BusLP::Element::class_name.full_name()` in generic contexts) —
 *     single source of truth on the data struct; not duplicated here.
 *
 *   * Aggregator / coordinator names (`"System"`, `"Cascade"`, `"Sddp"`,
 *     `"Kirchhoff"`) describe LP rows that are NOT owned by a single
 *     `*LP` element class.  They are defined as `std::string_view`
 *     constants below.
 *
 *   * Constraint-type names (`"flow_link"`, `"emission_cap"`, …)
 *     classify rows by purpose within an element class and are also
 *     defined below.
 *
 * Cut-related names (`sddp_*_constraint_name`, `sddp_*_class_name`)
 * live in `gtopt/sddp_types.hpp` next to their `CutTag` definitions.
 *
 * The values here are the same strings that `extract_row_type` in
 * `linear_problem.cpp` derives from row labels for the per-row-type
 * stats breakdown — adding a new type means adding a constant here AND
 * ensuring the row name still parses through `extract_row_type`
 * cleanly (`<class>_<constraint>_<…>`).
 */

#pragma once

#include <string_view>

namespace gtopt
{

// ─── class_name constants (aggregator / coordinator rows) ───────────────────
//
// Per-element class names (`Bus::class_name`, `Reservoir::class_name`,
// `LngTerminal::class_name`, …) live on each data struct as a
// `static constexpr LPClassName class_name`.  Reach them via
// `Bus::class_name` etc. directly — single source of truth.  The LP
// wrappers (`BusLP`, …) expose no separate `ClassName` member; generic
// code reaches the constant via `BusLP::Element::class_name`.
//
// The constants below describe LP rows that are NOT owned by a single
// data struct (system-wide, cascade coordinator, SDDP coordinator,
// Kirchhoff cycles).

/// Top-level system-wide constraint rows (e.g. emission cap).
inline constexpr std::string_view system_class_name = "System";

/// Cascade-method coordinator rows (state-target equality with elastic
/// slack).
inline constexpr std::string_view cascade_class_name = "Cascade";

/// SDDP-method coordinator rows / columns (e.g. α future-cost
/// variables).  Used in row labels where no per-element data-struct
/// `class_name` applies.
inline constexpr std::string_view sddp_class_name = "Sddp";

/// Kirchhoff Voltage Law cycle rows on a meshed network.
inline constexpr std::string_view kirchhoff_class_name = "Kirchhoff";

// ─── constraint_name constants (within-class row classification) ────────────

/// Flow conservation constraint on a node / waterway / line.
inline constexpr std::string_view flow_link_constraint_name = "flow_link";

/// Linearised line-loss constraint (segments approximating |P|² · R).
inline constexpr std::string_view loss_link_constraint_name = "loss_link";

/// Kirchhoff Voltage Law cycle constraint (Σ X·P = 0 around a loop).
inline constexpr std::string_view kirchhoff_cycle_constraint_name = "cycle";

/// Cascade-target / state-link constraint.
inline constexpr std::string_view cascade_target_constraint_name = "target";

/// Per-stage emission cap (Σ generation × emission_factor ≤ cap).
inline constexpr std::string_view emission_cap_constraint_name = "emission_cap";

/// Storage / reservoir end-of-horizon closure constraint.
/// Renamed from the legacy `"eclose"` for clarity.
inline constexpr std::string_view storage_close_constraint_name =
    "storage_close";

/// Elastic-filter fix constraint (added by `relax_fixed_state_variable`
/// in `benders_cut.cpp` to relax a fixed state variable into a slack
/// pair when the master phase is infeasible at the trial state).
inline constexpr std::string_view elastic_fix_constraint_name = "elastic_fix";

}  // namespace gtopt
