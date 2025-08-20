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

#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/**
 * @brief Base class for profile-based LP objects
 *
 * @tparam ProfileType The profile type (DemandProfile/GeneratorProfile)
 * @tparam ElementLPType The associated element LP type (DemandLP/GeneratorLP)
 */
template<typename ProfileType, typename ElementLPType>
class ProfileObjectLP : public ObjectLP<ProfileType>
{
public:
  using Base = ObjectLP<ProfileType>;
  using Base::id;
  using Base::is_active;
  using Base::uid;

  /**
   * @brief Construct a new ProfileObjectLP
   *
   * @param pprofile The profile object
   * @param ic Input context
   */
  explicit ProfileObjectLP(ProfileType pprofile,
                           InputContext& ic,
                           const LPClassName& cname)
      : Base(std::move(pprofile))
      , profile(ic, cname.full_name(), id(), std::move(Base::object().profile))
      , scost(ic, cname.full_name(), id(), std::move(Base::object().scost))
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
  bool add_profile_to_lp(const std::string_view& cname,
                         const SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp,
                         const std::string_view& profile_name,
                         const auto& element_cols,
                         const std::optional<ColIndex>& capacity_col,
                         double stage_capacity)
  {
    if (!is_active(stage)) {
      return true;
    }

    const auto stage_scost = scost.optval(stage.uid()).value_or(0.0);
    const auto& blocks = stage.blocks();

    BIndexHolder<ColIndex> scols;
    BIndexHolder<RowIndex> srows;
    scols.reserve(blocks.size());
    srows.reserve(blocks.size());

    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto block_profile = profile.at(scenario.uid(), stage.uid(), buid);
      const auto block_scost =
          sc.block_ecost(scenario, stage, block, stage_scost);

      auto name =
          sc.lp_label(scenario, stage, block, cname, profile_name, uid());
      const auto scol = lp.add_col({.name = name, .cost = block_scost});
      scols[buid] = scol;

      const auto ecol = element_cols.at(buid);
      auto srow = SparseRow {.name = std::move(name)};
      srow[scol] = 1;
      srow[ecol] = 1;

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
    spillover_cols[st_key] = std::move(scols);
    spillover_rows[st_key] = std::move(srows);

    return true;
  }

  /**
   * @brief Add profile results to output
   *
   * @param out Output context
   * @param profile_type Profile type name ("spillover"/"unserved")
   * @return true if successful
   */
  bool add_profile_to_output(const std::string_view& cname,
                             OutputContext& out,
                             std::string_view profile_name) const
  {
    const auto pid = id();

    out.add_col_sol(cname, profile_name, pid, spillover_cols);
    out.add_col_cost(cname, profile_name, pid, spillover_cols);
    out.add_row_dual(cname, profile_name, pid, spillover_rows);
    return true;
  }

private:
  /// Spillover column indices (scenario × stage × block)
  STBIndexHolder<ColIndex> spillover_cols;
  /// Spillover row indices (scenario × stage × block)
  STBIndexHolder<RowIndex> spillover_rows;

  /// Profile schedule data
  STBRealSched profile;
  /// Short-term cost schedule
  OptTRealSched scost;
};

}  // namespace gtopt
