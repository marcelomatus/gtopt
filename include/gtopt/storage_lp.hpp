/**
 * @file      storage_lp.hpp
 * @brief     LP formulation for energy storage elements
 * @date      Wed Apr  2 01:47:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines StorageLP and StorageOptions, which build the LP
 * energy balance constraints and state-of-charge variables for storage.
 */

#pragma once

#include <limits>
#include <span>

#include <gtopt/cost_helper.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
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

  /// Skip linking sini to the previous phase's efin at this cross-phase
  /// boundary.  When true, the sini column is left free (within emin/emax
  /// bounds) so that the caller can fix it to a provisioned value (e.g.,
  /// VolumeRight reset_month).  The efin StateVariable is still registered
  /// for outgoing propagation to the next phase.
  ///
  /// This prevents SDDP from propagating stale duals backward through a
  /// rights reset boundary, and from overwriting the provisioned eini with
  /// the previous phase's efin trial value in the forward pass.
  ///
  /// Should only be set when the stage is a phase boundary AND the storage
  /// element's initial state is independently determined (reset/reprovision).
  bool skip_state_link {false};

  /// Full class name for VariableScaleMap metadata (e.g. "Reservoir",
  /// "Battery").  When non-empty, StorageBase sets class_name/variable_name
  /// on energy columns so LinearProblem::add_col can auto-resolve scales.
  std::string_view class_name {};

  /// Element UID for per-element VariableScaleMap lookup.
  Uid variable_uid {unknown_uid};

  /// Energy (volume) scale factor: the LP energy variable is divided by this
  /// value so that the LP works in scaled units (physical_energy /
  /// energy_scale). Default 1.0 = no scaling. For reservoirs the default is
  /// 100000 (dam³→Gm³) and for batteries 0.01.
  double energy_scale {1.0};

  /// Flow variable scale factor applied to finp/fout/drain LP variables.
  ///
  /// flatten() applies col_scale to both flow and energy column coefficients,
  /// so the physical coefficient in the energy-balance row is simply:
  ///
  ///   coeff = flow_conversion_rate × duration
  ///
  /// For drain: bounds are divided and LP cost is multiplied by flow_scale so
  /// that the physical objective value is preserved.
  ///
  /// Default 1.0 = no flow scaling (battery behaviour is unchanged).
  double flow_scale {1.0};

  /// State cost for elastic penalty [$/physical_unit].  Passed to the
  /// StateVariable at registration time so the SDDP elastic filter can
  /// apply per-variable penalty costs.  Default 0.0 = use global penalty.
  double scost {0.0};
};

template<typename Object>
class StorageLP : public Object
{
public:
  using Object::id;
  using Object::is_active;
  using Object::object;
  using Object::uid;

  // LP variable/constraint name constants — shared between add_to_lp and
  // add_to_output so that column names are guaranteed unique and consistent.
  static constexpr std::string_view EiniName {"eini"};
  static constexpr std::string_view SiniName {"sini"};
  static constexpr std::string_view EnergyName {"energy"};
  static constexpr std::string_view SoftEminName {"soft_emin"};
  static constexpr std::string_view DrainName {"drain"};
  static constexpr std::string_view EfinName {"efin"};
  static constexpr std::string_view CapacityName {"capacity"};
  static constexpr std::string_view SeminGeName {"semin_ge"};

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
      , soft_emin(ic, cname.full_name(), id(), std::move(storage().soft_emin))
      , soft_emin_cost(
            ic, cname.full_name(), id(), std::move(storage().soft_emin_cost))
  {
  }

  [[nodiscard]] constexpr auto efin_col_at(const ScenarioLP& scenario,
                                           const StageLP& stage) const
  {
    return efin_cols.at({scenario.uid(), stage.uid()});
  }

