/**
 * @file      flat_helper.hpp
 * @brief     Helper for flattening multi-dimensional optimization data into LP
 * format
 * @date      Wed May 14 22:18:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @class FlatHelper
 * @brief Converts scenario/stage/block indexed data into flat vectors for
 * saving
 *
 * This helper class transforms hierarchical optimization data (organized by
 * scenarios, stages and blocks) into flat vectors suitable for data saving as
 * in parquet files. It handles:
 * - Active element filtering (only processes active scenarios/stages/blocks)
 * - Value projection and scaling during flattening
 * - Generation of validity markers for sparse problems
 * - Multiple index holder types (GSTB, STB, ST, T)
 *
 * Key Features:
 * - Efficient constexpr accessors for active elements
 * - Thread-safe operations (all methods are const)
 * - Support for custom projection and scaling
 * - Detailed error checking during construction
 * - Move semantics for efficient data transfer
 *
 * Typical Usage:
 * 1. Construct with active element indices
 * 2. Call flat() with indexed data holders
 * 3. Use returned vectors in LP matrix construction
 *
 * @note All methods are noexcept and many are constexpr for maximum performance
 * @see SystemContext which inherits and uses this functionality
 * @see GSTBIndexHolder, STBIndexHolder, STIndexHolder, TIndexHolder
 */

#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <gtopt/index_holder.hpp>
#include <gtopt/simulation_lp.hpp>

namespace gtopt
{

/**
 * @class FlatHelper
 * @brief Converts multi-dimensional optimization data into flat vectors for LP
 * formulation
 *
 * This helper class transforms scenario/stage/block indexed data into:
 * - Flat vectors for LP matrix construction
 * - Valid/invalid markers for sparse problems
 * - Scaled values using provided factors
 *
 * Key features:
 * - Handles active element filtering (only processes active
 * scenarios/stages/blocks)
 * - Applies scaling factors during flattening
 * - Supports multiple index holder types (GSTB, STB, ST, T)
 * - Provides constexpr accessors for active elements
 * - Thread-safe operations (all methods are const)
 *
 * @note All methods are noexcept and many are constexpr for maximum performance
 * @see SystemContext which inherits and uses this functionality
 */

struct STBUids
{
  std::vector<Uid> scenario_uids;
  std::vector<Uid> stage_uids;
  std::vector<Uid> block_uids;

  void reserve(size_t size)
  {
    scenario_uids.reserve(size);
    stage_uids.reserve(size);
    block_uids.reserve(size);
  }
};

struct STUids
{
  std::vector<Uid> scenario_uids;
  std::vector<Uid> stage_uids;

  void reserve(size_t size)
  {
    scenario_uids.reserve(size);
    stage_uids.reserve(size);
  }
};

struct TUids
{
  std::vector<Uid> stage_uids;

  void reserve(size_t size) { stage_uids.reserve(size); }
};

class FlatHelper
{
public:
  explicit FlatHelper(const SimulationLP& simulation)
      : m_simulation_(simulation)
      , m_active_scenarios_(std::ranges::to<std::vector>(
            simulation.scenarios() | std::views::filter(&ScenarioLP::is_active)
            | std::views::transform(&ScenarioLP::uid)))
      , m_active_stages_(std::ranges::to<std::vector>(
            simulation.stages() | std::views::filter(&StageLP::is_active)
            | std::views::transform(&StageLP::uid)))
      , m_active_stage_blocks_(std::ranges::to<std::vector>(
            simulation.stages() | std::views::filter(&StageLP::is_active)
            | std::views::transform(
                [](const StageLP& stage)
                {
                  return std::ranges::to<std::vector>(
                      stage.blocks() | std::views::transform(&BlockLP::uid));
                })))
      , m_active_blocks_(std::ranges::to<std::vector>(
            simulation.blocks() | std::views::transform(&BlockLP::uid)))
  {
  }

  [[nodiscard]] constexpr const SimulationLP& simulation() const noexcept
  {
    return m_simulation_.get();
  }

  [[nodiscard]] constexpr const auto& active_scenarios() const noexcept
  {
    return m_active_scenarios_;
  }

  [[nodiscard]] constexpr const auto& active_stages() const noexcept
  {
    return m_active_stages_;
  }

  [[nodiscard]] constexpr const auto& active_stage_blocks() const noexcept
  {
    return m_active_stage_blocks_;
  }

  [[nodiscard]] constexpr const auto& active_blocks() const noexcept
  {
    return m_active_blocks_;
  }

  [[nodiscard]] constexpr bool is_first_scenario(
      const ScenarioUid& scenario_uid) const noexcept
  {
    return !m_active_scenarios_.empty()
        && scenario_uid == m_active_scenarios_.front();
  }

  [[nodiscard]] constexpr size_t active_scenario_count() const noexcept
  {
    return m_active_scenarios_.size();
  }

  [[nodiscard]] constexpr size_t active_stage_count() const noexcept
  {
    return m_active_stages_.size();
  }

  [[nodiscard]] constexpr size_t active_block_count() const noexcept
  {
    return m_active_blocks_.size();
  }

  [[nodiscard]] constexpr bool is_first_stage(
      const StageUid& stage_uid) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_uid == m_active_stages_.front();
  }

