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

#include <functional>
#include <optional>

#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
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
  bool add_profile_to_lp(std::string_view cname,
                         const SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp,
                         std::string_view profile_name,
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
    map_reserve(scols, blocks.size());
    map_reserve(srows, blocks.size());

    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto block_profile = profile.at(scenario.uid(), stage.uid(), buid);
      const auto block_scost =
          sc.block_ecost(scenario, stage, block, stage_scost);

      auto name =
          sc.lp_col_label(scenario, stage, block, cname, profile_name, uid());
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
    capacity_info[st_key] = CapacityInfo {
        capacity_col,
        stage_capacity,
    };

    return true;
  }

  /**
   * @brief Add profile results to output
   *
   * @param out Output context
   * @param profile_type Profile type name ("spillover"/"unserved")
   * @return true if successful
   */
  bool add_profile_to_output(std::string_view cname,
                             OutputContext& out,
                             std::string_view profile_name) const
  {
    const auto pid = id();

    out.add_col_sol(cname, profile_name, pid, spillover_cols);
    out.add_col_cost(cname, profile_name, pid, spillover_cols);
    out.add_row_dual(cname, profile_name, pid, spillover_rows);
    return true;
  }

  /// Return the profile value for a given scenario/stage/block.
  [[nodiscard]] std::optional<double> aperture_value(ScenarioUid scenario_uid,
                                                     StageUid stage_uid,
                                                     BlockUid block_uid) const
  {
    return profile.at(scenario_uid, stage_uid, block_uid);
  }

  /**
   * @brief Update profile constraint for an aperture scenario.
   *
   * Re-reads the profile value via @p value_fn and updates the constraint
   * row accordingly.  When a capacity column was used during LP construction,
   * updates the coefficient; otherwise updates the row RHS.
   *
   * @param li            Cloned LinearInterface to modify in-place.
   * @param base_scenario Original scenario (identifies stored rows).
   * @param value_fn      (StageUid, BlockUid) -> optional<double> provider.
   * @param stage         Stage to update.
   * @return true on success.
   */
  [[nodiscard]] bool update_aperture(
      LinearInterface& li,
      const ScenarioLP& base_scenario,
      const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
      const StageLP& stage) const
  {
    if (!is_active(stage)) {
      return true;
    }

    const auto st_key = std::pair {base_scenario.uid(), stage.uid()};
    const auto row_it = spillover_rows.find(st_key);
    if (row_it == spillover_rows.end()) {
      return true;  // no rows registered for this (scenario, stage)
    }

    const auto cap_it = capacity_info.find(st_key);
    if (cap_it == capacity_info.end()) {
      return true;  // no capacity info (should not happen)
    }

    const auto& [cap_col, stage_cap] = cap_it->second;

    for (const auto& [block_uid, row] : row_it->second) {
      const auto new_profile = value_fn(stage.uid(), block_uid);
      if (!new_profile.has_value()) {
        continue;  // keep forward-pass value if not available
      }
      if (cap_col) {
        // Row: spillover + element - profile * capacity = 0
        li.set_coeff(row, *cap_col, -*new_profile);
      } else {
        // Row: spillover + element = capacity * profile
        const auto new_rhs = stage_cap * *new_profile;
        li.set_row_low(row, new_rhs);
        li.set_row_upp(row, new_rhs);
      }
    }
    return true;
  }

private:
  /// Capacity info stored during LP construction for aperture updates
  struct CapacityInfo
  {
    std::optional<ColIndex> cap_col {};
    double stage_capacity {0.0};
  };

  /// Spillover column indices (scenario × stage × block)
  STBIndexHolder<ColIndex> spillover_cols;
  /// Spillover row indices (scenario × stage × block)
  STBIndexHolder<RowIndex> spillover_rows;
  /// Capacity info per (scenario, stage) for aperture updates
  STIndexHolder<CapacityInfo> capacity_info;

  /// Profile schedule data
  STBRealSched profile;
  /// Short-term cost schedule
  OptTRealSched scost;
};

}  // namespace gtopt
