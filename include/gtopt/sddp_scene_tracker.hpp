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
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_common.hpp>

namespace gtopt
{

/// Per-scene bounds snapshot for one completed iteration.
struct SceneBounds
{
  double upper_bound {};
  double lower_bound {};
  bool feasible {true};
  IterationIndex iteration {};
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
      , m_num_scenes_(num_scenes)
      , m_max_spread_(max_spread)
  {
  }

  /// Record that @p scene completed iteration @p iter with the given
  /// bounds.  Maintains a ring buffer of depth max_spread + 1 per scene.
  void report_complete(SceneIndex scene,
                       IterationIndex iter,
                       double ub,
                       double lb,
                       bool feasible)
  {
    const auto si = static_cast<std::size_t>(scene);
    m_history_[si].push_back(SceneBounds {
        .upper_bound = ub,
        .lower_bound = lb,
        .feasible = feasible,
        .iteration = iter,
    });
    // Keep ring buffer bounded
    const auto max_depth = static_cast<std::size_t>(m_max_spread_) + 2;
    while (m_history_[si].size() > max_depth) {
      m_history_[si].pop_front();
    }
    m_scene_completed_iter_[si] = iter;
    m_completion_counts_[iter]++;
  }

  /// True when ALL scenes have completed at least iteration @p iter.
  [[nodiscard]] bool all_complete(IterationIndex iter) const
  {
    auto it = m_completion_counts_.find(iter);
    return it != m_completion_counts_.end() && it->second >= m_num_scenes_;
  }

  /// Get per-scene bounds for iteration @p iter.
  /// Requires `all_complete(iter)` to return true.
  [[nodiscard]] auto bounds_for_iteration(IterationIndex iter) const
      -> std::vector<SceneBounds>
  {
    std::vector<SceneBounds> result;
    result.reserve(static_cast<std::size_t>(m_num_scenes_));
    for (Index si = 0; si < m_num_scenes_; ++si) {
      const auto& hist = m_history_[static_cast<std::size_t>(si)];
      bool found = false;
      for (const auto& entry : hist) {
        if (entry.iteration == iter) {
          result.push_back(entry);
          found = true;
          break;
        }
      }
      if (!found) {
        // Should not happen when all_complete(iter) is true
        result.push_back(SceneBounds {
            .iteration = iter,
        });
      }
    }
    return result;
  }

  /// Minimum completed iteration across all scenes.
  /// Returns IterationIndex{-1} if no scene has completed any iteration.
  [[nodiscard]] auto min_completed_iteration() const -> IterationIndex
  {
    auto min_iter = IterationIndex {std::numeric_limits<int>::max()};
    for (Index si = 0; si < m_num_scenes_; ++si) {
      min_iter = std::min(
          min_iter, m_scene_completed_iter_[static_cast<std::size_t>(si)]);
    }
    return min_iter;
  }

  /// Maximum completed iteration across all scenes.
  [[nodiscard]] auto max_completed_iteration() const -> IterationIndex
  {
    auto max_iter = IterationIndex {-1};
    for (Index si = 0; si < m_num_scenes_; ++si) {
      max_iter = std::max(
          max_iter, m_scene_completed_iter_[static_cast<std::size_t>(si)]);
    }
    return max_iter;
  }

  /// Snapshot of per-scene completed iteration indices.
  [[nodiscard]] auto scene_iterations() const -> std::vector<int>
  {
    std::vector<int> result;
    result.reserve(static_cast<std::size_t>(m_num_scenes_));
    for (Index si = 0; si < m_num_scenes_; ++si) {
      result.push_back(static_cast<int>(
          m_scene_completed_iter_[static_cast<std::size_t>(si)]));
    }
    return result;
  }

  /// Mark a scene as individually converged at the given iteration.
  void mark_converged(SceneIndex scene, IterationIndex iter)
  {
    const auto si = static_cast<std::size_t>(scene);
    m_scene_converged_[si] = true;
    m_scene_converged_iter_[si] = iter;
  }

  /// True if a specific scene has individually converged.
  [[nodiscard]] bool is_converged(SceneIndex scene) const
  {
    return m_scene_converged_[static_cast<std::size_t>(scene)];
  }

  /// True when ALL scenes have individually converged.
  [[nodiscard]] bool all_converged() const
  {
    for (Index si = 0; si < m_num_scenes_; ++si) {
      if (!m_scene_converged_[static_cast<std::size_t>(si)]) {
        return false;
      }
    }
    return true;
  }

  /// Number of scenes that have individually converged.
  [[nodiscard]] auto num_converged() const -> Index
  {
    Index count = 0;
    for (Index si = 0; si < m_num_scenes_; ++si) {
      if (m_scene_converged_[static_cast<std::size_t>(si)]) {
        ++count;
      }
    }
    return count;
  }

  /// Iteration at which a scene converged (-1 if not yet converged).
  [[nodiscard]] auto converged_iteration(SceneIndex scene) const
      -> IterationIndex
  {
    return m_scene_converged_iter_[static_cast<std::size_t>(scene)];
  }

  [[nodiscard]] auto num_scenes() const -> Index { return m_num_scenes_; }
  [[nodiscard]] auto max_spread() const -> int { return m_max_spread_; }

private:
  /// Per-scene bounds history (ring buffer, depth = max_spread + 2).
  std::vector<std::deque<SceneBounds>> m_history_;

  /// Per-scene last completed iteration (-1 = none).
  std::vector<IterationIndex> m_scene_completed_iter_;

  /// Per-scene convergence flag.
  std::vector<bool> m_scene_converged_;

  /// Iteration at which each scene converged (-1 = not yet).
  std::vector<IterationIndex> m_scene_converged_iter_;

  /// How many scenes have completed iteration K.
  flat_map<IterationIndex, int> m_completion_counts_;

  Index m_num_scenes_ {0};
  int m_max_spread_ {0};
};

}  // namespace gtopt
