/**
 * @file      profile_object_lp.hpp
 * @brief     Base class for profile-based LP objects
 * @date      Wed Apr 17 10:20:35 2025  
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ProfileObjectLP base class which contains common functionality
 * for profile-based LP objects like DemandProfileLP and GeneratorProfileLP.
 */

#pragma once

#include <gtopt/object_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/output_context.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace gtopt {

/**
 * @brief Base class for profile-based LP objects
 * 
 * @tparam ProfileType The profile type (DemandProfile/GeneratorProfile)
 * @tparam ElementLPType The associated element LP type (DemandLP/GeneratorLP) 
 */
template<typename ProfileType, typename ElementLPType>
class ProfileObjectLP : public ObjectLP<ProfileType> {
    using Base = ObjectLP<ProfileType>;
    using ClassName = typename Base::ClassName;
    using id = typename Base::id;
    using Base::is_active;

    /// Spillover column indices (scenario × stage × block)
    STBIndexHolder<ColIndex> spillover_cols_;
    /// Spillover row indices (scenario × stage × block) 
    STBIndexHolder<RowIndex> spillover_rows_;

    /// Profile schedule data
    STBRealSched profile_;
    /// Short-term cost schedule
    OptTRealSched scost_;

    /**
     * @brief Construct a new ProfileObjectLP
     * 
     * @param pprofile The profile object
     * @param ic Input context
     */
    explicit ProfileObjectLP(ProfileType pprofile, InputContext& ic)
        : Base(std::move(pprofile))
        , profile_(ic, ClassName{}, id{}(), std::move(Base::object().profile))
        , scost_(ic, ClassName{}, id{}(), std::move(Base::object().scost))
    {
    }

    /**
     * @brief Add profile constraints to LP
     * 
     * @param sc System context
     * @param scenario Scenario
     * @param stage Stage
     * @param lp Linear problem
     * @param profile_type Profile type name ("spillover"/"unserved")
     * @param element_col Element column index
     * @param capacity_col Capacity column index (if any)
     * @param stage_capacity Stage capacity value
     * @return true if successful
     * @return false if failed
     */
    bool add_profile_to_lp(
        const SystemContext& sc,
        const ScenarioLP& scenario,
        const StageLP& stage,
        LinearProblem& lp,
        std::string_view profile_type,
        ColIndex element_col,
        const std::optional<ColIndex>& capacity_col,
        double stage_capacity)
    {
        if (!is_active(stage)) {
            return true;
        }

        const auto stage_scost = scost_.optval(stage.uid()).value_or(0.0);
        const auto& blocks = stage.blocks();

        BIndexHolder<ColIndex> scols;
        BIndexHolder<RowIndex> srows;
        scols.reserve(blocks.size());
        srows.reserve(blocks.size());

        for (const auto& block : blocks) {
            const auto buid = block.uid();
            const auto block_profile = profile_.at(scenario.uid(), stage.uid(), buid);
            const auto block_scost = sc.block_ecost(scenario, stage, block, stage_scost);

            auto name = sc.lp_label(scenario, stage, block, 
                                  ClassName::short_name(), profile_type, id{}());
            const auto scol = lp.add_col({
                .name = name, 
                .cost = block_scost
            });
            scols[buid] = scol;

            auto srow = SparseRow {.name = std::move(name)};
            srow[scol] = 1;
            srow[element_col] = 1;

            if (capacity_col) {
                srow[*capacity_col] = -block_profile;
                srows[buid] = lp.add_row(std::move(srow.equal(0)));
            } else {
                const auto cprofile = stage_capacity * block_profile;
                srows[buid] = lp.add_row(std::move(srow.equal(cprofile)));
            }
        }

        // Store indices for this scenario and stage
        const auto st_key = std::pair {scenario.uid(), stage.uid()};
        spillover_cols_[st_key] = std::move(scols);
        spillover_rows_[st_key] = std::move(srows);

        return true;
    }

    /**
     * @brief Add profile results to output
     * 
     * @param out Output context
     * @param profile_type Profile type name ("spillover"/"unserved")
     * @return true if successful
     */
    bool add_profile_to_output(OutputContext& out, 
                             std::string_view profile_type) const
    {
        out.add_col_sol(ClassName::full_name(), profile_type, id{}(), spillover_cols_);
        out.add_row_dual(ClassName::full_name(), profile_type, id{}(), spillover_rows_);
        return true;
    }
};

} // namespace gtopt
