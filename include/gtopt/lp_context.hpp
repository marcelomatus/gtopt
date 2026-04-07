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
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include <gtopt/aperture.hpp>
#include <gtopt/as_label.hpp>
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

/// SDDP cut context with iteration and extra index (cut offset).
/// Tuple: (scene_uid, phase_uid, iteration_index, extra)
using IterationContext = std::tuple<SceneUid, PhaseUid, IterationIndex, int>;

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
[[nodiscard]] constexpr auto make_scene_phase_context(SceneUid scene,
                                                      PhaseUid phase)
    -> ScenePhaseContext
{
  return ScenePhaseContext {scene, phase};
}

/// Build an SDDP scene/phase context with iteration and extra index.
[[nodiscard]] constexpr auto make_iteration_context(SceneUid scene,
                                                    PhaseUid phase,
                                                    IterationIndex iteration,
                                                    int extra)
    -> IterationContext
{
  return IterationContext {scene, phase, iteration, extra};
}

/// Build an SDDP aperture cut context.
[[nodiscard]] constexpr auto make_aperture_context(SceneUid scene,
                                                   PhaseUid phase,
                                                   ApertureUid aperture,
                                                   int extra) -> ApertureContext
{
  return ApertureContext {scene, phase, aperture, extra};
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
// Label format: "{class_short}_{variable}_{uid}_{ctx0}_{ctx1}_..."
// Example:      "rsv_eini_1_0_1"  (class=rsv, var=eini, uid=1, scen=0, stg=1)

/// Generate a column/row label from structured identity + context.
/// @param class_name   Class name (e.g. "Reservoir", "Bus", "Generator")
/// @param variable     Variable name (e.g. "eini", "theta", "fext")
/// @param uid          Element UID
/// @param context      Context tuple (e.g. {scenario_uid, stage_uid,
/// block_uid})
/// @return             Formatted label string (e.g. "reservoir_eini_1_0_1_2")
template<typename Tuple, size_t... Is>
[[nodiscard]] auto generate_lp_label_impl(std::string_view class_name,
                                          std::string_view variable,
                                          Uid uid,
                                          const Tuple& context,
                                          std::index_sequence<Is...> /*unused*/)
    -> std::string
{
  return as_label(
      lowercase(class_name), variable, uid, std::get<Is>(context)...);
}

template<typename... ContextUids>
[[nodiscard]] auto generate_lp_label(std::string_view class_name,
                                     std::string_view variable,
                                     Uid uid,
                                     const std::tuple<ContextUids...>& context)
    -> std::string
{
  return generate_lp_label_impl(class_name,
                                variable,
                                uid,
                                context,
                                std::index_sequence_for<ContextUids...> {});
}

/// Generate a label without context (for context-free columns).
[[nodiscard]] inline auto generate_lp_label(std::string_view class_name,
                                            std::string_view variable,
                                            Uid uid) -> std::string
{
  return as_label(lowercase(class_name), variable, uid);
}

/// Generate a label from a type-erased LpContext variant.
/// Returns empty string when class_name is empty or context is monostate.
[[nodiscard]] inline auto generate_lp_label(std::string_view class_name,
                                            std::string_view variable,
                                            Uid uid,
                                            const LpContext& context)
    -> std::string
{
  if (class_name.empty()) {
    return {};
  }
  return std::visit(
      [&](const auto& ctx) -> std::string
      {
        if constexpr (std::same_as<std::decay_t<decltype(ctx)>,
                                   std::monostate>) {
          return generate_lp_label(class_name, variable, uid);
        } else {
          return generate_lp_label(class_name, variable, uid, ctx);
        }
      },
      context);
}

}  // namespace gtopt
