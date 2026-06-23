/**
 * @file      future_cost.hpp
 * @brief     Future Cost Function (FCF) element — the cost-to-go / α
 * @date      Sat Jun 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the FutureCost structure — a first-class element that owns the
 * end-of-horizon **cost-to-go** (the FCF, α / `varphi_s`) and the boundary
 * cuts that linearise it.  It replaces the dispersed boundary-cut/α
 * plumbing previously scattered across the SDDP units and read piecemeal
 * by `monolithic_method` and `sddp_method`:
 *
 *   - the boundary cuts (`boundary_cuts.csv`: per-scene `rhs` = FCF
 *     intercept + per-reservoir `-water_value` slopes on the terminal
 *     storage state variables),
 *   - the α / `varphi_s` decision variable(s) (one per source scene under
 *     multicut, priced `1/N`), whose optimal value is the realised
 *     cost-to-go,
 *   - the α-rebase (`mean_shift`): shifting the cut intercepts so α' ≈ 0
 *     when the reservoirs hit their `efin` end-of-horizon targets, with
 *     the removed constant added back as an objective constant `c̄`.  When
 *     more water than target is saved, α' < 0 (a future-cost credit); when
 *     less, α' > 0.
 *
 * Both `monolithic_method` and `sddp_method` build/output this element
 * through the standard `FutureCostLP::add_to_lp` / `add_to_output`
 * lifecycle, so the FCF concept lives in one method-neutral place and its
 * α, the rebase constant `c̄`, and the un-rebased value `α + c̄` are saved
 * to the solution like any other element's outputs.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "fcf",
 *   "cuts_file": "boundary_cuts.csv",
 *   "scale_alpha": 100000,
 *   "mean_shift": true,
 *   "sharing": "multicut"
 * }
 * ```
 */

#pragma once

#include <optional>

#include <gtopt/basic_types.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/sddp_enums.hpp>

namespace gtopt
{

/**
 * @brief The future cost function (cost-to-go) as a first-class element.
 *
 * @see FutureCostLP for the LP build / output path.
 */
struct FutureCost
{
  static constexpr LPClassName class_name {"FutureCost"};

  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};
  OptName description {};  ///< Optional free-text provenance.

  /// Boundary-cut file (relative to the bundle) that linearises the FCF:
  /// rows `scene,rhs,<res...>` with `rhs` the FCF intercept and each
  /// reservoir coefficient `-water_value` ($/CMD) on that reservoir's
  /// terminal-volume state variable.  Was `simulation.boundary_cuts_file`.
  OptName cuts_file {};

  /// Numerical scale applied to the α / `varphi_s` columns (the LP
  /// conditioning factor on the cost-to-go variable).  When unset the LP
  /// auto-derives it from the cut coefficients
  /// (`boundary_cut_max_avg_coeff`); an explicit value overrides.  Was the
  /// dispersed `scale_alpha` option threaded through every method.
  OptReal scale_alpha {};

  /// Rebase the α at the reservoirs' `efin` end-of-horizon targets so α'
  /// is centred on ~0 when the targets are met (α' < 0 when more water is
  /// saved, > 0 when less).  Default `true`.  The removed constant is the
  /// per-scene `c̄` added back as an objective constant.  Was
  /// `sddp_options.boundary_cuts_mean_shift`.
  OptBool mean_shift {};

  /// When a SINGLE boundary cut is loaded for a scene, install it as the
  /// equality `α + Σ wvᵣ·efinᵣ = FCF` with α freed (`set_col_low_raw(-inf)`)
  /// — the continuous PLEXOS-style terminal value: α is pinned to
  /// `FCF − Σ wvᵣ·efinᵣ` so every reservoir's terminal storage is priced at
  /// `wvᵣ` on BOTH sides of the target.  Default (unset) = `true` = the
  /// current behaviour.  Set `false` to keep the slack-able `≥` Benders
  /// lower-bound form (α floored at the cut, never freed) even for a single
  /// cut — e.g. for a faithful comparison against a user-authored `≥` FCF
  /// cut.  With MANY cuts the `≥` max-envelope form is ALWAYS used regardless
  /// of this flag (the value function is the max of its supporting cuts).
  OptBool single_cut_equality {};

  /// Cross-scene cut sharing: `per_scene` (each scene keeps its own cut),
  /// `shared` (broadcast every cut onto every scene's terminal α), or
  /// `multicut` (N dedicated `varphi_s`, one per source scene, priced
  /// `1/N`).  Typed enum (no magic strings).  Was
  /// `sddp_options.boundary_cut_sharing`.
  std::optional<BoundaryCutSharingMode> sharing {};

  /// Boundary-cut load mode: `noload`, `separated` (per-scene, default), or
  /// `combined` (broadcast all cuts to all scenes).  Typed enum.  Was
  /// `sddp_options.boundary_cuts_mode`.
  std::optional<BoundaryCutsMode> mode {};

  /// Terminal-state water-value valuation statistic — `min`, `avg`, or `max`
  /// of each cut column's coefficients, used as the terminal soft cost on the
  /// named state variables.  Typed enum.  Was
  /// `simulation.boundary_cuts_valuation`.
  std::optional<BoundaryCutSoftCost> valuation {};

  /// User-overridable FCF (piece 5 step 2a).  When `true`, the FCF cost-to-go
  /// α is NOT the built-in `varphi_s` column priced `1/N`; instead the modeller
  /// authors a global `state`/`link` `DecisionVariable` (the user α, with
  /// `cost = 1.0`, `cost_type = "raw"`) plus global `UserConstraint` cut row(s)
  /// over it, and points `user_alpha_uid` at that variable.  The built-in α
  /// column is then registered INERT (pinned `lowb = uppb = 0`, `cost = 0`) so
  /// it neither prices nor floors anything — the user's recourse fully replaces
  /// the boundary-cut FCF.  Mutually exclusive with `cuts_file`
  /// (`boundary_cuts_file`): supplying both is a runtime error.  Default unset
  /// ⇒ `false` (the byte-for-byte legacy boundary-cut path).
  OptBool use_user_alpha {};

  /// UID of the user-authored α `DecisionVariable` (piece 5 step 2a).  Required
  /// when `use_user_alpha` is `true`: identifies the global `state`/`link`
  /// column that holds the cost-to-go so `add_to_output` can emit it as
  /// `FutureCost/alpha` and the forward-pass UB estimator can subtract its
  /// realised value.  Typed `Uid` (not a name) so it resolves through the
  /// state-variable registry without a string lookup.
  OptUid user_alpha_uid {};
};

}  // namespace gtopt