  [[nodiscard]] constexpr bool is_first_stage(
      const StageLP& stage) const noexcept
  {
    return is_first_stage(stage.uid());
  }

  [[nodiscard]] constexpr bool is_last_stage(
      const StageUid& stage_uid) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_uid == m_active_stages_.back();
  }

  [[nodiscard]] constexpr auto prev_stage(const StageLP& stage) const noexcept
  {
    return simulation().prev_stage(stage);
  }

  [[nodiscard]] STBUids stb_uids() const;
  [[nodiscard]] STUids st_uids() const;
  [[nodiscard]] TUids t_uids() const;

  /**
   * @brief Flattens GSTB-indexed data into vectors with optional scaling
   *
   * Processes a 3D (Scenario/Stage/Block) indexed container into:
   * - Values vector with projected/scaled values
   * - Valid vector marking which indices had data
   *
   * @tparam Projection Callable that transforms source values (double ->
   * double)
   * @tparam Factor Optional scaling factors (default: no scaling)
   * @param hstb The GSTB-indexed data holder
   * @param proj Projection function to apply to each value
   * @param factor Optional scaling factors (applied after projection)
   * @return Pair of (values, valid) vectors
   *
   * @note Complexity: O(N) where N is active_scenarios * active_stages *
   * active_blocks
   * @note If factor is provided, must match active element dimensions
   */
  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const GSTBIndexHolder<Value>& hstb,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    const auto size = m_active_scenarios_.size() * m_active_blocks_.size();

    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0; auto&& suid : m_active_scenarios_) {
      for (auto [tidx, tuid] : enumerate(m_active_stages_)) {
        for (auto&& buid : m_active_stage_blocks_[tidx]) {
          auto&& stbiter = hstb.find({suid, tuid, buid});
          if (stbiter != hstb.end()) {
            const auto value = proj(stbiter->second);
            values[idx] =
                factor.empty() ? value : value * factor[suid][tuid][buid];
            valid[idx] = true;
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::pair {need_values ? std::move(values) : std::vector<double> {},
                      need_valids ? std::move(valid) : std::vector<bool> {}};
  }

  template<typename Projection,
           typename Value = Index,
           typename Factor = block_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const STBIndexHolder<Value>& hstb,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    const auto size = m_active_scenarios_.size() * m_active_blocks_.size();

    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0;
         const auto [sidx, suid] : std::views::enumerate(m_active_scenarios_))
    {
      for (const auto [tidx, tuid] : std::views::enumerate(m_active_stages_)) {
        auto&& stiter = hstb.find({suid, tuid});
        const auto has_stuid = stiter != hstb.end() && !stiter->second.empty();

        for (const auto [bidx, buid] :
             std::views::enumerate(m_active_stage_blocks_[tidx]))
        {
          if (has_stuid) {
            const auto value = proj(stiter->second.at(buid));
            values[idx] =
                factor.empty() ? value : value * factor[sidx][tidx][bidx];

            valid[idx] = true;
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::pair {need_values ? std::move(values) : std::vector<double> {},
                      need_valids ? std::move(valid) : std::vector<bool> {}};
  }

  template<typename Projection,
           typename Value = Index,
           typename Factor = scenario_stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const STIndexHolder<Value>& hst,
                                    Projection proj,
                                    const Factor& factor = Factor()) const
  {
    const auto size = m_active_scenarios_.size() * m_active_stages_.size();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0;
         const auto [sidx, suid] : std::views::enumerate(m_active_scenarios_))
    {
      for (const auto [tidx, tuid] : std::views::enumerate(m_active_stages_)) {
        auto&& stiter = hst.find({suid, tuid});
        const auto has_stuid = stiter != hst.end();

        if (has_stuid) {
          const auto value = proj(stiter->second);
          values[idx] = factor.empty() ? value : value * factor[sidx][tidx];
          valid[idx] = true;
          ++count;

          need_values = true;
        }
        need_valids |= count != ++idx;
      }
    }

    return std::pair {need_values ? std::move(values) : std::vector<double> {},
                      need_valids ? std::move(valid) : std::vector<bool> {}};
  }

  template<typename Projection = std::identity,
           typename Value = Index,
           typename Factor = stage_factor_matrix_t>
  [[nodiscard]] constexpr auto flat(const TIndexHolder<Value>& ht,
                                    Projection proj = {},
                                    const Factor& factor = {}) const
  {
    const auto size = m_active_stages_.size();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0;
         const auto [tidx, tuid] : std::views::enumerate(m_active_stages_))
    {
      auto&& titer = ht.find(tuid);
      const auto has_tuid = titer != ht.end();

      if (has_tuid) {
        const auto value = proj(titer->second);
        values[idx] = factor.empty() ? value : value * factor[tidx];

        valid[idx] = true;
        ++count;

        need_values = true;
      }
      need_valids |= count != ++idx;
    }

    return std::pair {need_values ? std::move(values) : std::vector<double> {},
                      need_valids ? std::move(valid) : std::vector<bool> {}};
  }

private:
  std::reference_wrapper<const SimulationLP> m_simulation_;

  std::vector<ScenarioUid> m_active_scenarios_ {};
  std::vector<StageUid> m_active_stages_ {};
  std::vector<std::vector<BlockUid>> m_active_stage_blocks_ {};
  std::vector<BlockUid> m_active_blocks_ {};
};

}  // namespace gtopt
