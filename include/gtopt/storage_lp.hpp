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
  /// Propagate SoC/energy across phase/stage boundaries via StateVariables.
  /// Forced to false when daily_cycle is true.
  bool use_state_variable {true};

  /// PLP daily-cycle mode: scale block durations in the energy balance by
  /// 24/stage_duration and close each stage with efin==eini. Implies
  /// use_state_variable=false.
  bool daily_cycle {false};

  /// Energy (volume) scale factor: the LP energy variable is divided by this
  /// value so that the LP works in scaled units (physical_energy /
  /// energy_scale). Default 1.0 = no scaling. For reservoirs the default is
  /// 100000 (dam³→Gm³) and for batteries 0.1.
  double energy_scale {1.0};
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
      , ecost(ic, cname.full_name(), id(), std::move(storage().ecost))
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

  [[nodiscard]] constexpr const auto& energy_cols_at(const ScenarioLP& scenario,
                                                     const StageLP& stage) const
  {
    return energy_cols.at({scenario.uid(), stage.uid()});
  }

  /// Energy/volume scale factor used in the LP: LP_var = physical / scale.
  /// For batteries this is Battery::energy_scale; for reservoirs it is
  /// Reservoir::vol_scale.  Use to convert between LP and physical units.
  [[nodiscard]] constexpr double energy_scale() const noexcept
  {
    return m_energy_scale_;
  }

  /// Convert an LP-unit energy/volume value to physical units.
  [[nodiscard]] constexpr double to_physical(double lp_value) const noexcept
  {
    return lp_value * m_energy_scale_;
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

    // Energy scale factor: LP variable = physical_energy / energy_scale.
    // Default 1.0 = no scaling.  Reservoir default: 100000, Battery: 0.1.
    const double energy_scale =
        opts.energy_scale > 0.0 ? opts.energy_scale : 1.0;
    m_energy_scale_ = energy_scale;

    // Daily-cycle scaling is only meaningful when the stage is longer than
    // 24 h AND the average block duration exceeds 1 h.  Below those thresholds
    // the stage already represents sub-daily operation and no scaling is
    // needed.
    const auto& blocks = stage.blocks();
    const bool use_daily_cycle = opts.daily_cycle && stage.duration() > 24.0
        && (stage.duration() / static_cast<double>(blocks.size())) > 1.0;
    const double dc_stage_scale =
        use_daily_cycle ? (24.0 / stage.duration()) : 1.0;

    const auto is_last_stage =
        stage.uid() == sc.simulation().stages().back().uid();
    const auto [prev_stage, prev_phase] = sc.prev_stage(stage);

    // The objective cost is per physical energy unit; since the LP variable
    // is physical/energy_scale, multiply the coefficient by energy_scale so
    // that cost = (ecost * energy_scale) * x = ecost * physical_energy.
    const auto stage_ecost = sc.scenario_stage_ecost(  //
                                 scenario,
                                 stage,
                                 ecost.at(stage.uid()).value_or(0.0))
        / stage.duration() * energy_scale;

    const auto hour_loss =
        annual_loss.at(stage.uid()).value_or(0.0) / hours_per_year;

    // Physical bounds; will be divided by energy_scale for LP variable bounds.
    const auto [stage_emax, stage_emin] =
        sc.stage_maxmin_at(stage, emax, emin, stage_capacity);

    // LP variable bounds in scaled units.
    const double lp_emax = stage_emax / energy_scale;
    const double lp_emin = stage_emin / energy_scale;

    // Determine the initial-energy column (vicol / eini):
    //   • No previous stage (first stage of phase 0): create a fresh eini col.
    //   • Previous stage in the SAME phase: reuse its efin col (shared LP).
    //   • Previous stage in a DIFFERENT phase (SDDP boundary):
    //     - use_state_variable=true:  create a new eini col and register it as
    //       a DependentVariable of the previous phase's efin StateVariable so
    //       that PlanningLP::resolve_scene_phases() and the SDDP forward pass
    //       can propagate the trial value.
    //     - use_state_variable=false: create a new eini col without linking;
    //       an efin==eini constraint is added below to close the phase.
    ColIndex eicol;
    if (prev_stage == nullptr) {
      // First stage of the first phase – create the initial energy column.
      // eini bounds are in LP (scaled) units.
      eicol = lp.add_col({
          .name = sc.lp_label(scenario, stage, cname, "eini", uid()),
          .lowb = storage().eini.value_or(stage_emin) / energy_scale,
          .uppb = storage().eini.value_or(stage_emax) / energy_scale,
      });
    } else if (prev_phase == nullptr) {
      // Same phase – the previous stage's efin column serves as eini here
      // (both stages live in the same LP, so the column is shared).
      eicol = efin_col_at(scenario, *prev_stage);
    } else {
      // Cross-phase boundary (gtopt-phase = PLP-stage for SDDP).
      // Create a new eini column for this phase's LP.
      eicol = lp.add_col({
          .name = sc.lp_label(scenario, stage, cname, "eini", uid()),
          .lowb = lp_emin,
          .uppb = lp_emax,
      });
      if (effective_usv) {
        // Link as DependentVariable of the previous phase's efin StateVariable
        // so that PlanningLP::resolve_scene_phases() and the SDDP forward pass
        // can propagate the trial value.
        const auto efin_key =
            StateVariable::key(scenario, *prev_stage, cname, uid(), "efin");
        if (auto prev_efin = sc.get_state_variable(efin_key); prev_efin) {
          prev_efin->get().add_dependent_variable(scenario, stage, eicol);
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

    BIndexHolder<ColIndex> ecols;
    BIndexHolder<ColIndex> dcols;
    BIndexHolder<RowIndex> erows;
    BIndexHolder<RowIndex> crows;
    map_reserve(ecols, blocks.size());
    map_reserve(erows, blocks.size());
    map_reserve(crows, blocks.size());
    map_reserve(dcols, blocks.size());

    auto prev_vc = eicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto is_last_block = is_last_stage && (buid == blocks.back().uid());

      auto erow =
          SparseRow {
              .name = sc.lp_label(scenario, stage, block, cname, "vol", uid()),
          }
              .equal(0);

      // Energy LP variable is in scaled units (physical / energy_scale).
      const auto ec = lp.add_col({
          .name = erow.name,
          .lowb = !is_last_block
              ? lp_emin
              : storage().efin.value_or(stage_emin) / energy_scale,
          .uppb = lp_emax,
          .cost = stage_ecost,
      });

      ecols[buid] = ec;

      erow[prev_vc] = -(1 - (hour_loss * block.duration() * dc_stage_scale));
      erow[ec] = 1;

      // Flow coefficients are divided by energy_scale so that flow [m³/s]
      // × duration [h] × (conversion / energy_scale) = LP_energy_change.
      const auto fout_col = fout_cols.at(buid);
      const auto finp_col = finp_cols.at(buid);
      erow[fout_col] = +(flow_conversion_rate / fout_efficiency)
          * block.duration() * dc_stage_scale / energy_scale;

      // if the input and output are the same, we only need one entry
      if (fout_col != finp_col) {
        erow[finp_col] = -(flow_conversion_rate * finp_efficiency)
            * block.duration() * dc_stage_scale / energy_scale;
      }

      if (drain_cost) {
        const auto dcol = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "drain", uid()),
            .lowb = 0,
            .uppb = drain_capacity.value_or(LinearProblem::DblMax),
            .cost = sc.block_ecost(scenario, stage, block, *drain_cost),
        });

        dcols[buid] = dcol;
        erow[dcol] = flow_conversion_rate * block.duration() * dc_stage_scale
            / energy_scale;
      }

      erows[buid] = lp.add_row(std::move(erow));

      // Capacity constraint: capacity_col [physical units] >= ec [scaled units]
      // requires coefficient -energy_scale so the constraint in physical units
      // is: capacity >= ec * energy_scale (= physical energy).
      if (capacity_col) {
        auto crow =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "cap", uid()),
            }
                .greater_equal(0);
        crow[*capacity_col] = 1;
        crow[ec] = -energy_scale;

        crows[buid] = lp.add_row(std::move(crow));
      }

      prev_vc = ec;
    }

    // Register efin (the last block's energy column) as a StateVariable so
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
      //   Stage N eicol = efin_{N-1}  (column reuse, not a new variable).
      // Adding efin_N == eicol_N for each N creates the chain:
      //   efin_1 == eini, efin_2 == efin_1 == eini, …
      // so the entire phase is closed without needing to detect the last stage.
      auto close_row =
          SparseRow {
              .name = sc.lp_label(scenario, stage, cname, "eclose", uid()),
          }
              .equal(0);
      close_row[prev_vc] = 1;
      close_row[eicol] = -1;
      [[maybe_unused]] const auto close_row_idx =
          lp.add_row(std::move(close_row));
    }

    // Store the combined dual correction factor:
    //   dual_physical = dual_LP * (dc_stage_scale / energy_scale)
    // This corrects both the daily-cycle time-scaling and the energy scaling.
    // When the factor is effectively 1.0, no entry is stored; downstream
    // flat() defaults to 1.0 for absent keys (no correction applied).
    const auto st_key = std::pair {scenario.uid(), stage.uid()};
    const double dual_scale = dc_stage_scale / energy_scale;
    if (std::abs(dual_scale - 1.0) > std::numeric_limits<double>::epsilon()) {
      output_dual_scale[st_key] = dual_scale;
    }

    // storing the indices for this scenario and stage
    eini_cols[st_key] = eicol;
    efin_cols[st_key] = prev_vc;
    energy_rows[st_key] = std::move(erows);
    energy_cols[st_key] = std::move(ecols);
    if (drain_cost) {
      drain_cols[st_key] = std::move(dcols);
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

    // Primal outputs: LP variable is in scaled units
    // (physical/m_energy_scale_). Multiply by m_energy_scale_ to recover
    // physical energy/volume.
    //
    // Reduced cost (cost) outputs: the LP reduced cost is per unit of the LP
    // variable.  To convert to per unit of the physical variable, divide by
    // m_energy_scale_:  rc_phys = rc_LP / energy_scale.
    // This is the inverse of the primal rescaling, ensuring that the output
    // is invariant to the choice of energy_scale.
    if (std::abs(m_energy_scale_ - 1.0)
        > std::numeric_limits<double>::epsilon())
    {
      const auto scale = m_energy_scale_;
      const auto inv_scale = 1.0 / scale;
      const auto sol_rescale = [scale](auto v) { return v * scale; };
      const auto cost_rescale = [inv_scale](auto v) { return v * inv_scale; };
      out.add_col_sol(cname, "eini", pid, eini_cols, sol_rescale);
      out.add_col_cost(cname, "eini", pid, eini_cols, cost_rescale);
      out.add_col_sol(cname, "efin", pid, efin_cols, sol_rescale);
      out.add_col_cost(cname, "efin", pid, efin_cols, cost_rescale);
      out.add_col_sol(cname, "volumen", pid, energy_cols, sol_rescale);
      out.add_col_cost(cname, "volumen", pid, energy_cols, cost_rescale);
    } else {
      out.add_col_sol(cname, "eini", pid, eini_cols);
      out.add_col_cost(cname, "eini", pid, eini_cols);
      out.add_col_sol(cname, "efin", pid, efin_cols);
      out.add_col_cost(cname, "efin", pid, efin_cols);
      out.add_col_sol(cname, "volumen", pid, energy_cols);
      out.add_col_cost(cname, "volumen", pid, energy_cols);
    }

    // Dual output: output_dual_scale = dc_stage_scale / energy_scale.
    // This corrects both the daily-cycle time-scaling (dc_stage_scale) and the
    // energy variable scaling (1/energy_scale).  When neither applies the map
    // is empty and the flat() function defaults to 1.0 (no correction).
    out.add_row_dual(cname, "volumen", pid, energy_rows, output_dual_scale);

    out.add_row_dual(cname, "capacity", pid, capacity_rows);

    out.add_col_sol(cname, "drain", pid, drain_cols);
    out.add_col_cost(cname, "drain", pid, drain_cols);

    return true;
  }

private:
  OptTRealSched emin;
  OptTRealSched emax;
  OptTRealSched ecost;

  OptTRealSched annual_loss;

  STBIndexHolder<ColIndex> energy_cols;
  STBIndexHolder<ColIndex> drain_cols;
  STBIndexHolder<RowIndex> energy_rows;
  STBIndexHolder<RowIndex> capacity_rows;

  STIndexHolder<ColIndex> eini_cols;
  STIndexHolder<ColIndex> efin_cols;

  /// Combined dual correction factor per (scenario, stage):
  ///   dual_physical = dual_LP * output_dual_scale[{suid, tuid}]
  /// When a key is absent, downstream flat() defaults to 1.0 (no correction).
  STIndexHolder<double> output_dual_scale;

  /// Energy scale factor cached from the last add_to_lp call.
  /// Equals StorageOptions::energy_scale; used in add_to_output to rescale
  /// primal solution values back to physical units.
  double m_energy_scale_ {1.0};
};

}  // namespace gtopt
