/**
 * @file      lp_context.hpp
 * @brief     Typed LP context tuples and label generation utilities
 * @date      Sun Apr  6 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines typed context tuples that identify WHERE in the LP hierarchy
 * a column or row lives.  Each tuple alias names a specific granularity
 * level (stage-level, block-level, etc.).
 *
 * These tuples replace the fragile positional string arguments previously
 * passed to lp_col_label() / lp_row_label().  String labels are now
 * generated lazily from structured metadata when needed (LP file output,
 * solver diagnostics).
 *
 * Convention: tuples are ordered from broadest to narrowest scope.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <variant>

#include <gtopt/aperture.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

// -- Context tuple aliases ---------------------------------------------------
//
// Each alias names a level in the LP hierarchy.  Columns and rows carry
// one of these as their `.context` field, encoding the exact dimensional
// coordinates without string formatting.
//
// Uses strong UID types (ScenarioUid, StageUid, BlockUid) for type safety.
// Each tuple alias is a distinct type, preventing overload ambiguity.

/// Stage-level context: state variables (eini, sini, efin),
/// demand fail columns, capacity variables.
/// Tuple: (scenario_uid, stage_uid)
using StageContext = std::tuple<ScenarioUid, StageUid>;

/// Block-level context: dispatch variables (generation, load, flow,
/// theta, extraction) -- one column per block.
/// Tuple: (scenario_uid, stage_uid, block_uid)
using BlockContext = std::tuple<ScenarioUid, StageUid, BlockUid>;

/// Block-level context with an extra index (e.g. segment number).
/// Tuple: (scenario_uid, stage_uid, block_uid, extra_index)
using BlockExContext = std::tuple<ScenarioUid, StageUid, BlockUid, int>;

// -- SDDP context types ------------------------------------------------------
//
// SDDP operates on a (scene, phase, iteration) hierarchy, distinct from
// the LP (scenario, stage, block) hierarchy.

/// SDDP phase-level context: alpha columns, shared cuts.
/// Tuple: (scene_uid, phase_uid)
using ScenePhaseContext = std::tuple<SceneUid, PhaseUid>;

/// SDDP cut context with iteration UID and extra index (cut offset).
/// Tuple: (scene_uid, phase_uid, iteration_uid, extra).
/// All three positional identifiers are UIDs (1-based, matching the
/// `PhaseUid` / `SceneUid` convention).  Convert to/from the 0-based
/// runtime `IterationIndex` via `uid_of(idx)` / `index_of(uid)`.
using IterationContext = std::tuple<SceneUid, PhaseUid, IterationUid, int>;

/// SDDP aperture cut context.
/// Tuple: (scene_uid, phase_uid, aperture_uid, extra)
using ApertureContext = std::tuple<SceneUid, PhaseUid, ApertureUid, int>;

/// Type-erased context for internal storage (LinearProblem, FlatLinearProblem).
/// std::monostate = no context (legacy or context-free columns).
using LpContext = std::variant<std::monostate,
                               StageContext,
                               BlockContext,
                               BlockExContext,
                               ScenePhaseContext,
                               IterationContext,
                               ApertureContext>;

// -- Context factory functions ------------------------------------------------
//
// Accept the proper strong UID types to prevent accidentally swapping
// scenario/stage/block UIDs.

/// Build a stage-level context from strongly-typed UIDs.
[[nodiscard]] constexpr auto make_stage_context(ScenarioUid scenario_uid,
                                                StageUid stage_uid)
    -> StageContext
{
  return StageContext {scenario_uid, stage_uid};
}

/// Build a block-level context from strongly-typed UIDs.
[[nodiscard]] constexpr auto make_block_context(ScenarioUid scenario_uid,
                                                StageUid stage_uid,
                                                BlockUid block_uid)
    -> BlockContext
{
  return BlockContext {scenario_uid, stage_uid, block_uid};
}

/// Build a block-level context with an extra index (e.g. segment number).
[[nodiscard]] constexpr auto make_block_context(ScenarioUid scenario_uid,
                                                StageUid stage_uid,
                                                BlockUid block_uid,
                                                int extra) -> BlockExContext
{
  return BlockExContext {scenario_uid, stage_uid, block_uid, extra};
}

/// Build an SDDP scene/phase context.
[[nodiscard]] constexpr auto make_scene_phase_context(SceneUid scene_uid,
                                                      PhaseUid phase_uid)
    -> ScenePhaseContext
{
  return ScenePhaseContext {scene_uid, phase_uid};
}

/// Build an SDDP scene/phase context from an already-converted
/// `IterationUid`.  The primary overload — keeps call sites
/// visually homogeneous (three UIDs + one extra).
///
/// All three UIDs must be REAL identifiers (not the
/// ``unknown_uid`` sentinel).  Passing ``unknown_uid`` for any of
/// them produces an unlabelable LP-row context that
/// ``LinearInterface::generate_labels_from_maps`` cannot
/// disambiguate from other ``unknown_uid``-tagged rows, ultimately
/// triggering a duplicate-label collision or the
/// "metadata without a class_name (unlabelable)" error.  Caught
/// here at construction time by throwing ``std::invalid_argument``
/// so the bad call site surfaces directly instead of in the (much
/// later) label generation pass.
[[nodiscard]] inline auto make_iteration_context(SceneUid scene_uid,
                                                 PhaseUid phase_uid,
                                                 IterationUid iteration_uid,
                                                 int extra) -> IterationContext
{
  if (scene_uid.is_unknown()) {
    throw std::invalid_argument(
        "make_iteration_context: scene_uid must not be unknown_uid");
  }
  if (phase_uid.is_unknown()) {
    throw std::invalid_argument(
        "make_iteration_context: phase_uid must not be unknown_uid");
  }
  if (iteration_uid.is_unknown()) {
    throw std::invalid_argument(
        "make_iteration_context: iteration_uid must not be unknown_uid");
  }
  return IterationContext {scene_uid, phase_uid, iteration_uid, extra};
}

// (The IterationIndex convenience overload was removed: every call
// site must pass a real ``IterationUid``.  Convert at the call site
// via ``uid_of(iteration_index)`` (or ``sim.uid_of(...)``) — keeps
// the type discipline visible and prevents accidental
// ``IterationIndex`` → ``int`` narrowing in the ``extra`` slot.)

/// Build an SDDP aperture cut context.
[[nodiscard]] constexpr auto make_aperture_context(SceneUid scene_uid,
                                                   PhaseUid phase_uid,
                                                   ApertureUid aperture,
                                                   int extra) -> ApertureContext
{
  return ApertureContext {scene_uid, phase_uid, aperture, extra};
}

// -- Tuple hashing -----------------------------------------------------------
//
// std::tuple does not have a std::hash specialization.  We provide a
// generic hasher for any tuple of hashable elements (boost::hash_combine).

namespace detail
{

constexpr size_t hash_combine(size_t seed, size_t h) noexcept
{
  return seed ^ (h + size_t {0x9e3779b9} + (seed << 6U) + (seed >> 2U));
}

template<typename Tuple, size_t... Is>
constexpr size_t tuple_hash_impl(const Tuple& t,
                                 std::index_sequence<Is...> /*unused*/) noexcept
{
  size_t seed = 0;
  ((seed = hash_combine(
        seed, std::hash<std::tuple_element_t<Is, Tuple>> {}(std::get<Is>(t)))),
   ...);
  return seed;
}

}  // namespace detail

/// Generic hash for any std::tuple of hashable elements.
struct TupleHash
{
  template<typename... Ts>
  constexpr size_t operator()(const std::tuple<Ts...>& t) const noexcept
  {
    return detail::tuple_hash_impl(t, std::index_sequence_for<Ts...> {});
  }
};

// -- Label generation --------------------------------------------------------
//
// Generates LP column/row name strings from structured metadata.
// Called lazily during flatten() or diagnostics -- NOT during add_col().
//
// Historical note: this header previously provided `generate_lp_label`
// overloads for building LP row/column labels inline.  That mechanism has
// been superseded by `gtopt::LabelMaker` (see <gtopt/label_maker.hpp>),
// which consumes `SparseCol` / `SparseRow` directly, honors `LpNamesLevel`,
// and is the single place in gtopt that formats LP labels.

}  // namespace gtopt