  /// Return the initial-energy column for (scenario, stage).
  ///
  /// Three cases:
  ///   - Global initial condition (first stage of first phase): stored in
  ///     eini_cols with a fixed "eini" column.
  ///   - Same-phase reuse: eini_cols stores the previous stage's efin col.
  ///   - Cross-phase SDDP boundary: sini_cols stores the "sini" col; fall
  ///     back from eini_cols (which does NOT have this entry) to sini_cols.
  ///
  /// Invariant: every active (scenario, stage) pair is stored in exactly one
  /// of the two maps.  The fallback throws std::out_of_range only if the
  /// caller passes an invalid (scenario, stage) combination.
  [[nodiscard]] ColIndex eini_col_at(const ScenarioLP& scenario,
                                     const StageLP& stage) const
  {
    const auto key = std::tuple {scenario.uid(), stage.uid()};
    if (const auto it = eini_cols.find(key); it != eini_cols.end()) {
      return it->second;
    }
    // Cross-phase SDDP boundary: key must be in sini_cols.
    // std::out_of_range is thrown if neither map has the key, which
    // indicates a programming error (stage was never added to the LP).
    return sini_cols.at(key);
  }

  [[nodiscard]] constexpr const auto& energy_cols_at(const ScenarioLP& scenario,
                                                     const StageLP& stage) const
  {
    return energy_cols.at({scenario.uid(), stage.uid()});
  }

  /// Return the energy balance row indices for (scenario, stage).
  ///
  /// These are the storage balance constraint rows (one per block).
  /// External entities can add coefficients to these rows to couple
  /// their flow variables into this storage's energy balance.
  [[nodiscard]] constexpr const auto& energy_rows_at(const ScenarioLP& scenario,
                                                     const StageLP& stage) const
  {
    return energy_rows.at({scenario.uid(), stage.uid()});
  }

  /// Return the drain/spill column indices for (scenario, stage).
  ///
  /// Drain columns represent the spillway (for reservoirs) or energy
  /// curtailment (for batteries).  They are only present when the storage
  /// object has a non-zero drain cost; if absent for the requested pair,
  /// `std::out_of_range` is thrown (caught by the user-constraint resolver
  /// to produce a graceful `std::nullopt`).
  [[nodiscard]] constexpr const auto& drain_cols_at(const ScenarioLP& scenario,
                                                    const StageLP& stage) const
  {
    return drain_cols.at({scenario.uid(), stage.uid()});
  }

  /// Non-throwing lookup: returns a pointer to the drain/spill column map
  /// for (scenario, stage), or nullptr when not present.
  [[nodiscard]] constexpr const BIndexHolder<ColIndex>* find_drain_cols(
      const ScenarioLP& scenario, const StageLP& stage) const noexcept
  {
    const auto it = drain_cols.find({scenario.uid(), stage.uid()});
    return it != drain_cols.end() ? &it->second : nullptr;
  }

