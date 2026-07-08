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

#include <span>

#include <gtopt/constraint_names.hpp>
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

  /// Daily energy-throughput cap N (PLEXOS ``Max Cycles Day``).
  /// 0 = off; >0 = emit per-day ``cycle_limit`` rows enforcing
  /// ``Σ_{b∈window} (fcr / fout_eff_b) · duration(b) · dc_stage_scale ·
  /// fout[b] ≤ N · capacity``.  Generic to all storage elements; only the
  /// parameter is element-supplied (Battery passes
  /// ``battery().max_cycles_day``; Reservoir/VolumeRight leave it at 0).
  double max_cycles_day {0.0};

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
  ///
  /// For SAME-PHASE reset stages use `break_stage_chain` instead —
  /// skip_state_link only affects the cross-phase sini creation.
  bool skip_state_link {false};

  /// Break the same-phase stage chain at this (reset) stage: instead of
  /// reusing the previous stage's efin column as this stage's initial
  /// energy, create a FRESH `eini` column that the caller provisions
  /// (bounds pin or a debit/credit row).  Without this, an in-phase
  /// reset would pin the PREVIOUS stage's ending volume — corrupting
  /// the prior balance (the previous efin column and this stage's
  /// initial state are the same LP column in the shared-LP layout).
  /// No effect on the first stage of a phase (cross-phase boundaries
  /// already allocate a fresh sini column).
  bool break_stage_chain {false};

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
  /// Constraint name for the per-block energy-balance row.  Distinct
  /// from `EnergyName` (the variable) so the LP row and column do not
  /// share a base label — without this split, writers like
  /// `CPXwriteprob` emit `#<col_index>` disambiguators on every
  /// column when they detect the row/col name clash, making LP files
  /// much harder to diff.  Keeping `EnergyName` for the variable
  /// preserves the `<class>.energy.*` column convention that
  /// downstream tooling (ScaledView lookup, cut I/O, Pampl readers)
  /// already depends on.
  static constexpr std::string_view EnergyBalanceName {"energy_balance"};
  static constexpr std::string_view SoftEminName {"soft_emin"};
  static constexpr std::string_view DrainName {"drain"};
  static constexpr std::string_view EfinName {"efin"};
  /// Slack variable name for the soft-`efin` slack column (created only
  /// when ``Reservoir.efin_cost`` is set).  Mirrors `SoftEminName` —
  /// the slack relaxes the hard ``vol_end >= efin`` row into
  /// ``vol_end + slack >= efin`` priced at `efin_cost`.
  static constexpr std::string_view EfinSlackName {"efin_slack"};
  static constexpr std::string_view CapacityName {"capacity"};
  static constexpr std::string_view SeminGeName {"semin_ge"};
  /// LP row label for the daily energy-throughput limit
  /// (``StorageOptions::max_cycles_day`` / PLEXOS ``Max Cycles Day``).
  /// Duals surface as ``<class>/cycle_limit_…``.
  static constexpr std::string_view CycleLimitName {"cycle_limit"};

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

  /// Non-throwing lookup of the explicit ``vol_end >= efin`` row for
  /// (scenario, stage).  Returns the row index if present, or
  /// ``std::nullopt`` when ``Storage::efin`` is unset on this element.
  [[nodiscard]] constexpr std::optional<RowIndex> find_efin_row(
      const ScenarioLP& scenario, const StageLP& stage) const noexcept
  {
    const auto it = efin_rows.find({scenario.uid(), stage.uid()});
    if (it == efin_rows.end()) {
      return std::nullopt;
    }
    return it->second;
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

  /// Per-(scenario, stage) factor applied to the energy-balance dual at
  /// Parquet-write time.  Folds two effects: a constant `-1` sign flip
  /// (industry-convention positive water value / marginal storage value)
  /// and the `24/duration` daily-cycle correction when `daily_cycle=true`.
  /// Exposed for regression tests that pin the sign convention.
  [[nodiscard]] constexpr double output_dual_scale_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return output_dual_scale.at({scenario.uid(), stage.uid()});
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

  /// Return the soft-`efin` slack column for (scenario, stage), if it
  /// exists.
  ///
  /// The slack is created only when ``Reservoir.efin`` is set AND
  /// ``Reservoir.efin_cost`` is > 0 AND the stage is the last stage of the
  /// last phase.  Returns `std::nullopt` when the column was not
  /// created (i.e., the hard ``vol_end >= efin`` row is in effect, or no
  /// efin target exists for this storage element).
  [[nodiscard]] std::optional<ColIndex> efin_slack_col_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    const auto key = std::tuple {scenario.uid(), stage.uid()};
    if (const auto it = efin_slack_cols.find(key); it != efin_slack_cols.end())
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

  /// Retrieve the physical eini (initial energy/volume) for a given
  /// scenario and stage.
  ///
  /// For the first stage of the first phase the eini column is the fixed
  /// initial condition, so @p default_eini is returned directly.
  ///
  /// For cross-phase boundaries (phase > 0), eini corresponds to the
  /// previous phase's efin.  When the current LP hasn't been solved, the
  /// method looks up the previous
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
    // Priority chain:
    //   1. Previous phase's optimal efin — the only "live" source.
    //      Cross-phase reads the predecessor's freshly-propagated
    //      reservoir trajectory.  Under `off` this is the live
    //      backend's col_solution; under `compress` it's the cached
    //      col_solution captured at the predecessor's last
    //      `release_backend()`.  Both modes produce the same value
    //      because the cache is populated post-solve and never
    //      overwritten by subsequent reconstructs.
    //
    //   2. Global default `eini` from the JSON — last-resort
    //      fallback.  Iter-0 first phase falls through here.
    if (const auto* prev_sys = sys.prev_phase_sys()) {
      const auto& prev_li = prev_sys->linear_interface();
      if (prev_li.is_optimal()) {
        const auto& prev_rsv =
            prev_sys->template element<typename SIdT::object_type>(sid);
        const auto& prev_stages = prev_sys->phase().stages();
        if (!prev_stages.empty()) {
          return prev_rsv.physical_efin(
              prev_li, scenario, prev_stages.back(), default_eini);
        }
      }
    }
    return default_eini;
  }

  /// Retrieve the physical eini without cross-phase lookup.
  ///
  /// Used by callers that only have a LinearInterface (e.g. tests, non-SDDP
  /// code).  Fallback chain: optimal solution → default_eini.
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
      // Physical + optimal-only bound-clamped view (see sister
      // overload above).
      return li.get_col_sol()[col];
    }
    return default_eini;
  }

  /// Retrieve the physical efin (final energy/volume) for a given
  /// scenario and stage.  Fallback chain:
  ///   1. LP optimal solution
  ///   2. default_efin
  [[nodiscard]] double physical_efin(const LinearInterface& li,
                                     const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     double default_efin) const
  {
    const auto col = efin_col_at(scenario, stage);
    if (li.is_optimal()) {
      // Physical + optimal-only bound-clamped view: scrubs solver-
      // tolerance noise so a propagated efin never lands outside
      // the reservoir's physical envelope.
      return li.get_col_sol()[col];
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

  /// @param finp_efficiency_at  Callable ``(BlockUid) -> double`` returning
  ///        the per-block charge / inflow efficiency.  Pass a lambda
  ///        sampling an ``OptTBRealSched`` for per-block schedules
  ///        (e.g. Battery's input_efficiency since PR-E), or
  ///        ``[](BlockUid){ return 1.0; }`` for elements with no
  ///        efficiency loss (Reservoir / LngTerminal / VolumeRight).
  /// @param fout_efficiency_at  Same shape, for discharge / outflow.
  template<typename SystemContextT, typename FinpEffFn, typename FoutEffFn>
  bool add_to_lp(const LPClassName& cname,
                 std::string_view ampl_class,
                 SystemContextT& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp,
                 const double flow_conversion_rate,
                 const BIndexHolder<ColIndex>& finp_cols,
                 FinpEffFn&& finp_efficiency_at,
                 const BIndexHolder<ColIndex>& fout_cols,
                 FoutEffFn&& fout_efficiency_at,
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

    // Physical objective cost per energy unit.  `ecost` is now
    // per-(stage, block); we resolve it inside the block loop below.
    // The legacy per-stage `stage_ecost` is no longer cached.

    const auto hour_loss =
        annual_loss.at(stage.uid()).value_or(0.0) / hours_per_year;
    const auto stg_ctx = make_stage_context(scenario.uid(), stage.uid());

    // Physical bounds — stored directly in SparseCol; flatten() converts
    // to LP units by dividing by col.scale.  ``emin`` / ``emax`` are
    // ``OptTBRealSched`` (per-(stage, block)); for the eini / sini
    // boundary columns (created BEFORE the block loop) we read the
    // first block's bound, which represents the SoC at the start of
    // the stage.
    const auto [stage_emax, stage_emin] =
        sc.block_maxmin_at(stage, blocks.front(), emax, emin, stage_capacity);

    // PLP-style emin enforcement gate.  When false (default), the per-stage
    // `emin` floor is dropped from the `sini` (cross-phase initial energy)
    // and last-block `efin` columns so the LP can dip below `emin` at iter-0
    // of an SDDP cascade — exactly mirroring PLP, where `ve<u>` is Free and
    // only the future-volume column `vf<u>` carries the `vmin` lower bound.
    // When true, the strict bound is restored on both columns.
    const bool strict_volume = sc.options().strict_storage_emin();
    const double sini_lowb = strict_volume ? stage_emin : 0.0;
    const double efin_block_lowb = strict_volume ? stage_emin : 0.0;

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
          .cost_scale_type = ConstraintScaleType::Energy,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = EiniName,
          .variable_uid = opts.variable_uid,
          .context = stg_ctx,
      });
    } else if (prev_phase == nullptr) {
      if (opts.break_stage_chain) {
        // Reset stage inside a shared LP: decouple from the previous
        // stage's efin — a fresh eini column becomes the chain start
        // and the caller provisions it (bounds pin / debit row).  The
        // previous stage's balance stays untouched.
        eicol = lp.add_col({
            .lowb = 0.0,
            .uppb = stage_emax,
            .cost_scale_type = ConstraintScaleType::Energy,
            .scale = energy_scale,
            .class_name = opts.class_name,
            .variable_name = EiniName,
            .variable_uid = opts.variable_uid,
            .context = stg_ctx,
        });
      } else {
        // Same phase – the previous stage's efin column serves as eini
        // here (both stages live in the same LP, so the column is
        // shared).
        eicol = efin_col_at(scenario, *prev_stage);
      }
    } else {
      // Cross-phase boundary (gtopt-phase = PLP-stage for SDDP).
      // Create a new "sini" (state-initial) column for this phase's LP.
      // Named "sini" to distinguish from the global "eini" which only exists
      // in the first phase; sini columns are the SDDP inter-phase coupling
      // variables propagated by the forward pass.
      eicol = lp.add_col({
          .lowb = sini_lowb,
          .uppb = stage_emax,
          .cost_scale_type = ConstraintScaleType::Energy,
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
            uid());
      }
      // If !use_state_variable: sini is free (within emin/emax bounds).
      // An efin==eini close constraint is added after the block loop below.
    }

    // Drain (spillway) emission gate: skip the per-block drain column when
    // the caller signals the drain is disabled by passing a *zero* capacity.
    // For Reservoir this corresponds to `Reservoir.spillway_capacity == 0`
    // (PLP `IBind`/`SerVer == 0` reservoirs whose spillway is structurally
    // bound to zero — emitting a `[0,0]` column adds dead variables and rows
    // to the energy balance).  When the capacity is unset the column keeps
    // the historical unbounded behaviour (DblMax), so existing callers that
    // pass `drain_capacity = std::nullopt` are unchanged.
    const bool drain_enabled =
        drain_cost.has_value() && drain_capacity.value_or(1.0) > 0.0;

    BIndexHolder<ColIndex> ecols;
    BIndexHolder<ColIndex> dcols;
    BIndexHolder<RowIndex> erows;
    BIndexHolder<RowIndex> crows;
    map_reserve(ecols, blocks.size());
    map_reserve(erows, blocks.size());
    map_reserve(crows, blocks.size());
    if (drain_enabled) {
      map_reserve(dcols, blocks.size());
    }

    // stg_ctx (StageContext = tuple<ScenarioUid, StageUid>) serves as both
    // the LP hierarchy context and the index holder key.

    auto prev_vc = eicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      // is_last_block identifies the terminal block for the global efin
      // condition.  The eini column feeds into the first block automatically
      // via prev_vc = eicol, so no explicit is_first_block guard is needed.
      const auto is_last_block = is_last_stage && (buid == blocks.back().uid());

      // Hoisted once per block: every per-block LP col / row in this
      // loop body shares the same (scenario, stage, block) context.
      // Pre-2026-05-14 the four `SparseRow{...}` / `lp.add_col(...)`
      // sites below re-built it from the three `.uid()` accessors.
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), block.uid());

      auto erow =
          SparseRow {
              // STOCK constraint: its dual is the water value / energy
              // shadow price ($/stored-unit, e.g. $/CMD) — duration-
              // independent, read back WITHOUT the per-block 1/duration term.
              .cost_scale_type = ConstraintScaleType::Energy,
              .class_name = cname,
              .constraint_name = EnergyBalanceName,
              .variable_uid = opts.variable_uid,
              .context = block_ctx,
          }
              .equal(0);

      // Energy LP variable is in scaled units (physical / energy_scale).
      // - Global initial condition (eini) is a separate column created before
      //   this loop, used as prev_vc for the first block (is_first_block).
      // - Global final condition (efin) is a named >= row added below for the
      //   last block (is_last_block) of the last stage.
      //
      // PLP-style emin enforcement (revised 2026-06-19): the per-(stage,
      // block) `emin` floor is bound as the per-block energy column's lower
      // bound — `.lowb = block_emin` — ONLY when the user opts in via
      // `model_options.strict_storage_emin=true`.  When false (the PLP-
      // faithful default for plp2gtopt), intra-stage blocks AND the
      // last-block efin handoff use `lowb = 0` so the energy-balance row has
      // the same headroom as PLP's `qe<u>_b` Free / `ve<u>` Free formulation.
      //
      // Why the `strict_storage_emin` flag IS the feasibility guard: at
      // iter-0 of an SDDP cascade, the forward pass may fix `sini =
      // stage_emin` if the predecessor's trial drove the reservoir to its
      // floor.  Combined with the energy-balance equation
      // `energy_b = sini − fcr·duration·extraction − ...`, a per-block
      // `lowb = stage_emin` leaves zero headroom and forces a primal
      // infeasibility.  PLP's per-stage LP avoids this by enforcing emin only
      // on the future-volume column (`vf<u> ≥ vmin`).  plp2gtopt therefore
      // emits `strict_storage_emin=false`, keeping the LP feasible across the
      // iter-0 cascade.  plexos2gtopt runs monolithic with no Benders cuts
      // clamping `sini`, so the strict default (`true`) is feasible and
      // makes the PLEXOS per-period Min Volume floor bind every block (was
      // previously dropped on intra-stage blocks → storage dipped below emin,
      // e.g. PEHUENCHE 1175 < emin 1234).  When `strict_storage_emin=true`,
      // the last-block efin column ALSO carries `efin_block_lowb =
      // stage_emin`, preserving the inter-stage handoff so the next stage's
      // `sini` is ≥ stage_emin.
      //
      // Capacity (uppb = block_emax) is a real physical bound and is
      // read per-block — ``emin`` / ``emax`` are ``OptTBRealSched`` so
      // per-(stage, block) overrides bind correctly here (UC.jl-style
      // ``Last period maximum level`` etc.).
      const auto [block_emax, block_emin] =
          sc.block_maxmin_at(stage, block, emax, emin, stage_capacity);
      const bool emin_block = (buid == blocks.back().uid());
      // Per-block hard emin floor.  Under the lax default
      // (`strict_storage_emin=false`) this stays 0 for every block (PLP /
      // SDDP iter-0 feasibility).  Under strict mode the last block uses the
      // efin handoff floor (`efin_block_lowb`) and the intra-stage blocks use
      // their own per-(stage, block) `block_emin` schedule value.
      const double block_lowb =
          strict_volume ? (emin_block ? efin_block_lowb : block_emin) : 0.0;
      // Per-(stage, block) ecost (since PR-D).  `scenario_stage_ecost`
      // applies the scenario probability + discount; we divide by
      // stage.duration() so the LP cost stays in $/(physical_unit)
      // matching the legacy stage-scalar formulation.
      const auto block_ecost_val =
          sc.scenario_stage_ecost(
              scenario, stage, ecost.at(stage.uid(), buid).value_or(0.0))
          / stage.duration();
      const auto ec = lp.add_col({
          .lowb = block_lowb,
          .uppb = block_emax,
          .cost = block_ecost_val,
          // STOCK state column: reduced cost is $/stored-unit, read back
          // WITHOUT the per-block 1/duration term (Energy time-basis).
          .cost_scale_type = ConstraintScaleType::Energy,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = EnergyName,
          .variable_uid = opts.variable_uid,
          .context = block_ctx,
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
        const auto fout_eff_b = fout_efficiency_at(buid);
        erow[fout_col] = +(flow_conversion_rate / fout_eff_b) * block.duration()
            * dc_stage_scale;

        if (has_finp) {
          const auto finp_col = finp_cols.at(buid);
          // if the input and output are the same, we only need one entry
          if (fout_col != finp_col) {
            const auto finp_eff_b = finp_efficiency_at(buid);
            erow[finp_col] = -(flow_conversion_rate * finp_eff_b)
                * block.duration() * dc_stage_scale;
          }
        }
      } else if (has_finp) {
        // No fout — finp is a pure inflow (adds to storage volume).
        const auto finp_col = finp_cols.at(buid);
        const auto finp_eff_b = finp_efficiency_at(buid);
        erow[finp_col] = -(flow_conversion_rate * finp_eff_b) * block.duration()
            * dc_stage_scale;
      }

      if (drain_enabled) {
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
            .context = block_ctx,
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
                .context = block_ctx,
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
                // Terminal-volume target `vol_end >= efin` — its dual is a
                // $/stored-unit shadow price (Energy time-basis).
                .cost_scale_type = ConstraintScaleType::Energy,
                .class_name = cname,
                .constraint_name = EfinName,
                .variable_uid = opts.variable_uid,
                .context = stg_ctx,
            }
                .greater_equal(lp_efin);
        efin_row[ec] = 1.0;
        // Soft-`efin` slack: when ``Reservoir.efin_cost`` is set (and
        // > 0), make the hard ``vol_end >= efin`` row soft by adding a
        // slack column priced at `efin_cost`.  The row becomes
        // ``vol_end + slack >= efin`` so the LP can miss the
        // end-of-horizon target at a cost rather than going infeasible
        // when upstream Benders cuts have clamped `sini` below what one
        // stage of inflows can recover.  Mirrors PLP's per-stage
        // rebalse-cost slack on the volume target.  When `efin_cost`
        // is unset / zero, the historical hard `>=` behaviour
        // is preserved.
        const auto& efin_cost_opt = storage().efin_cost;
        if (efin_cost_opt.has_value() && *efin_cost_opt > 0.0) {
          // Penalty cost per LP unit of slack: physical cost.  Apply
          // scenario probability and discount via scenario_stage_ecost,
          // then remove the duration factor (state penalty, not flow).
          // flatten() applies col_scale (energy_scale) to the objective.
          const double slack_cost =
              sc.scenario_stage_ecost(scenario, stage, *efin_cost_opt)
              / stage.duration();
          const auto efin_slack_col = lp.add_col({
              .lowb = 0,
              .uppb = LinearProblem::DblMax,
              .cost = slack_cost,
              // STOCK state penalty: $/stored-unit (Energy time-basis).
              .cost_scale_type = ConstraintScaleType::Energy,
              .scale = energy_scale,
              .class_name = opts.class_name,
              .variable_name = EfinSlackName,
              .variable_uid = opts.variable_uid,
              .context = stg_ctx,
          });
          efin_row[efin_slack_col] = 1.0;
          efin_slack_cols[stg_ctx] = efin_slack_col;
        }
        efin_rows[stg_ctx] = lp.add_row(std::move(efin_row));
      }

      prev_vc = ec;
    }

    // Soft minimum energy constraint (PLP "holgura" / slack):
    //   efin + slack >= soft_emin
    // The slack variable has a penalty cost in the objective, allowing the
    // volume/SoC to drop below soft_emin at a cost.  One constraint per
    // stage, applied to the efin column (prev_vc = last block's energy col).
    // `soft_emin` / `soft_emin_cost` are per-(stage, block) since PR-D;
    // since the constraint itself is stage-scoped (applies to the efin
    // column), we sample the last block — where the efin lives.  Block
    // 0 would also be valid; the last-block choice keeps round-trip
    // semantics with the legacy single-value-per-stage form.
    const auto soft_emin_buid = blocks.back().uid();
    const auto stage_soft_emin =
        soft_emin.at(stage.uid(), soft_emin_buid).value_or(0.0);
    const auto stage_soft_emin_cost =
        soft_emin_cost.at(stage.uid(), soft_emin_buid).value_or(0.0);
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
          // STOCK state penalty: $/stored-unit (Energy time-basis).
          .cost_scale_type = ConstraintScaleType::Energy,
          .scale = energy_scale,
          .class_name = opts.class_name,
          .variable_name = SoftEminName,
          .variable_uid = opts.variable_uid,
          .context = stg_ctx,
      });

      auto semin_row =
          SparseRow {
              // Soft minimum-volume floor — its dual is a $/stored-unit
              // shadow price (Energy time-basis).
              .cost_scale_type = ConstraintScaleType::Energy,
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
              .constraint_name = storage_close_constraint_name,
              .variable_uid = opts.variable_uid,
              .context = stg_ctx,
          }
              .equal(0);
      close_row[prev_vc] = 1;
      close_row[eicol] = -1;
      [[maybe_unused]] const auto close_row_idx =
          lp.add_row(std::move(close_row));
    }

    // Store the per-(scenario, stage) factor applied to the
    // energy-balance dual at write time.  Two effects are folded
    // together:
    //
    //   1. **Sign flip (always −1).**  The energy-balance row is an
    //      equality whose RHS is 0; raising the RHS by +1 is
    //      algebraically equivalent to injecting one extra unit of
    //      stored energy at block `b` (the `+1` slot belongs to
    //      `energy_b`).  In a minimization LP this lowers the
    //      objective, so the raw LP dual π is ≤ 0 whenever stored
    //      energy is valuable.  Industry tools (PLEXOS Shadow
    //      Price, PyPSA `mu_energy_balance`, PLP / PSR-SDDP
    //      "water value", Calliope, GenX, Backbone) publish a
    //      **positive** number for the same quantity.  We flip the
    //      sign at the write boundary so the Parquet column is
    //      directly comparable with those reports.  The flip is
    //      confined to the energy-balance row dual; capacity,
    //      efin, and soft-emin row duals keep their natural LP
    //      signs (those rows are inequalities with orthogonal sign
    //      conventions).
    //
    //   2. **Daily-cycle time-rescale.**  When `daily_cycle=true`,
    //      the LP is built in compressed (per-block) time but the
    //      physical reporting basis is 24 h; multiplying by
    //      `dc_stage_scale = 24 / stage.duration()` converts the
    //      LP-units dual back to physical $/MWh (Battery) or $/m³
    //      (Reservoir).  When daily_cycle is off, `dc_stage_scale`
    //      is 1.0 and only the sign flip applies.
    //
    // flatten() applies col_scale to coefficients, so energy_scale
    // is already accounted for in the LP matrix — no manual
    // correction needed here.
    //
    // The map is populated unconditionally (one entry per (scen,
    // stage) call to `add_to_lp`) so downstream `add_field_st_scaled`
    // always finds the negative factor and never falls back to the
    // absent-key default of 1.0.
    output_dual_scale[stg_ctx] = -dc_stage_scale;

    // Daily energy-throughput limit (PLEXOS `Max Cycles Day`).  HARD
    // constraint, NOT an objective cost:
    //
    //     Σ_{b∈W} (flow_conversion_rate / fout_eff_b) · duration(b)
    //             · dc_stage_scale · fout[b]   ≤   N · capacity
    //
    // The per-block coefficient is IDENTICAL to the SoC-drain term in the
    // energy balance above, so it measures cell-side throughput: the
    // `/fout_eff_b` makes discharge losses count toward a cycle (one full
    // discharge of the usable capacity == one cycle).  Generic to every
    // storage element; only the cap `N` is element-supplied via
    // `opts.max_cycles_day`.  Emitted only when `max_cycles_day > 0` AND the
    // stage capacity is finite.
    if (opts.max_cycles_day > 0.0 && stage_capacity < LinearProblem::DblMax) {
      const double rhs = opts.max_cycles_day * stage_capacity;

      // `cycle_rows` is an STBIndexHolder = (scenario, stage) → per-block
      // map.  We collect each window's row keyed by the window's first block
      // (one entry per window) and store the per-stage map once below.
      BIndexHolder<RowIndex> cyc_rows;

      // Emit one `cycle_limit` row per window, keyed by the window's first
      // block.  Daily-cycle: one window spanning ALL blocks of the stage
      // (the stage already represents one rescaled day).  Chronological:
      // rolling 24 h windows walked in chronological block order — each
      // consecutive 24 h span is one clean per-day group (blocks never
      // straddle a day boundary), and a stage ≤ 24 h yields a single window.
      const auto emit_window =
          [&](BlockUid first_buid, auto block_first, auto block_last)
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = CycleLimitName,
                .variable_uid = opts.variable_uid,
                .context =
                    make_block_context(scenario.uid(), stage.uid(), first_buid),
            }
                .less_equal(rhs);
        for (auto it = block_first; it != block_last; ++it) {
          const auto buid = it->uid();
          if (!fout_cols.contains(buid)) {
            continue;
          }
          const auto fout_col = fout_cols.at(buid);
          const auto fout_eff_b = fout_efficiency_at(buid);
          row[fout_col] = (flow_conversion_rate / fout_eff_b) * it->duration()
              * dc_stage_scale;
        }
        cyc_rows[first_buid] = lp.add_row(std::move(row));
      };

      if (use_daily_cycle) {
        // One row summing all blocks of the stage.
        emit_window(blocks.front().uid(), blocks.begin(), blocks.end());
      } else {
        // Rolling 24 h windows in chronological order.
        auto win_begin = blocks.begin();
        double win_hours = 0.0;
        for (auto it = blocks.begin(); it != blocks.end(); ++it) {
          win_hours += it->duration();
          const auto next = std::next(it);
          // Close the window once it spans (at least) 24 h, or at the end.
          if (win_hours >= 24.0 || next == blocks.end()) {
            emit_window(win_begin->uid(), win_begin, next);
            win_begin = next;
            win_hours = 0.0;
          }
        }
      }

      cycle_rows[stg_ctx] = std::move(cyc_rows);
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
    if (drain_enabled) {
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
      if (drain_enabled) {
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
  bool add_to_output(OutputContext& out,
                     std::string_view cname,
                     std::string_view energy_dual_name = EnergyName) const
  {
    const auto pid = id();

    // Primal and reduced-cost outputs: the LinearInterface now returns
    // physical values from get_col_sol() (LP × col_scale) and
    // get_col_cost() (LP / col_scale), so no manual rescaling needed.
    // All energy/SoC columns are STOCK state ($/stored-unit): their
    // `cost_scale_type` is set to Energy at add_to_lp, so OutputContext
    // reads their reduced costs back with the duration-free factor,
    // consistent with the energy-balance dual below.
    out.add_col_sol(cname, EiniName, pid, eini_cols);
    out.add_col_cost(cname, EiniName, pid, eini_cols);
    out.add_col_sol(cname, SiniName, pid, sini_cols);
    out.add_col_cost(cname, SiniName, pid, sini_cols);
    out.add_col_sol(cname, EfinName, pid, efin_cols);
    out.add_col_cost(cname, EfinName, pid, efin_cols);
    out.add_col_sol(cname, EnergyName, pid, energy_cols);
    out.add_col_cost(cname, EnergyName, pid, energy_cols);

    // Energy-balance dual.  `output_dual_scale = -dc_stage_scale`
    // (sign-flipped at `add_to_lp` time so the Parquet column matches
    // PLEXOS / PyPSA / PLP / PSR-SDDP sign conventions).  Row
    // equilibration is already removed by get_row_dual(); flatten()
    // handles energy_scale via col_scale on coefficients.  The
    // `energy_dual_name` argument lets subclasses publish the column
    // under a domain-specific name (e.g. ReservoirLP overrides it
    // with `"water_value"`) while keeping the LP variable / row
    // names — and thus `<class>.energy.sol` / `<class>.energy.cost` —
    // untouched.
    // The energy balance is a STOCK constraint (stored-unit, e.g. CMD), so its
    // dual (water value / energy shadow price) must be back-scaled WITHOUT the
    // block-duration term — otherwise multi-hour blocks report slope/duration
    // instead of the true per-stored-unit value.  The `energy_rows` carry
    // `cost_scale_type = Energy` (set at add_to_lp), so OutputContext selects
    // the 1/(prob*discount) factor.  (Per-block power duals like LMP keep the
    // duration term via the Power default.)
    out.add_row_dual(
        cname, energy_dual_name, pid, energy_rows, output_dual_scale);

    out.add_row_dual(cname, CapacityName, pid, capacity_rows);
    out.add_row_dual(cname, EfinName, pid, efin_rows);

    out.add_col_sol(cname, SoftEminName, pid, soft_emin_slack_cols);
    out.add_col_cost(cname, SoftEminName, pid, soft_emin_slack_cols);
    out.add_row_dual(cname, SoftEminName, pid, soft_emin_rows);

    out.add_col_sol(cname, DrainName, pid, drain_cols);
    out.add_col_cost(cname, DrainName, pid, drain_cols);

    // Daily energy-throughput limit duals (<class>/cycle_limit_…).  Empty
    // map when max_cycles_day is off, so no columns are emitted.
    out.add_row_dual(cname, CycleLimitName, pid, cycle_rows);

    return true;
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_emin(StageUid s, BlockUid b) const
  {
    return emin.at(s, b);
  }
  [[nodiscard]] auto param_emax(StageUid s, BlockUid b) const
  {
    return emax.at(s, b);
  }
  [[nodiscard]] auto param_ecost(StageUid s, BlockUid b) const
  {
    return ecost.at(s, b);
  }
  /// @}

private:
  OptTBRealSched emin;  ///< Per-(stage, block) min SoC.  Sources from
                        ///< the Element's ``emin`` field (Battery /
                        ///< Reservoir / VolumeRight / LngTerminal —
                        ///< all are ``OptTBRealFieldSched`` since
                        ///< 2026-05-18).
  OptTBRealSched emax;  ///< Per-(stage, block) max SoC.  Same source
                        ///< contract as ``emin``.
  OptTBRealSched ecost;  ///< Per-(stage, block) holding cost.

  OptTRealSched annual_loss;

  OptTBRealSched soft_emin;  ///< Per-(stage, block) soft-emin floor.
  OptTBRealSched soft_emin_cost;  ///< Per-(stage, block) soft-emin
                                  ///< penalty cost.

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
  STIndexHolder<ColIndex>
      efin_slack_cols;  ///< Soft-`efin` slack variable per
                        ///< (scenario, stage); only present when
                        ///< ``Reservoir.efin_cost`` is > 0.

  STIndexHolder<ColIndex> soft_emin_slack_cols;  ///< Soft emin slack variable
                                                 ///< per (scenario, stage).
  STIndexHolder<RowIndex> soft_emin_rows;  ///< Soft emin >= constraint rows.

  /// Daily energy-throughput limit rows (``CycleLimitName``).  Each row is
  /// keyed by the first block of its window: in PLP daily-cycle mode there
  /// is exactly one row per (scenario, stage) keyed by the stage's first
  /// block; in chronological mode there is one row per rolling 24 h window
  /// keyed by that window's first block.  Empty when
  /// ``StorageOptions::max_cycles_day`` is unset/zero or the stage capacity
  /// is unbounded.
  STBIndexHolder<RowIndex> cycle_rows;

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
