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
protected:
    const std::vector<ScenarioIndex> m_active_scenarios_;
    const std::vector<StageIndex> m_active_stages_;
    const std::vector<std::vector<BlockIndex>> m_active_stage_blocks_;

    FlatHelper(const std::vector<ScenarioIndex>& active_scenarios,
              const std::vector<StageIndex>& active_stages,
              const std::vector<std::vector<BlockIndex>>& active_stage_blocks)
        : m_active_scenarios_(active_scenarios)
        , m_active_stages_(active_stages)
        , m_active_stage_blocks_(active_stage_blocks)
    {}

    template<typename Projection, typename Factor = block_factor_matrix_t>
    constexpr auto flat(const GSTBIndexHolder& hstb,
                        Projection proj,
                        const Factor& factor = {}) const noexcept
    {
        const auto size = m_active_scenarios_.size() * m_active_stage_blocks_.size();
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
                        const auto fact =
                            factor.empty() ? 1.0 : factor[sindex][tindex][bindex];
                        values[idx] = proj(stbiter->second) * fact;
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

    // Other flat() methods would go here...
};

} // namespace gtopt
