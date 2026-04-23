/**
 * @file      sddp_scene_tracker.hpp
 * @brief     Per-scene iteration tracker for asynchronous SDDP execution
 * @date      2026-04-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tracks per-scene iteration progress, bounds history, and completion
 * counts for the asynchronous SDDP execution mode.  All methods are
 * called from the main orchestration thread only — no locking needed.
 */

#pragma once

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/// Per-scene bounds snapshot for one completed iteration.
struct SceneBounds
{
  double upper_bound {};
  double lower_bound {};
  bool feasible {true};
  IterationIndex iteration_index {};
};

/// Tracks per-scene iteration progress for async SDDP execution.
///
/// All methods are called from the main thread only (the one running
/// the state-machine orchestration loop).  Pool tasks write results
/// into their futures; the main thread collects them and updates the
/// tracker.  No thread safety is required.
class SceneIterationTracker
{
public:
  SceneIterationTracker() = default;

  explicit SceneIterationTracker(Index num_scenes, int max_spread)
      : m_history_(static_cast<std::size_t>(num_scenes))
      , m_scene_completed_iter_(static_cast<std::size_t>(num_scenes),
                                IterationIndex {-1})
      , m_scene_converged_(static_cast<std::size_t>(num_scenes), false)
      , m_scene_converged_iter_(static_cast<std::size_t>(num_scenes),
                                IterationIndex {-1})
      , m_max_spread_(max_spread)
  {
  }

  /// Record that @p scene completed iteration @p iteration_index with
  /// the given bounds.  Maintains a ring buffer of depth max_spread + 1
  /// per scene.
  void report_complete(SceneIndex scene_index,
                       IterationIndex iteration_index,
                       double ub,
                       double lb,
                       bool feasible)
  {
    m_history_[scene_index].push_back(SceneBounds {
        .upper_bound = ub,
        .lower_bound = lb,
        .feasible = feasible,
        .iteration_index = iteration_index,
    });
    // Keep ring buffer bounded
    const auto max_depth = static_cast<std::size_t>(m_max_spread_) + 2;
    while (m_history_[scene_index].size() > max_depth) {
      m_history_[scene_index].pop_front();
    }
    m_scene_completed_iter_[scene_index] = iteration_index;
    m_completion_counts_[iteration_index]++;
  }

  /// True when ALL scenes have completed at least iteration
  /// @p iteration_index.
  [[nodiscard]] bool all_complete(IterationIndex iteration_index) const
  {
    auto it = m_completion_counts_.find(iteration_index);
    return it != m_completion_counts_.end()
        && std::cmp_greater_equal(it->second, m_scene_completed_iter_.size());
  }

  /// Get per-scene bounds for iteration @p iteration_index.
  /// Requires `all_complete(iteration_index)` to return true.
  [[nodiscard]] auto bounds_for_iteration(IterationIndex iteration_index) const
      -> std::vector<SceneBounds>
  {
    std::vector<SceneBounds> result;
    result.reserve(m_history_.size());
    for (const auto& hist : m_history_) {
      auto it = std::ranges::find_if(
          hist,
          [&](const auto& e) { return e.iteration_index == iteration_index; });
      if (it != hist.end()) {
        result.push_back(*it);
      } else {
        // Should not happen when all_complete(iteration_index) is true
        result.push_back(SceneBounds {
            .iteration_index = iteration_index,
        });
      }
    }
    return result;
  }

  /// Minimum completed iteration across all scenes.
  /// Returns IterationIndex{-1} if no scene has completed any iteration.
  [[nodiscard]] auto min_completed_iteration() const -> IterationIndex
  {
    return *std::ranges::min_element(m_scene_completed_iter_);
  }

  /// Maximum completed iteration across all scenes.
  [[nodiscard]] auto max_completed_iteration() const -> IterationIndex
  {
    return *std::ranges::max_element(m_scene_completed_iter_);
  }

  /// Snapshot of per-scene completed iteration indices, as raw int for
  /// the JSON serialisation boundary (`SolverStatusSnapshot`,
  /// `SDDPIterationResult`).  The unwrap here is legitimate — downstream
  /// consumers only print/JSON-serialise these values and never use them
  /// as `IterationIndex` identities.
  [[nodiscard]] auto scene_iterations() const -> std::vector<int>
  {
    std::vector<int> result;
    result.reserve(m_scene_completed_iter_.size());
    for (const auto& iteration_index : m_scene_completed_iter_) {
      result.push_back(static_cast<int>(iteration_index));
    }
    return result;
  }

  /// Mark a scene as individually converged at the given iteration.
  void mark_converged(SceneIndex scene_index, IterationIndex iteration_index)
  {
    m_scene_converged_[scene_index] = true;
    m_scene_converged_iter_[scene_index] = iteration_index;
  }

  /// True if a specific scene has individually converged.
  [[nodiscard]] bool is_converged(SceneIndex scene_index) const
  {
    return m_scene_converged_[scene_index];
  }

  /// True when ALL scenes have individually converged.
  [[nodiscard]] bool all_converged() const
  {
    return std::ranges::all_of(m_scene_converged_, std::identity {});
  }

  /// Number of scenes that have individually converged.
  [[nodiscard]] auto num_converged() const -> Index
  {
    return static_cast<Index>(std::ranges::count(m_scene_converged_, true));
  }

  /// Iteration at which a scene converged (-1 if not yet converged).
  [[nodiscard]] auto converged_iteration(SceneIndex scene_index) const
      -> IterationIndex
  {
    return m_scene_converged_iter_[scene_index];
  }

  [[nodiscard]] auto num_scenes() const -> Index
  {
    return static_cast<Index>(m_scene_completed_iter_.size());
  }
  [[nodiscard]] auto max_spread() const -> int { return m_max_spread_; }

private:
  /// Per-scene bounds history (ring buffer, depth = max_spread + 2).
  StrongIndexVector<SceneIndex, std::deque<SceneBounds>> m_history_;

  /// Per-scene last completed iteration (-1 = none).
  StrongIndexVector<SceneIndex, IterationIndex> m_scene_completed_iter_;

  /// Per-scene convergence flag.
  StrongIndexVector<SceneIndex, bool> m_scene_converged_;

  /// Iteration at which each scene converged (-1 = not yet).
  StrongIndexVector<SceneIndex, IterationIndex> m_scene_converged_iter_;

  /// How many scenes have completed iteration K.
  flat_map<IterationIndex, int> m_completion_counts_;

  int m_max_spread_ {0};
};

}  // namespace gtopt
