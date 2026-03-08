/**
 * @file      storage_lp.hpp
 * @brief     Header of
 * @date      Wed Apr  2 01:47:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <limits>

#include <gtopt/index_holder.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/// Options controlling storage LP behaviour for a single add_to_lp call.
struct StorageOptions
{
  /// Propagate SoC/volume across phase/stage boundaries via StateVariables.
  /// Forced to false when daily_cycle is true.
  bool use_state_variable {true};

  /// PLP daily-cycle mode: scale block durations in the energy balance by
  /// 24/stage_duration and close each stage with efin==eini. Implies
  /// use_state_variable=false.
  bool daily_cycle {false};
};

template<typename Object>
class StorageLP : public Object
{
public:
  using Object::id;
  using Object::is_active;
  using Object::object;
  using Object::uid;

  [[nodiscard]] constexpr auto&& storage(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return self.object();
  }

  template<typename ObjectT>
  explicit StorageLP(ObjectT&& pstorage,
                     const InputContext& ic,
                     const LPClassName& cname)
      : Object(std::forward<ObjectT>(pstorage), ic, cname)
      , emin(ic, cname.full_name(), id(), std::move(storage().emin))
      , emax(ic, cname.full_name(), id(), std::move(storage().emax))
      , vcost(ic, cname.full_name(), id(), std::move(storage().vcost))
      , annual_loss(
            ic, cname.full_name(), id(), std::move(storage().annual_loss))
  {
  }

  [[nodiscard]] constexpr auto efin_col_at(const ScenarioLP& scenario,
                                           const StageLP& stage) const
  {
    return efin_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr auto eini_col_at(const ScenarioLP& scenario,
                                           const StageLP& stage) const
  {
    return eini_cols.at({scenario.uid(), stage.uid()});
  }

  template<typename SystemContextT>
  bool add_to_lp(std::string_view cname,
                 SystemContextT& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp,
                 const double flow_conversion_rate,
                 const BIndexHolder<ColIndex>& finp_cols,
                 const double finp_efficiency,
                 const BIndexHolder<ColIndex>& fout_cols,
                 const double fout_efficiency,
                 const double stage_capacity,
                 const std::optional<ColIndex> capacity_col = {},
                 const std::optional<Real> drain_cost = {},
                 const std::optional<Real> drain_capacity = {},
                 const StorageOptions opts = {})
  {
    if (!is_active(stage)) {
      return true;
    }

    const bool effective_usv =
        opts.daily_cycle ? false : opts.use_state_variable;

    // Daily-cycle scaling is only meaningful when the stage is longer than
    // 24 h AND the average block duration exceeds 1 h.  Below those thresholds
    // the stage already represents sub-daily operation and no scaling is
    // needed.
    const auto& blocks = stage.blocks();
    const bool use_daily_cycle = opts.daily_cycle && stage.duration() > 24.0
        && (stage.duration() / static_cast<double>(blocks.size())) > 1.0;
    const double eff_block_scale =
        use_daily_cycle ? (24.0 / stage.duration()) : 1.0;

    const auto is_last_stage =
        stage.uid() == sc.simulation().stages().back().uid();
    const auto [prev_stage, prev_phase] = sc.prev_stage(stage);

    const auto stage_vcost = sc.scenario_stage_ecost(  //
                                 scenario,
                                 stage,
                                 vcost.at(stage.uid()).value_or(0.0))
        / stage.duration();

    const auto hour_loss =
        annual_loss.at(stage.uid()).value_or(0.0) / hours_per_year;

    const auto [stage_emax, stage_emin] =
        sc.stage_maxmin_at(stage, emax, emin, stage_capacity);

    // Determine the initial-volume column (vicol / eini):
    //   • No previous stage (first stage of phase 0): create a fresh eini col.
    //   • Previous stage in the SAME phase: reuse its efin col (shared LP).
    //   • Previous stage in a DIFFERENT phase (SDDP boundary):
    //     - use_state_variable=true:  create a new eini col and register it as
    //       a DependentVariable of the previous phase's efin StateVariable so
    //       that PlanningLP::resolve_scene_phases() and the SDDP forward pass
    //       can propagate the trial value.
    //     - use_state_variable=false: create a new eini col without linking;
    //       an efin==eini constraint is added below to close the phase.
    ColIndex vicol;
    if (prev_stage == nullptr) {
      // First stage of the first phase – create the initial volume column.
      vicol = lp.add_col({
          .name = sc.lp_label(scenario, stage, cname, "eini", uid()),
          .lowb = storage().eini.value_or(stage_emin),
          .uppb = storage().eini.value_or(stage_emax),
      });
    } else if (prev_phase == nullptr) {
      // Same phase – the previous stage's efin column serves as eini here
      // (both stages live in the same LP, so the column is shared).
      vicol = efin_col_at(scenario, *prev_stage);
    } else {
      // Cross-phase boundary (gtopt-phase = PLP-stage for SDDP).
      // Create a new eini column for this phase's LP.
      vicol = lp.add_col({
          .name = sc.lp_label(scenario, stage, cname, "eini", uid()),
          .lowb = stage_emin,
          .uppb = stage_emax,
      });
      if (effective_usv) {
        // Link as DependentVariable of the previous phase's efin StateVariable
        // so that PlanningLP::resolve_scene_phases() and the SDDP forward pass
        // can propagate the trial value.
        const auto efin_key =
            StateVariable::key(scenario, *prev_stage, cname, uid(), "efin");
        if (auto prev_efin = sc.get_state_variable(efin_key); prev_efin) {
          prev_efin->get().add_dependent_variable(scenario, stage, vicol);
        } else {
          SPDLOG_WARN(
              "StorageLP: no efin StateVariable found for cross-phase eini "
              "linking (class='{}' uid={} phase boundary). "
              "Reservoir/battery state will NOT be coupled across this phase.",
              cname,
              static_cast<int>(uid()));
        }
      }
      // If !use_state_variable: eini is free (within emin/emax bounds).
      // An efin==eini close constraint is added after the block loop below.
    }

    BIndexHolder<ColIndex> vcols;
    BIndexHolder<ColIndex> dcols;
    BIndexHolder<RowIndex> vrows;
    BIndexHolder<RowIndex> crows;
    map_reserve(vcols, blocks.size());
    map_reserve(vrows, blocks.size());
    map_reserve(crows, blocks.size());
    map_reserve(dcols, blocks.size());

    auto prev_vc = vicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto is_last_block = is_last_stage && (buid == blocks.back().uid());

      auto vrow =
          SparseRow {
              .name = sc.lp_label(scenario, stage, block, cname, "vol", uid()),
          }
              .equal(0);

      const auto vc = lp.add_col({
          .name = vrow.name,
          .lowb =
              !is_last_block ? stage_emin : storage().efin.value_or(stage_emin),
          .uppb = stage_emax,
          .cost = stage_vcost,
      });

      vcols[buid] = vc;

      vrow[prev_vc] = -(1 - (hour_loss * block.duration() * eff_block_scale));
      vrow[vc] = 1;

      const auto fout_col = fout_cols.at(buid);
      const auto finp_col = finp_cols.at(buid);
      vrow[fout_col] = +(flow_conversion_rate / fout_efficiency)
          * block.duration() * eff_block_scale;

      // if the input and output are the same, we only need one entry
      if (fout_col != finp_col) {
        vrow[finp_col] = -(flow_conversion_rate * finp_efficiency)
            * block.duration() * eff_block_scale;
      }

      if (drain_cost) {
        const auto dcol = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "drain", uid()),
            .lowb = 0,
            .uppb = drain_capacity.value_or(LinearProblem::DblMax),
            .cost = sc.block_ecost(scenario, stage, block, *drain_cost),
        });

        dcols[buid] = dcol;
        vrow[dcol] = flow_conversion_rate * block.duration() * eff_block_scale;
      }

      vrows[buid] = lp.add_row(std::move(vrow));

      // adding the capacity constraint
      if (capacity_col) {
        auto crow =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "cap", uid()),
            }
                .greater_equal(0);
        crow[*capacity_col] = 1;
        crow[vc] = -1;

        crows[buid] = lp.add_row(std::move(crow));
      }

      prev_vc = vc;
    }

    // Register efin (the last block's volume column) as a StateVariable so
    // that PlanningLP::resolve_scene_phases() and the SDDP solver can
    // discover and propagate the reservoir/battery state across phase
    // boundaries (gtopt-phase = PLP-stage).
    if (effective_usv) {
      sc.add_state_variable(
          StateVariable::key(scenario, stage, cname, uid(), "efin"), prev_vc);
    } else {
      // No cross-stage/phase state coupling: add efin == eini constraint so
      // that each independent segment (phase or single-stage horizon) is
      // "closed" – i.e., the storage ends at the same energy level it started.
      //
      // The constraint is added for EVERY stage in the phase (not just the
      // first or last) because within-phase stages share efin columns:
      //   Stage N vicol = efin_{N-1}  (column reuse, not a new variable).
      // Adding efin_N == vicol_N for each N creates the chain:
      //   efin_1 == eini, efin_2 == efin_1 == eini, …
      // so the entire phase is closed without needing to detect the last stage.
      auto close_row =
          SparseRow {
              .name = sc.lp_label(scenario, stage, cname, "eclose", uid()),
          }
              .equal(0);
      close_row[prev_vc] = 1;
      close_row[vicol] = -1;
      [[maybe_unused]] const auto close_row_idx =
          lp.add_row(std::move(close_row));
    }

    // storing the indices for this scenario and stage
    const auto st_key = std::pair {scenario.uid(), stage.uid()};
    eini_cols[st_key] = vicol;
    efin_cols[st_key] = prev_vc;
    volumen_rows[st_key] = std::move(vrows);
    volumen_cols[st_key] = std::move(vcols);
    if (drain_cost) {
      drain_cols[st_key] = std::move(dcols);
    }
    if (use_daily_cycle) {
      volumen_dc_scale[st_key] = 24.0 / stage.duration();
    }

    if (!crows.empty()) {
      capacity_rows[st_key] = std::move(crows);
    }

    return true;
  }

  template<typename OutputContext>
  bool add_to_output(OutputContext& out, std::string_view cname) const
  {
    const auto pid = id();

    out.add_col_sol(cname, "eini", pid, eini_cols);
    out.add_col_cost(cname, "eini", pid, eini_cols);
    out.add_col_sol(cname, "efin", pid, efin_cols);
    out.add_col_cost(cname, "efin", pid, efin_cols);

    out.add_col_sol(cname, "volumen", pid, volumen_cols);
    out.add_col_cost(cname, "volumen", pid, volumen_cols);
    // Back-scale volume balance duals: when daily_cycle was active for a stage
    // the LP duals are scaled by stage_dur/24, so we multiply by 24/stage_dur
    // (stored in volumen_dc_scale).  When volumen_dc_scale is empty (no stage
    // used daily_cycle), the st_scale overload defaults every entry to 1.0 and
    // the result is identical to the plain add_row_dual call.
    out.add_row_dual(cname, "volumen", pid, volumen_rows, volumen_dc_scale);

    out.add_row_dual(cname, "capacity", pid, capacity_rows);

    out.add_col_sol(cname, "drain", pid, drain_cols);
    out.add_col_cost(cname, "drain", pid, drain_cols);

    return true;
  }

private:
  OptTRealSched emin;
  OptTRealSched emax;
  OptTRealSched vcost;

  OptTRealSched annual_loss;

  STBIndexHolder<ColIndex> volumen_cols;
  STBIndexHolder<ColIndex> drain_cols;
  STBIndexHolder<RowIndex> volumen_rows;
  STBIndexHolder<RowIndex> capacity_rows;

  STIndexHolder<ColIndex> eini_cols;
  STIndexHolder<ColIndex> efin_cols;

  STIndexHolder<double> volumen_dc_scale;
};

}  // namespace gtopt
