/**
 * @file      iteration.hpp
 * @brief     Defines the Iteration structure for per-iteration solver control
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * An Iteration allows per-iteration control of SDDP solver behaviour.
 * When an `iteration_array` is present in the Simulation JSON, the solver
 * looks up the current iteration index and honours per-iteration flags
 * such as `update_lp` (whether to dispatch LP coefficient updates).
 *
 * ### JSON Example
 * ```json
 * {"index": 0, "update_lp": false}
 * {"index": 5, "update_lp": true}
 * ```
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/uid.hpp>

namespace gtopt
{

/**
 * @struct Iteration
 * @brief Per-iteration control flags for the SDDP solver
 *
 * Unlike Phase and Scenario, Iteration is keyed by `Index index` rather
 * than a Uid.  The solver matches the current 0-based iteration number
 * against this index to find per-iteration overrides.
 */
struct Iteration
{
  /// 0-based iteration index used as lookup key
  Index index {0};

  /// Whether to dispatch update_lp for this iteration.
  /// When absent (nullopt), update_lp is dispatched (default = true).
  OptBool update_lp {};

  /// Class name constant used for serialisation/deserialisation
  static constexpr std::string_view class_name = "iteration";

  /// @return Whether update_lp should be dispatched for this iteration
  [[nodiscard]] constexpr auto should_update_lp() const noexcept
  {
    return update_lp.value_or(true);
  }
};

/// Tag type for SDDP iteration numbering
struct IterationTag;

/// Strongly-typed index for SDDP iterations.  0-based, matches the
/// pattern used by `SceneIndex` / `PhaseIndex` for array subscripts
/// and loop counters.
using IterationIndex = StrongIndexType<IterationTag>;

/// Strongly-typed UID for SDDP iterations.  1-based, matches the
/// `PhaseUid` / `SceneUid` convention used in LP-label context
/// tuples: UIDs appear in `ScenePhaseContext` / `IterationContext`,
/// indices appear in runtime control flow.  The two are convertible
/// via `uid_of(index)` / `index_of(uid)` (uid = index + 1).
using IterationUid = UidOf<Iteration>;

/// @brief Convert a 0-based `IterationIndex` to the matching 1-based
///        `IterationUid`.  The only sanctioned way to produce an
///        `IterationUid` from runtime iteration state, so the +1
///        convention stays in one place.
[[nodiscard]] constexpr auto uid_of(IterationIndex idx) noexcept -> IterationUid
{
  return make_uid<Iteration>(static_cast<uid_t>(idx) + 1);
}

/// @brief Convert a 1-based `IterationUid` back to the matching
///        0-based `IterationIndex`.
[[nodiscard]] constexpr auto index_of(IterationUid uid) noexcept
    -> IterationIndex
{
  return IterationIndex {value_of(uid) - 1};
}

/// @brief Next iteration index (iteration_index + 1), preserving strong type.
[[nodiscard]] constexpr auto next(IterationIndex iteration_index) noexcept
    -> IterationIndex
{
  return ++iteration_index;
}

/// @brief Advance an iteration index by `n` steps, returning a new
///        `IterationIndex`.  Replaces the
///        `IterationIndex{static_cast<Index>(offset) + n}` pattern used
///        to compute an iteration budget horizon.
///
/// Example: `next(m_iteration_offset_, m_options_.max_iterations)`
/// returns the exclusive upper bound of a training run that starts
/// at `m_iteration_offset_` and takes `max_iterations` iterations.
[[nodiscard]] constexpr auto next(IterationIndex iteration_index,
                                  Index n) noexcept -> IterationIndex
{
  // Underlying strong::arithmetic supports `+ Index`, preserving the
  // strong type without any static_cast at the call site.
  return iteration_index + IterationIndex {n};
}

/// @brief Previous iteration index (iteration_index - 1), preserving strong
/// type.
[[nodiscard]] constexpr auto previous(IterationIndex iteration_index) noexcept
    -> IterationIndex
{
  return --iteration_index;
}

/// @brief Signed distance of `cur` from a base `offset`, as a plain
///        `Index` offset (not wrapped in `IterationIndex` — because the
///        difference of two positional indices is not itself a
///        positional index, it's a count).
///
/// Replaces the `iteration_index - m_iteration_offset_` idiom sprinkled
/// across SDDP iteration management (relative-iteration logging,
/// `min_iter` clamping, `max_async_spread` checks).  Having a single
/// helper means future tweaks (e.g. bounds-checking on negative
/// differences) land in one place.
[[nodiscard]] constexpr auto iteration_relative(IterationIndex cur,
                                                IterationIndex offset) noexcept
    -> Index
{
  return static_cast<Index>(cur) - static_cast<Index>(offset);
}

}  // namespace gtopt
