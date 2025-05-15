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

#include <gtopt/block_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>

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
class FlatHelper
{
public:
  FlatHelper() = delete;

  explicit FlatHelper(const SimulationLP& simulation,
                      std::vector<ScenarioIndex> active_scenarios,
                      std::vector<StageIndex> active_stages,
                      std::vector<std::vector<BlockIndex>> active_stage_blocks,
                      std::vector<BlockIndex> active_blocks)
      : m_simulation_(simulation)
      , m_active_scenarios_(std::move(active_scenarios))
      , m_active_stages_(std::move(active_stages))
      , m_active_stage_blocks_(std::move(active_stage_blocks))
      , m_active_blocks_(std::move(active_blocks))
  {
    if (m_active_stages_.size() != m_active_stage_blocks_.size()) {
      throw std::invalid_argument("Stage count must match stage blocks size");
    }
  }

  [[nodiscard]] constexpr const SimulationLP& simulation() const noexcept
  {
    return m_simulation_.get();
  }

  [[nodiscard]] constexpr const std::vector<ScenarioIndex>& active_scenarios()
      const noexcept
  {
    return m_active_scenarios_;
  }

  [[nodiscard]] constexpr const std::vector<StageIndex>& active_stages()
      const noexcept
  {
    return m_active_stages_;
  }

  [[nodiscard]] constexpr const std::vector<std::vector<BlockIndex>>&
  active_stage_blocks() const noexcept
  {
    return m_active_stage_blocks_;
  }

  [[nodiscard]] constexpr const std::vector<BlockIndex>& active_blocks()
      const noexcept
  {
    return m_active_blocks_;
  }

  [[nodiscard]] constexpr bool is_first_scenario(
      const ScenarioIndex& scenario_index) const noexcept
  {
    return !m_active_scenarios_.empty()
        && scenario_index == m_active_scenarios_.front();
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
      const StageIndex& stage_index) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_index == m_active_stages_.front();
  }

  [[nodiscard]] constexpr bool is_last_stage(
      const StageIndex& stage_index) const noexcept
  {
    if (m_active_stages_.empty()) {
      return false;
    }
    return stage_index == m_active_stages_.back();
  }

  [[nodiscard]] STBUids stb_active_uids() const;
  [[nodiscard]] STUids st_active_uids() const;
  [[nodiscard]] TUids t_active_uids() const;

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
  template<typename Projection, typename Factor = block_factor_matrix_t>
  constexpr auto flat(const GSTBIndexHolder& hstb,
                      Projection proj,
                      const Factor& factor = Factor()) const noexcept
  {
    const auto size = m_active_scenarios_.size() * m_active_blocks_.size();

    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0; auto&& sindex : m_active_scenarios_) {
      for (auto&& tindex : m_active_stages_) {
        for (auto&& bindex : m_active_stage_blocks_[tindex]) {
          auto&& stbiter = hstb.find({sindex, tindex, bindex});
          if (stbiter != hstb.end()) {
            const auto value = proj(stbiter->second);
            values[idx] =
                factor.empty() ? value : value * factor[sindex][tindex][bindex];
            valid[idx] = true;
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::make_pair(
        need_values ? std::move(values) : std::vector<double> {},
        need_valids ? std::move(valid) : std::vector<bool> {});
  }

  template<typename Projection, typename Factor = block_factor_matrix_t>
  constexpr auto flat(const STBIndexHolder& hstb,
                      Projection proj,
                      const Factor& factor = Factor()) const noexcept
  {
    const auto size = m_active_scenarios_.size() * m_active_blocks_.size();

    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0; auto&& sindex : m_active_scenarios_) {
      for (auto&& tindex : m_active_stages_) {
        auto&& stiter = hstb.find({sindex, tindex});
        const auto has_stindex =
            stiter != hstb.end() && !stiter->second.empty();

        for (auto&& bindex : m_active_stage_blocks_[tindex]) {
          if (has_stindex) {
            const auto value = proj(stiter->second.at(bindex));
            values[idx] =
                factor.empty() ? value : value * factor[sindex][tindex][bindex];

            valid[idx] = (true);
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::make_pair(
        need_values ? std::move(values) : std::vector<double> {},
        need_valids ? std::move(valid) : std::vector<bool> {});
  }

  template<typename Projection,
           typename Factor = scenario_stage_factor_matrix_t>
  constexpr auto flat(const STIndexHolder& hst,
                      Projection proj,
                      const Factor& factor = Factor()) const noexcept
  {
    const auto size = m_active_scenarios_.size() * m_active_stages_.size();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0; auto&& sindex : m_active_scenarios_) {
      for (auto&& tindex : m_active_stages_) {
        auto&& stiter = hst.find({sindex, tindex});
        const auto has_stindex = stiter != hst.end();

        if (has_stindex) {
          const auto value = proj(stiter->second);
          values[idx] = factor.empty() ? value : value * factor[sindex][tindex];
          valid[idx] = true;
          ++count;

          need_values = true;
        }
        need_valids |= count != ++idx;
      }
    }

    return std::make_pair(
        need_values ? std::move(values) : std::vector<double> {},
        need_valids ? std::move(valid) : std::vector<bool> {});
  }

  template<typename Projection = std::identity,
           typename Factor = stage_factor_matrix_t>
  constexpr auto flat(const TIndexHolder& ht,
                      Projection proj = {},
                      const Factor& factor = {}) const noexcept
  {
    const auto size = m_active_stages_.size();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    for (size_t count = 0; auto&& tindex : m_active_stages_) {
      auto&& titer = ht.find(tindex);
      const auto has_tindex = titer != ht.end();

      if (has_tindex) {
        const auto value = proj(titer->second);
        values[idx] = factor.empty() ? value : value * factor[tindex];

        valid[idx] = true;
        ++count;

        need_values = true;
      }
      need_valids |= count != ++idx;
    }

    return std::make_pair(
        need_values ? std::move(values) : std::vector<double> {},
        need_valids ? std::move(valid) : std::vector<bool> {});
  }

private:
  std::reference_wrapper<const SimulationLP> m_simulation_;
  std::vector<ScenarioIndex> m_active_scenarios_;
  std::vector<StageIndex> m_active_stages_;
  std::vector<std::vector<BlockIndex>> m_active_stage_blocks_;
  std::vector<BlockIndex> m_active_blocks_;
};

}  // namespace gtopt
