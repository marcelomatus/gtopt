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
    // Priority chain (revised 2026-05-04 — symmetry between
    // `LowMemoryMode::off` and `LowMemoryMode::compress`):
    //
    //   1. Previous phase's *optimal* efin — the only "live" source.
    //      Cross-phase reads the predecessor's freshly-propagated
    //      reservoir trajectory.  Under `off` this is the live
    //      backend's col_solution; under `compress` it's the cached
    //      col_solution captured at the predecessor's last
    //      `release_backend()`.  Both modes produce the *same* value
    //      because the cache is populated post-solve and never
    //      overwritten by subsequent reconstructs (see
    //      `LinearInterface::release_backend` cache-refresh gate).
    //
    //   2. Hot-start warm value — loaded from a prior-run state file
    //      via `LinearInterface::warm_col_sol()`.  Used when no
    //      predecessor exists (iter-0 first phase) and the current
    //      run is restarting from saved state.
    //
    //   3. Global default `eini` from the JSON — last-resort
    //      fallback.  Iter-0 first phase falls through to here when
    //      no warm-start state file is loaded.
    //
    // The own-phase eini_col fallback was REMOVED to keep `off` and
    // `compress` byte-identical.  Rationale: when a Benders cut is
    // added to our cell mid-backward, the live backend's
    // `is_proven_optimal()` returns false (CPLEX flags the
    // post-add_row LP as no-longer-solved) → step 2 was skipped →
    // step 4 default fired.  Under compress, the cached optimality
    // flag stays `true` from the prior release, so step 2 fired and
    // returned a now-stale `eini_col` that doesn't satisfy the new
    // cut.  The two paths drift: off produces a default-driven LP
    // for `update_lp`, compress produces a stale-cache-driven LP.
    // Removing step 2 makes both modes agree on the cross-phase value
    // (which is the only physically meaningful source anyway — our
    // own phase's `eini_col` is just a copy of the predecessor's
    // efin enforced by the state-link constraint).
    //
    // 1. Cross-phase: previous phase's optimal efin.
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
    // 2. Hot-start warm value (no predecessor or predecessor not optimal).
    const auto& li = sys.linear_interface();
    const auto col = eini_col_at(scenario, stage);
    const auto& warm = li.warm_col_sol();
    if (!warm.empty() && static_cast<size_t>(col) < warm.size()) {
      return physical_col_value(warm, col);
    }
    // 3. Global default eini.
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
      // Physical + optimal-only bound-clamped view (see sister
      // overload above).
      return li.get_col_sol()[col];
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
      // Physical + optimal-only bound-clamped view: scrubs solver-
      // tolerance noise so a propagated efin never lands outside
      // the reservoir's physical envelope.
      return li.get_col_sol()[col];
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
  bool add_to_lp(const LPClassName& cname,
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
          .lowb = sini_lowb,
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

      auto erow =
          SparseRow {
              .class_name = cname,
              .constraint_name = EnergyBalanceName,
              .variable_uid = opts.variable_uid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .equal(0);

      // Energy LP variable is in scaled units (physical / energy_scale).
      // - Global initial condition (eini) is a separate column created before
      //   this loop, used as prev_vc for the first block (is_first_block).
      // - Global final condition (efin) is a named >= row added below for the
      //   last block (is_last_block) of the last stage.
      //
      // PLP-style emin enforcement (revised 2026-04-25): the per-stage `emin`
      // floor is applied to the last-block energy column (which serves
      // as `efin`, the inter-stage handoff state) ONLY when the user opts in
      // via `model_options.strict_storage_emin=true`.  Intra-stage
      // blocks always use `lowb = 0` so the energy-balance row has the same
      // headroom as PLP's `qe<u>_b` Free / `ve<u>` Free formulation.
      //
      // Why default lax: at iter-0 of an SDDP cascade, the forward pass may
      // fix `sini = stage_emin` if the predecessor's trial drove the reservoir
      // to its floor.  Combined with the energy-balance equation
      // `energy_b = sini − fcr·duration·extraction − ...`, a per-block
      // `lowb = stage_emin` leaves zero headroom and forces a primal
      // infeasibility.  PLP's per-stage LP avoids this by enforcing emin only
      // on the future-volume column (`vf<u> ≥ vmin`).  Mirroring that here
      // keeps the LP feasible across the iter-0 cascade.  When
      // `strict_storage_emin=true`, the historic strict bound is
      // restored — `efin_block_lowb = stage_emin` — preserving the inter-
      // stage semantics so the next stage's `sini` is also ≥ stage_emin.
      //
      // Capacity (uppb = stage_emax) is a real physical bound and stays per
      // block.
      const bool emin_block = (buid == blocks.back().uid());
      const auto ec = lp.add_col({
          .lowb = emin_block ? efin_block_lowb : 0.0,
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
              .scale = energy_scale,
              .class_name = opts.class_name,
              .variable_name = EfinSlackName,
              .variable_uid = opts.variable_uid,
              .context = stg_ctx,
          });
          efin_row[efin_slack_col] = 1.0;
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
