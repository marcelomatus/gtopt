/**
 * @file      plant_lp.hpp
 * @brief     LP build path for Plant elements
 * @date      Sat May 30 12:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Builds Σ-capacity and Σ-commit / uniq rows for a `Plant` element.
 * No new LP columns are created — the rows reference already-built
 * generator `generation_cols` and commitment `status_cols` (when a
 * Commitment exists for the variant).
 *
 * Wired into `SystemLP::collections_t` AFTER `CommitmentLP` so the
 * commit / uniq rows can find the per-(stage, block) status columns
 * built by the commitment pass.  After `UserConstraintLP` only by the
 * ordering convention — Plant rows are purely physical caps and never
 * reference user-constraint columns.
 */

#pragma once

#include <gtopt/element_index.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/plant.hpp>

namespace gtopt
{

using PlantLPSId = ObjectSingleId<class PlantLP>;

class PlantLP : public ObjectLP<Plant>
{
public:
  static constexpr std::string_view CapacityName {"cap"};
  static constexpr std::string_view CommitName {"commit"};
  static constexpr std::string_view UniqName {"uniq"};

  explicit PlantLP(const Plant& plant, const InputContext& ic);

  [[nodiscard]] constexpr auto&& plant(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] static bool add_to_output(OutputContext& out);

  /// Per-(scenario, stage) per-block Σ-capacity rows
  /// (``plant_cap_<name>``).  Empty when ``pmax`` is unset.
  [[nodiscard]] const auto& capacity_rows() const noexcept
  {
    return capacity_rows_;
  }

  /// Per-(scenario, stage) Σ-commit rows (``plant_commit_<name>``).
  /// Empty when ``n_units`` is unset.
  [[nodiscard]] const auto& commit_rows() const noexcept
  {
    return commit_rows_;
  }

  /// Per-(scenario, stage) Σ-status ≤ 1 rows (``plant_uniq_<name>``).
  /// Empty when ``uniq_mutex`` is false.
  [[nodiscard]] const auto& uniq_rows() const noexcept { return uniq_rows_; }

private:
  /// Pre-resolved generator indices into the SystemLP's
  /// `Collection<GeneratorLP>`.  Empty entries for unknown names log a
  /// single warning at construction time so the (scen, stg) loop can
  /// skip them silently.
  std::vector<ElementIndex<GeneratorLP>> generator_indices_;

  STBIndexHolder<RowIndex> capacity_rows_;
  STIndexHolder<RowIndex> commit_rows_;
  STIndexHolder<RowIndex> uniq_rows_;
};

}  // namespace gtopt
