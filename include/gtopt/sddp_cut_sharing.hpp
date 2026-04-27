/**
 * @file      sddp_cut_sharing.hpp
 * @brief     SDDP cut sharing across scenes
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements three cut sharing modes for SDDP multi-scene problems:
 *   - accumulate: sum all scene cuts into one accumulated cut
 *   - expected:   probability-weighted average across scenes
 *   - max:        broadcast all cuts from all scenes to all scenes
 */

#pragma once

#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{

class PlanningLP;

/// Share optimality cuts across scenes for a single phase.
///
/// @param phase_index      Phase index where cuts will be added
/// @param scene_cuts       Per-scene optimality cuts for this phase
/// @param mode             Cut sharing mode (none/accumulate/expected/max)
/// @param planning         PlanningLP reference (for LP access)
/// @param iteration_index  Current SDDP iteration; combined with the
///                         per-scene UID and ``phase_index`` to build a
///                         unique LP-row label context for every cut
///                         landing in each destination scene's LP
///                         (avoids the
///                         "metadata without a class_name" labeller
///                         throw and the duplicate-label collision when
///                         the same accumulated cut is replicated to
///                         multiple scenes).  Defaults to
///                         ``IterationIndex{0}`` for callers that
///                         legitimately treat a single share-call as
///                         iteration 0 (e.g. unit tests that exercise
///                         a single iteration).
void share_cuts_for_phase(
    PhaseIndex phase_index,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    CutSharingMode mode,
    PlanningLP& planning,
    IterationIndex iteration_index = IterationIndex {0});

}  // namespace gtopt