  /// Return the soft-emin slack column for (scenario, stage), if it exists.
  ///
  /// The soft-emin slack is only created when `soft_emin > 0` and
  /// `soft_emin_cost > 0` for the given stage.  Returns `std::nullopt` when
  /// the column was not created (i.e., soft_emin is inactive for this stage).
  [[nodiscard]] std::optional<ColIndex> soft_emin_col_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    const auto key = std::tuple {scenario.uid(), stage.uid()};
    if (const auto it = soft_emin_slack_cols.find(key);
        it != soft_emin_slack_cols.end())
    {
      return it->second;
    }
    return std::nullopt;
  }

  /// Energy/volume scale factor used in the LP: LP_var = physical / scale.
  /// For batteries this is Battery::energy_scale; for reservoirs it is
  /// Reservoir::energy_scale.  Use to convert between LP and physical units.
  [[nodiscard]] constexpr double energy_scale() const noexcept
  {
    return m_energy_scale_;
  }

  /// Flow variable scale factor used in the LP.
  ///
  /// For drain (and extraction/finp/fout in the reservoir case):
  ///   LP_var = physical / flow_scale.
  /// Default 1.0 (no scaling; battery default).  Reservoirs use energy_scale.
  [[nodiscard]] constexpr double flow_scale() const noexcept
  {
    return m_flow_scale_;
  }

  /// Convert an LP-unit energy/volume value to physical units.
  [[nodiscard]] constexpr double to_physical(double lp_value) const noexcept
  {
    return lp_value * m_energy_scale_;
  }

  /// Retrieve a physical energy/volume value from an LP column vector.
  /// @param col_values  LP solution (or bounds) vector indexed by ColIndex
  /// @param col         Column index of the energy/volume variable
  /// @return The column value converted to physical units
  [[nodiscard]] constexpr double physical_col_value(
      std::span<const double> col_values, ColIndex col) const noexcept
  {
    return col_values[col] * m_energy_scale_;
  }

  /// Retrieve the physical eini (initial energy/volume) for a given
  /// scenario and stage.
  ///
  /// For the first stage of the first phase the eini column is the fixed
  /// initial condition, so @p default_eini is returned directly.
  ///
  /// For cross-phase boundaries (phase > 0), eini corresponds to the
  /// previous phase's efin.  When the current LP hasn't been solved and
  /// no warm solution is available, the method looks up the previous
  /// phase's efin from sys.prev_phase_sys().  Fallback chain:
  ///   1. Current LP optimal solution (eini/sini column)
  ///   2. Current LP warm column solution (from hot-start state file)
  ///   3. Previous phase's efin (via sys.prev_phase_sys())
  ///   4. default_eini (system initial volume / vini)
  ///
  /// @param sys          Current SystemLP (provides linear_interface and
  ///   prev_phase_sys for cross-phase lookups).
  /// @param scenario     Current scenario LP object.
  /// @param stage        Current stage LP object.
  /// @param default_eini Initial energy/volume when no LP solution exists.
  /// @param sid  ObjectSingleId for this element, used to look up
  ///   the same storage element in the previous phase.
  template<typename SystemLPT, typename SIdT>
  [[nodiscard]] double physical_eini(const SystemLPT& sys,
                                     const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     double default_eini,
                                     const SIdT& sid) const
  {
    if (!stage.index() && !stage.phase_index()) {
      return default_eini;
    }
    const auto& li = sys.linear_interface();
    const auto col = eini_col_at(scenario, stage);
    if (li.is_optimal()) {
      return physical_col_value(li.get_col_sol_raw(), col);
    }
    const auto& warm = li.warm_col_sol();
    if (!warm.empty() && static_cast<size_t>(col) < warm.size()) {
      return physical_col_value(warm, col);
    }
    // Cross-phase fallback: eini at phase N == efin at phase N-1.
    // Look up the same storage element in the previous phase and
    // retrieve its efin from the last stage.
    if (const auto* prev_sys = sys.prev_phase_sys()) {
      const auto& prev_rsv =
          prev_sys->template element<typename SIdT::object_type>(sid);
      const auto& prev_li = prev_sys->linear_interface();
      const auto& prev_stages = prev_sys->phase().stages();
      if (!prev_stages.empty()) {
        return prev_rsv.physical_efin(
            prev_li, scenario, prev_stages.back(), default_eini);
      }
    }
    return default_eini;
  }

  /// Retrieve the physical eini without cross-phase lookup.
  ///
  /// Used by callers that only have a LinearInterface (e.g. tests, non-SDDP
  /// code).  Fallback chain: optimal solution → warm solution → default_eini.
  [[nodiscard]] double physical_eini(const LinearInterface& li,
                                     const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     double default_eini) const
  {
    if (!stage.index() && !stage.phase_index()) {
      return default_eini;
    }
    const auto col = eini_col_at(scenario, stage);
    if (li.is_optimal()) {
      return physical_col_value(li.get_col_sol_raw(), col);
    }
    const auto& warm = li.warm_col_sol();
    if (!warm.empty() && static_cast<size_t>(col) < warm.size()) {
      return physical_col_value(warm, col);
    }
    return default_eini;
  }

  /// Retrieve the physical efin (final energy/volume) for a given
  /// scenario and stage.  Fallback chain:
  ///   1. LP optimal solution
  ///   2. Warm column solution (loaded from hot-start state file)
  ///   3. default_efin
  [[nodiscard]] double physical_efin(const LinearInterface& li,
                                     const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     double default_efin) const
  {
    const auto col = efin_col_at(scenario, stage);
    if (li.is_optimal()) {
      return physical_col_value(li.get_col_sol_raw(), col);
    }
    const auto& warm = li.warm_col_sol();
    if (!warm.empty() && static_cast<size_t>(col) < warm.size()) {
      return physical_col_value(warm, col);
    }
    return default_efin;
  }

  /// Overload accepting a SystemLP (extracts LinearInterface internally).
  template<typename SystemLPT>
  [[nodiscard]] double physical_efin(const SystemLPT& sys,
                                     const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     double default_efin) const
  {
    return physical_efin(sys.linear_interface(), scenario, stage, default_efin);
  }

  template<typename SystemContextT>
  bool add_to_lp(std::string_view cname,
                 std::string_view ampl_class,
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
    // Default 1.0 = no scaling.  Both Battery and Reservoir default to 1.0.
    const double energy_scale =
        opts.energy_scale > 0.0 ? opts.energy_scale : 1.0;
    m_energy_scale_ = energy_scale;

    // Flow variable scale factor for finp/fout/drain LP variables.
    // When flow_scale > 1.0 the caller has pre-divided those variables by
    // flow_scale; we multiply the energy-balance coefficients by flow_scale to
    // compensate.  Default 1.0 = no flow scaling (battery case).
    const double flow_scale = opts.flow_scale > 0.0 ? opts.flow_scale : 1.0;
    m_flow_scale_ = flow_scale;

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

    // Physical objective cost per energy unit.  flatten() applies col_scale
    // so that cost_LP = cost_phys × col_scale / scale_objective.
    const auto stage_ecost = sc.scenario_stage_ecost(  //
                                 scenario,
                                 stage,
                                 ecost.at(stage.uid()).value_or(0.0))
        / stage.duration();

    const auto hour_loss =
        annual_loss.at(stage.uid()).value_or(0.0) / hours_per_year;
    const auto stg_ctx = make_stage_context(scenario.uid(), stage.uid());

    // Physical bounds — stored directly in SparseCol; flatten() converts
    // to LP units by dividing by col.scale.
    const auto [stage_emax, stage_emin] =
        sc.stage_maxmin_at(stage, emax, emin, stage_capacity);

    // Determine the initial-energy column (vicol / eini):
    //
    //  ┌─────────────────────┬──────────────────────────────────────────────┐
    //  │ Case                │ LP column                                    │
    //  ├─────────────────────┼──────────────────────────────────────────────┤
    //  │ First stage of      │ "eini" col – GLOBAL INITIAL CONDITION.       │
    //  │ first phase         │ Bounds fixed to storage().eini.              │
    //  │                     │ Only one per scenario, in phase-0 LP only.   │
    //  ├─────────────────────┼──────────────────────────────────────────────┤
    //  │ Same phase,         │ Reuse previous stage's efin col (shared LP). │
    //  │ later stage         │ Inter-stage connection – no new column.      │
    //  ├─────────────────────┼──────────────────────────────────────────────┤
    //  │ Cross-phase SDDP    │ "sini" col – INTER-PHASE STATE VARIABLE.     │
    //  │ boundary            │ Bounds free (lp_emin/lp_emax); SDDP forward  │
    //  │                     │ pass fixes to prev phase's efin trial value. │
    //  │                     │ Linked via DependentVariable when usv=true.  │
    //  └─────────────────────┴──────────────────────────────────────────────┘
    //
    // Global final condition ("efin") is an explicit named constraint row
    // added AFTER the block loop for the last stage of the last phase only.
    ColIndex eicol;
    const bool is_first_stage = (prev_stage == nullptr);
    const bool is_cross_phase =
        (prev_stage != nullptr && prev_phase != nullptr);
    if (is_first_stage) {
      // Global initial condition – first stage of the first phase only.
      // eini bounds are in LP (scaled) units.
      eicol = lp.add_col({
          .lowb = storage().eini.value_or(stage_emin),
          .uppb = storage().eini.value_or(stage_emax),
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = EiniName,
          .variable_uid = opts.variable_uid,
          .context = stg_ctx,
      });
    } else if (prev_phase == nullptr) {
      // Same phase – the previous stage's efin column serves as eini here
      // (both stages live in the same LP, so the column is shared).
      eicol = efin_col_at(scenario, *prev_stage);
    } else {
      // Cross-phase boundary (gtopt-phase = PLP-stage for SDDP).
      // Create a new "sini" (state-initial) column for this phase's LP.
      // Named "sini" to distinguish from the global "eini" which only exists
      // in the first phase; sini columns are the SDDP inter-phase coupling
      // variables propagated by the forward pass.
      eicol = lp.add_col({
          .lowb = stage_emin,
          .uppb = stage_emax,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = SiniName,
          .variable_uid = opts.variable_uid,
          .context = stg_ctx,
      });
      if (effective_usv && !opts.skip_state_link) {
        // Queue a deferred dependent-variable link to the previous
        // phase's efin StateVariable.  Resolution happens in the
        // per-scene tightening pass after parallel phase build joins
        // (see PlanningLP::tighten_scene_phase_links).  Calling
        // `prev_efin->add_dependent_variable` here directly would race
        // with phase N's add_to_lp under parallel phase construction.
        sc.defer_state_link(
            // NOLINTNEXTLINE(readability-suspicious-call-argument)
            StateVariable::key(scenario, *prev_stage, cname, uid(), EfinName),
            eicol);
      } else if (effective_usv && opts.skip_state_link) {
        SPDLOG_TRACE(
            "StorageLP: skipping state link at phase boundary "
            "(class='{}' uid={}) — eini is independently provisioned "
            "(reset_month or similar).",
            cname,
            static_cast<int>(uid()));
      }
      // If !use_state_variable: sini is free (within emin/emax bounds).
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

    // stg_ctx (StageContext = tuple<ScenarioUid, StageUid>) serves as both
    // the LP hierarchy context and the index holder key.

    auto prev_vc = eicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      // is_last_block identifies the terminal block for the global efin
      // condition.  The eini column feeds into the first block automatically
      // via prev_vc = eicol, so no explicit is_first_block guard is needed.
      const auto is_last_block = is_last_stage && (buid == blocks.back().uid());

      auto erow =
          SparseRow {
              .class_name = cname,
              .constraint_name = EnergyName,
              .variable_uid = opts.variable_uid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .equal(0);

      // Energy LP variable is in scaled units (physical / energy_scale).
      // All blocks use uniform bounds [lp_emin, lp_emax].
      // - Global initial condition (eini) is a separate column created before
      //   this loop, used as prev_vc for the first block (is_first_block).
      // - Global final condition (efin) is a named >= row added below for the
      //   last block (is_last_block) of the last stage.
      const auto ec = lp.add_col({
          .lowb = stage_emin,
          .uppb = stage_emax,
          .cost = stage_ecost,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = EnergyName,
          .variable_uid = opts.variable_uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });

      ecols[buid] = ec;

      erow[prev_vc] = -(1 - (hour_loss * block.duration() * dc_stage_scale));
      erow[ec] = 1;

      // Physical flow coefficients: fcr × duration × dc_stage_scale.
      // flatten() applies col_scale to both flow and energy columns.
      //
      // fout_cols and finp_cols may each be empty.  VolumeRight with
      // source_flow_right passes empty finp (outflow injected later);
      // VolumeRight always passes empty fout (outflow via FlowRight).
      const auto has_fout = fout_cols.contains(buid);
      const auto has_finp = finp_cols.contains(buid);

      if (has_fout) {
        const auto fout_col = fout_cols.at(buid);
        erow[fout_col] = +(flow_conversion_rate / fout_efficiency)
            * block.duration() * dc_stage_scale;

        if (has_finp) {
          const auto finp_col = finp_cols.at(buid);
          // if the input and output are the same, we only need one entry
          if (fout_col != finp_col) {
            erow[finp_col] = -(flow_conversion_rate * finp_efficiency)
                * block.duration() * dc_stage_scale;
          }
        }
      } else if (has_finp) {
        // No fout — finp is a pure inflow (adds to storage volume).
        const auto finp_col = finp_cols.at(buid);
        erow[finp_col] = -(flow_conversion_rate * finp_efficiency)
            * block.duration() * dc_stage_scale;
      }

      if (drain_cost) {
        // Physical drain cost — flatten() applies col_scale.
        const auto dcol = lp.add_col({
            .lowb = 0,
            .uppb = drain_capacity.value_or(LinearProblem::DblMax),
            .cost =
                CostHelper::block_ecost(scenario, stage, block, *drain_cost),
            .scale = flow_scale,
            .class_name = opts.class_name,
            .variable_name = DrainName,
            .variable_uid = opts.variable_uid,
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        });

        dcols[buid] = dcol;
        erow[dcol] = flow_conversion_rate * block.duration() * dc_stage_scale;
      }

      erows[buid] = lp.add_row(std::move(erow));

      // Capacity constraint: capacity_col >= ec (both physical).
      // flatten() applies col_scale to matrix coefficients automatically.
      if (capacity_col) {
        auto crow =
            SparseRow {
                .class_name = cname,
                .constraint_name = CapacityName,
                .variable_uid = opts.variable_uid,
                .context = make_block_context(
                    scenario.uid(), stage.uid(), block.uid()),
            }
                .greater_equal(0);
        crow[*capacity_col] = 1;
        crow[ec] = -1.0;

        crows[buid] = lp.add_row(std::move(crow));
      }

      // Global final condition: for the last block of the last stage only,
      // add a named ">=" constraint row enforcing vol_last >= storage().efin.
      // Counterpart of the global "eini" equality column (first block, first
      // stage):
      //
      //   eini col   (1st phase, 1st stage, 1st block): vol_start  = eini [=]
      //   efin row   (last phase, last stage, last block): vol_end >= efin [>=]
      //
      // Named "efin" so it appears as rsv_efin_uid_scen_stage (or
      // bat_soc_efin_uid_scen_stage) in the LP file.
      const auto& efin_opt = storage().efin;
      if (is_last_block && efin_opt.has_value()) {
        const double lp_efin = *efin_opt;
        auto efin_row =
            SparseRow {
                .class_name = cname,
                .constraint_name = EfinName,
                .variable_uid = opts.variable_uid,
                .context = stg_ctx,
            }
                .greater_equal(lp_efin);
        efin_row[ec] = 1.0;
        efin_rows[stg_ctx] = lp.add_row(std::move(efin_row));
      }

      prev_vc = ec;
    }

    // Soft minimum energy constraint (PLP "holgura" / slack):
    //   efin + slack >= soft_emin
    // The slack variable has a penalty cost in the objective, allowing the
    // volume/SoC to drop below soft_emin at a cost.  One constraint per
    // stage, applied to the efin column (prev_vc = last block's energy col).
    const auto stage_soft_emin = soft_emin.at(stage.uid()).value_or(0.0);
    const auto stage_soft_emin_cost =
        soft_emin_cost.at(stage.uid()).value_or(0.0);
    if (stage_soft_emin > 0.0 && stage_soft_emin_cost > 0.0) {
      const double lp_soft_emin = stage_soft_emin;
      // Penalty cost per LP unit of slack: physical cost.
      // Apply scenario probability and discount via scenario_stage_ecost,
      // then remove the duration factor (state penalty, not flow).
      // flatten() applies col_scale (energy_scale) to the objective.
      const double slack_cost =
          sc.scenario_stage_ecost(scenario, stage, stage_soft_emin_cost)
          / stage.duration();

      const auto semin_col = lp.add_col({
          .lowb = 0,
          .uppb = LinearProblem::DblMax,
          .cost = slack_cost,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = SoftEminName,
          .variable_uid = opts.variable_uid,
          .context = stg_ctx,
      });

      auto semin_row =
          SparseRow {
              .class_name = cname,
              .constraint_name = SeminGeName,
              .variable_uid = opts.variable_uid,
              .context = stg_ctx,
          }
              .greater_equal(lp_soft_emin);
      semin_row[prev_vc] = 1.0;
      semin_row[semin_col] = 1.0;

      soft_emin_rows[stg_ctx] = lp.add_row(std::move(semin_row));
      soft_emin_slack_cols[stg_ctx] = semin_col;
    }

    // Register efin (the last block's energy column) as a StateVariable so
    // that PlanningLP::resolve_scene_phases() and the SDDP solver can
    // discover and propagate the reservoir/battery state across phase
    // boundaries (gtopt-phase = PLP-stage).
    if (effective_usv) {
      // Register the already-added last-block energy column as a state
      // variable (efin); sets is_state=true for SDDP cut I/O.  Column names
      // are available at LpNamesLevel::all, but state variable
      // I/O uses the StateVariable map (ColIndex-based) directly.
      sc.add_state_col(
          lp,
          // NOLINTNEXTLINE(readability-suspicious-call-argument)
          StateVariable::key(scenario, stage, cname, uid(), EfinName),
          prev_vc,
          opts.scost,
          energy_scale,
          stg_ctx);
    } else {
      // No cross-stage/phase state coupling: add efin == eini constraint so
      // that each independent segment (phase or single-stage horizon) is
      // "closed" – i.e., the storage ends at the same energy level it started.
      auto close_row =
          SparseRow {
              .class_name = cname,
              .constraint_name = "eclose",
              .variable_uid = opts.variable_uid,
              .context = stg_ctx,
          }
              .equal(0);
      close_row[prev_vc] = 1;
      close_row[eicol] = -1;
      [[maybe_unused]] const auto close_row_idx =
          lp.add_row(std::move(close_row));
    }

    // Store the dual correction factor for daily-cycle time-scaling.
    // flatten() applies col_scale to coefficients, so energy_scale is
    // already accounted for in the LP matrix — no manual correction needed.
    // When the factor is effectively 1.0, no entry is stored; downstream
    // flat() defaults to 1.0 for absent keys (no correction applied).
    const double dual_scale = dc_stage_scale;
    if (std::abs(dual_scale - 1.0) > std::numeric_limits<double>::epsilon()) {
      output_dual_scale[stg_ctx] = dual_scale;
    }

    // storing the indices for this scenario and stage
    if (is_cross_phase) {
      // Cross-phase SDDP state variable (sini): stored only in sini_cols.
      // eini_col_at() falls back to sini_cols when the key is absent from
      // eini_cols, so no duplicate entry is needed in eini_cols.
      sini_cols[stg_ctx] = eicol;
    } else {
      // Global initial condition (first stage) or same-phase reuse:
      // stored in eini_cols for direct access via eini_col_at().
      eini_cols[stg_ctx] = eicol;
    }
    efin_cols[stg_ctx] = prev_vc;
    energy_rows[stg_ctx] = std::move(erows);
    energy_cols[stg_ctx] = std::move(ecols);
    if (drain_cost) {
      drain_cols[stg_ctx] = std::move(dcols);
    }

    if (!crows.empty()) {
      capacity_rows[stg_ctx] = std::move(crows);
    }

    // ── Central PAMPL variable registration ──────────────────────────
    // Register the generic storage variables (energy/drain/eini/efin/
    // soft_emin) under the caller's canonical AMPL class name.  Callers
    // that pass an empty ampl_class opt out and must register manually.
    if (!ampl_class.empty()) {
      sc.add_ampl_variable(
          ampl_class, uid(), EnergyName, scenario, stage, energy_cols[stg_ctx]);
      if (drain_cost) {
        sc.add_ampl_variable(
            ampl_class, uid(), DrainName, scenario, stage, drain_cols[stg_ctx]);
      }
      sc.add_ampl_variable(ampl_class, uid(), EiniName, scenario, stage, eicol);
      sc.add_ampl_variable(
          ampl_class, uid(), EfinName, scenario, stage, prev_vc);
      if (const auto sit = soft_emin_slack_cols.find(stg_ctx);
          sit != soft_emin_slack_cols.end())
      {
        sc.add_ampl_variable(
            ampl_class, uid(), SoftEminName, scenario, stage, sit->second);
      }
    }

    return true;
  }

  template<typename OutputContext>
  bool add_to_output(OutputContext& out, std::string_view cname) const
  {
    const auto pid = id();

    // Primal and reduced-cost outputs: the LinearInterface now returns
    // physical values from get_col_sol() (LP × col_scale) and
    // get_col_cost() (LP / col_scale), so no manual rescaling needed.
    out.add_col_sol(cname, EiniName, pid, eini_cols);
    out.add_col_cost(cname, EiniName, pid, eini_cols);
    out.add_col_sol(cname, SiniName, pid, sini_cols);
    out.add_col_cost(cname, SiniName, pid, sini_cols);
    out.add_col_sol(cname, EfinName, pid, efin_cols);
    out.add_col_cost(cname, EfinName, pid, efin_cols);
    out.add_col_sol(cname, EnergyName, pid, energy_cols);
    out.add_col_cost(cname, EnergyName, pid, energy_cols);

    // Dual output: output_dual_scale = dc_stage_scale.
    // Row equilibration is already removed by get_row_dual().
    // This corrects the daily-cycle time-scaling (dc_stage_scale).
    // flatten() handles energy_scale via col_scale on coefficients.
    out.add_row_dual(cname, EnergyName, pid, energy_rows, output_dual_scale);

    out.add_row_dual(cname, CapacityName, pid, capacity_rows);
    out.add_row_dual(cname, EfinName, pid, efin_rows);

    out.add_col_sol(cname, SoftEminName, pid, soft_emin_slack_cols);
    out.add_col_cost(cname, SoftEminName, pid, soft_emin_slack_cols);
    out.add_row_dual(cname, SoftEminName, pid, soft_emin_rows);

    out.add_col_sol(cname, DrainName, pid, drain_cols);
    out.add_col_cost(cname, DrainName, pid, drain_cols);

    return true;
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_emin(StageUid s) const { return emin.at(s); }
  [[nodiscard]] auto param_emax(StageUid s) const { return emax.at(s); }
  [[nodiscard]] auto param_ecost(StageUid s) const { return ecost.at(s); }
  /// @}

private:
  OptTRealSched emin;
  OptTRealSched emax;
  OptTRealSched ecost;

  OptTRealSched annual_loss;

  OptTRealSched soft_emin;
  OptTRealSched soft_emin_cost;

  STBIndexHolder<ColIndex> energy_cols;
  STBIndexHolder<ColIndex> drain_cols;
  STBIndexHolder<RowIndex> energy_rows;
  STBIndexHolder<RowIndex> capacity_rows;

  STIndexHolder<ColIndex> eini_cols;  ///< Global initial (first stage) and
                                      ///< same-phase reuse entries; used by
                                      ///< eini_col_at().
  STIndexHolder<ColIndex>
      sini_cols;  ///< Cross-phase SDDP state-initial cols
                  ///< ("sini"); subset of eini_cols entries.
  STIndexHolder<ColIndex> efin_cols;  ///< Last-block energy col per stage;
                                      ///< used by SDDP StateVariable linking.
  STIndexHolder<RowIndex> efin_rows;  ///< Explicit >= efin constraint rows;
                                      ///< only for last stage when efin set.

  STIndexHolder<ColIndex> soft_emin_slack_cols;  ///< Soft emin slack variable
                                                 ///< per (scenario, stage).
  STIndexHolder<RowIndex> soft_emin_rows;  ///< Soft emin >= constraint rows.

  /// Combined dual correction factor per (scenario, stage):
  ///   dual_physical = dual_LP * output_dual_scale[{suid, tuid}]
  /// When a key is absent, downstream flat() defaults to 1.0 (no correction).
  STIndexHolder<double> output_dual_scale;

  /// Energy scale factor cached from the last add_to_lp call.
  /// Equals StorageOptions::energy_scale; used in add_to_output to rescale
  /// primal solution values back to physical units.
  double m_energy_scale_ {1.0};

  /// Flow variable scale factor cached from the last add_to_lp call.
  /// Equals StorageOptions::flow_scale; used in add_to_output to rescale
  /// drain primal solution values back to physical units.
  double m_flow_scale_ {1.0};
};

}  // namespace gtopt
