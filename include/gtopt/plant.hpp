/**
 * @file      plant.hpp
 * @brief     Declarative grouping of generator configuration variants
 * @date      Sat May 30 12:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A `Plant` groups two or more `Generator` configuration variants of the
 * same physical machine (e.g. combined-cycle plant with `_ConfTGA`,
 * `_ConfTGB`, `_ConfTV` configs, or a fuel-band family with `_GN_A`,
 * `_GN_B`, `_DIE`).  Emits up to three native LP rows per stage:
 *
 *   1. ``plant_cap_<name>``    — Σ generation ≤ pmax  (per stage × block)
 *   2. ``plant_commit_<name>`` — Σ commit_coeff·status ≤ n_units (per stage)
 *   3. ``plant_uniq_<name>``   — Σ status ≤ 1         (per stage)
 *
 * Replaces the synthesized ``PlantCap_<stem>`` UserConstraints emitted
 * by `plexos2gtopt` and the per-plant ``<plant>_Uniq`` MIP exclusion
 * UCs.  Native enforcement lets the LP avoid the per-block UC overhead
 * and gives gtopt's solver the cleanest formulation.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "ATA_CC_1",
 *   "generator_names": ["ATA_CC_1_ConfTGA",
 *                       "ATA_CC_1_ConfTGB",
 *                       "ATA_CC_1_ConfTV"],
 *   "pmax": 391.0,
 *   "n_units": 1,
 *   "uniq_mutex": true
 * }
 * ```
 *
 * @see PlantLP for the LP build path
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Declarative plant primitive: groups generator configuration
 *        variants and emits Σ-capacity / Σ-commit caps natively.
 *
 * Designed as a passive descriptor — the LP wrapper (`PlantLP`) reads
 * the field set at `add_to_lp` time, resolves the named generators to
 * their already-built `generation_cols` / commitment `status_cols`, and
 * appends the cap / commit / uniq rows directly.  No new LP columns
 * are introduced.
 */
struct Plant
{
  static constexpr LPClassName class_name {"Plant"};

  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  /// Member generators (configuration variants of one physical plant).
  /// At least two entries are required for the LP to emit any row —
  /// a one-member group is a degenerate case `GeneratorLP` already
  /// caps via its own pmax.
  Array<Name> generator_names {};

  /// Total dispatchable capacity envelope of the physical plant.
  /// When set, the LP emits one ``plant_cap_<name>`` row per stage
  /// per block: ``Σ generation ≤ pmax``.  Empty → row not emitted.
  OptReal pmax {};

  /// Number of physical units the plant can commit.  When set, the
  /// LP emits one ``plant_commit_<name>`` row per stage:
  /// ``Σ commit_coeffs[i] · status[i] ≤ n_units``.  Empty → row
  /// not emitted (typical when uniq_mutex covers the constraint).
  OptInt n_units {};

  /// Per-variant commitment coefficients (same length as
  /// ``generator_names``).  Empty defaults to ``1.0`` per variant —
  /// the common case where every configuration consumes one unit of
  /// commit budget.
  Array<Real> commit_coeffs {};

  /// When true, the LP emits one ``plant_uniq_<name>`` row per stage:
  /// ``Σ status[i] ≤ 1``.  Models the PLEXOS-style "at most one
  /// configuration active" mutex (combined-cycle plants where TGA /
  /// TGB / TV variants of the same machine cannot run in parallel).
  /// Defaults to off (``std::nullopt`` / ``false``) — opt in by
  /// setting ``true`` per Plant entry.
  OptBool uniq_mutex {};
};

}  // namespace gtopt
