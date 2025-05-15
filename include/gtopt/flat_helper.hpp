#pragma once

#include <utility>
#include <vector>

#include <boost/multi_array.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_context_fwd.hpp>

namespace gtopt
{

using block_factor_matrix_t = boost::multi_array<std::vector<double>, 2>;
using stage_factor_matrix_t = std::vector<double>;
using scenario_stage_factor_matrix_t = boost::multi_array<double, 2>;

class FlatHelper
{
  std::vector<ScenarioIndex> m_active_scenarios_;
  std::vector<StageIndex> m_active_stages_;
  std::vector<std::vector<BlockIndex>> m_active_stage_blocks_;
  std::vector<BlockIndex> m_active_blocks_;

public:
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
    if (m_active_scenarios_.empty()) {
      return false;
    }
    return scenario_index == m_active_scenarios_.front();
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

  FlatHelper(const std::vector<ScenarioIndex>& active_scenarios,
             const std::vector<StageIndex>& active_stages,
             const std::vector<std::vector<BlockIndex>>& active_stage_blocks,
             const std::vector<BlockIndex>& active_blocks)
      : m_active_scenarios_(active_scenarios)
      , m_active_stages_(active_stages)
      , m_active_stage_blocks_(active_stage_blocks)
      , m_active_blocks_(active_blocks)
  {
  }

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
};

}  // namespace gtopt
