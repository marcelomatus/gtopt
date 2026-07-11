/**
 * @file      planning_options_lp.hpp
 * @brief     Wrapper for PlanningOptions with default value handling
 * @date      Tue Apr 22 03:21:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the PlanningOptionsLP class, which wraps the
 * PlanningOptions structure and provides accessors with default values for
 * optional parameters. This allows consistent and easy access to option values
 * throughout the linear programming (LP) optimization processes.
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/bus.hpp>
#include <gtopt/constraint_names.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/line_enums.hpp>
#include <gtopt/phase_range_set.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/variable_scale.hpp>

namespace gtopt
{

/**
 * @brief Wrapper for PlanningOptions with default value handling
 *
 * PlanningOptionsLP wraps a PlanningOptions structure and provides accessor
 * methods that automatically apply default values when options are not
 * explicitly set. This ensures consistent behavior throughout the optimization
 * code by defining system-wide defaults in one place.
 */
class PlanningOptionsLP
{
public:
  // Default values for input settings
  /** @brief Default input directory path */
  static constexpr auto default_input_directory = "input";
  /** @brief Default input file format */
  static constexpr DataFormat default_input_format = DataFormat::parquet;

  // Default values for optimization parameters
  /** @brief Default setting for line loss modeling (deprecated) */
  static constexpr Bool default_use_line_losses = true;
  /** @brief Default line losses mode */
  static constexpr LineLossesMode default_line_losses_mode =
      LineLossesMode::adaptive;
  /** @brief Default number of flow segments per line (1 = linear model,
   * &gt;1 = piecewise-linear quadratic approximation of P_loss = R·f²/V²) */
  static constexpr Int default_loss_segments = 1;
  /** @brief Default setting for Kirchhoff constraints */
  static constexpr Bool default_use_kirchhoff = true;
  /** @brief Default Kirchhoff Voltage Law formulation.
   *
   *  ``cycle_basis`` (loop-flow form, PyPSA-style) is the default
   *  because for meshed grids it produces a strictly smaller LP than
   *  ``node_angle`` (no per-bus theta column, fewer KVL rows: one per
   *  fundamental cycle instead of one per line — typically
   *  ``|L| − |B| + #islands`` rows vs ``|L|``).  On juan-scale cases
   *  this is ~40 % smaller per block.  Switch to ``node_angle``
   *  explicitly when the case needs per-stage topology changes or
   *  phase-shift transformers (which require the bus-angle form).
   */
  static constexpr KirchhoffMode default_kirchhoff_mode =
      KirchhoffMode::cycle_basis;
  /** @brief Default setting for single-bus modeling */
  static constexpr Bool default_use_single_bus = false;
  /** @brief Default setting for strict per-stage volume floor (`emin`)
   *  enforcement on `reservoir_sini` and the last-block `efin` column.
   *  `true` (default): both columns get `lowb = stage_emin`, so the per-stage
   *  volume floor is a HARD constraint at the inter-stage handoff state.
   *  Opt out with `model_options.strict_storage_emin = false` for the PLP-
   *  style relaxation (`lowb = 0`) when iter-0 of an SDDP cascade needs the
   *  floor relaxed to stay feasible. */
  static constexpr Bool default_strict_storage_emin = true;
  /** @brief Default threshold for Kirchhoff constraints */
  static constexpr Real default_kirchhoff_threshold = 0;
  /** @brief Default per-unit reactance floor for DC-line auto-promotion.
   *
   *  Lines whose effective per-unit susceptance term `|x_pu| = |X/V²|`
   *  falls below this value are rewritten to `X = 0` by
   *  `PlanningLP::validate_line_reactance` and skipped by the Kirchhoff
   *  assembler — eliminating outlier `x_τ` coefficients that blow up
   *  CPLEX/HiGHS kappa.
   *
   *  `1e-6` is conservative: real transmission `x_pu ≥ 1e-3` (4 OOM
   *  above), real distribution `x_pu ≥ 1e-4` (2 OOM above), and real EHV
   *  lines in physical units (e.g. 220 kV, X=0.1 Ω → `x_pu ≈ 2e-6`) are
   *  never falsely promoted — a 1e-5 floor WOULD wrongly catch them.
   *  V-vs-kV unit-typo lines (`x_pu ≈ 1e-7…1e-9`) and HVDC/phase-shifters
   *  (`X = 0`) are caught.
   *  Set to `0.0` via `--set model_options.dc_line_reactance_threshold=0`
   *  to disable promotion entirely. */
  static constexpr Real default_dc_line_reactance_threshold = 1e-6;
  /** @brief Default per-unit resistance floor for lossless auto-promotion.
   *
   *  Lines whose per-unit loss term `|R/V²|` falls below this value are
   *  rewritten to `R = 0` by `PlanningLP::validate_line_resistance` and
   *  skipped by the loss assembler — eliminating the outlier `line_loss*_link`
   *  coefficients (`∝ 1/(R/V²)`) that very-low-R lines inject as the largest
   *  matrix entries and that blow up `--no-scale` CPLEX/HiGHS kappa.
   *
   *  The loss-link coefficient is driven by `R/V²` (X is irrelevant), so the
   *  dimensionally-correct test is `|R/V²|`, not `|R|` or the `R·X` product
   *  (which misses low-R / normal-X lines).  The threshold is `1e-7` — one
   *  decade BELOW the reactance floor (`1e-6`), because real lines run lower in
   *  `R/V²` than in `X/V²` (EHV lines have `R ≪ X`): a real 220 kV line with
   *  `R = 0.01 Ω` sits at `R/V² = 2e-7`, so a `1e-6` resistance floor would
   *  wrongly strip its losses, whereas `1e-7` spares it while still catching
   *  V-vs-kV unit-typo / busbar lines (e.g. `R/V² ≈ 5e-8`).  Set to `0.0` via
   *  `--set model_options.dc_line_resistance_threshold=0` to disable. */
  static constexpr Real default_dc_line_resistance_threshold = 1e-7;
  /** @brief Default voltage-angle bound (`θ ∈ [−2π, +2π]`) used as
   *  fallback when `auto_scale_theta` did not run (e.g.
   *  `auto_scale=false`).  When auto-scaling is enabled the bound is
   *  replaced by `Σ_l tmax_l · x_τ_l` so flows can reach their full
   *  capacity. */
  static constexpr Real default_theta_max = 2 * std::numbers::pi;
  /** @brief Default objective function scaling factor for MONOLITHIC method.
   *
   * For SDDP / cascade methods, ``scale_objective()`` returns ``1.0``
   * unconditionally (see accessor doc).  This constant is the default
   * for the legacy monolithic LP path only — kept at ``1000`` so the
   * (large) set of monolithic-MIP unit-test fixtures continue to read
   * obj values in LP-space units as historically.
   *
   * Why SDDP defaults to 1.0: 2026-05 benchmark on juan/IPLP showed
   * ``scale_objective=1000`` is the dominant numerical-conditioning
   * contributor to basis kappa.  Measured at iter 15 of a 16-scene,
   * 51-phase, 7,431-cut hot-start:
   *
   *   - ``scale_objective=1000`` (old default) : kappa = 1.27e+08
   *   - ``scale_objective=1``    (new SDDP)    : kappa = 3.97e+07 (3.2× lower)
   *   - All layers off (``--no-scale``)        : kappa = 2.01e+07 (6.3× lower)
   *
   * Solution quality (gap, LB, UB) is bit-identical across all
   * scaling configurations.  The 1000× division of obj coefficients
   * was intended to normalise cost magnitudes from $/MWh into LP-
   * friendly range, but together with per-element ``auto_scale`` and
   * Ruiz equilibration it widens the matrix dynamic range, hurting
   * basis-condition.
   *
   * See also: ``equilibration_method()`` (defaults to ``none`` for
   * single-bus; ``ruiz`` only for multi-bus Kirchhoff).
   */
  static constexpr Real default_scale_objective = 1'000;

  // Default values for output settings
  /** @brief Default output directory path */
  static constexpr auto default_output_directory = "output";
  /** @brief Default output file format */
  static constexpr DataFormat default_output_format = DataFormat::parquet;
  /** @brief Default compression codec for Parquet output files.
   *
   *  `zstd` since 2026-05-19 — empirically the best ratio × decode-speed
   *  combo for the dense long-form value column produced by the long-only
   *  output writer.  On the 2-year case `zstd + long + BYTE_STREAM_SPLIT +
   *  round8` lands at ~298 MB vs ~1 GB for the legacy `snappy + wide`
   *  combination.  All modern parquet readers
   *  (Arrow ≥ 1.8, pandas, polars, duckdb, R `arrow`) read it natively.
   *
   *  Use `output_compression: snappy` when faster encode-side latency
   *  matters more than archival ratio, or `output_compression: lz4`
   *  when downstream consumers prefer the LZ4 frame format.
   *
   *  History: between 2026-05-18 and 2026-05-19 the default was
   *  briefly flipped to `lz4`, but `resolve_parquet_codec` was missing
   *  the `lz4` mapping — every column was silently written
   *  UNCOMPRESSED.  The codec table was fixed; default flipped from
   *  `snappy` to `zstd` once long-form + BYTE_STREAM_SPLIT made the
   *  ratio win unambiguous.
   *
   *  `lp_compression` (LP debug files) keeps `zstd` as its default —
   *  those files are large textual dumps where the ratio matters and
   *  decode speed does not.
   */
  static constexpr CompressionCodec default_output_compression =
      CompressionCodec::zstd;
  /** @brief Default setting for using UIDs in filenames */
  static constexpr Bool default_use_uid_fname = true;
  /** @brief Default annual discount rate for multi-year planning */
  static constexpr Real default_annual_discount_rate = 0.0;

  // ── Fallback helpers ─────────────────────────────────────────────────
  // Three-tier resolution: flat → model_options → compiled default.

  template<typename T>
  [[nodiscard]] static constexpr auto fallback_3(const std::optional<T>& flat,
                                                 const std::optional<T>& model,
                                                 const T& def) noexcept -> T
  {
    if (flat.has_value()) {
      return *flat;
    }
    return model.value_or(def);
  }

  /**
   * @brief Constructs a PlanningOptionsLP wrapper around a PlanningOptions
   * object
   * @param poptions The PlanningOptions object to wrap (defaults to empty)
   */
  explicit PlanningOptionsLP(PlanningOptions poptions = {})
      : m_options_(std::move(poptions))
      , m_variable_scale_map_(populate_variable_scales(m_options_))
  {
  }

  /**
   * @brief Gets the input directory path, using default if not set
   * @return The input directory path
   */
  [[nodiscard]] constexpr auto input_directory() const
  {
    return m_options_.input_directory.value_or(default_input_directory);
  }

  /**
   * @brief Gets the input file format, using default if not set
   * @return The input file format as a string name
   */
  [[nodiscard]] auto input_format() const -> std::string_view
  {
    return enum_name(m_options_.input_format.value_or(default_input_format));
  }

  /**
   * @brief Gets the input file format as a typed enum
   * @return The input DataFormat enum value
   */
  [[nodiscard]] constexpr auto input_format_enum() const -> DataFormat
  {
    return m_options_.input_format.value_or(default_input_format);
  }

  /// @brief Gets the demand failure cost from model_options.
  [[nodiscard]] constexpr auto demand_fail_cost() const
  {
    return m_options_.model_options.demand_fail_cost;
  }

  /// @brief True when ``model_options.objective_mode = "emissions"`` —
  /// the LP should minimize Σ (emission_rate × generation) instead of
  /// dispatch cost.  See ``ModelOptions::objective_mode`` (issue #519).
  [[nodiscard]] constexpr bool is_emissions_objective() const
  {
    const auto& m = m_options_.model_options.objective_mode;
    if (!m.has_value()) {
      return false;
    }
    const auto& s = *m;
    return s == "emissions" || s == "emission";
  }

  /// @brief Gets the hydro spill cost from model_options.
  /// Renamed from `hydro_fail_cost` per §11.10; legacy spelling
  /// still accepted as a JSON alias via the naming-dialects
  /// registry.
  ///
  /// **UNIT**: returned in `[$/m³]` (per-volume), matching the
  /// documentation on `ModelOptions::hydro_spill_cost`.  Callers
  /// pricing a flow column `[m³/s]` via `CostHelper::block_ecost(...)`
  /// (which multiplies by block duration `[h]`) MUST lift the value
  /// by `× 3600` to convert to `[$/(m³/s)/h]` — see
  /// `flow_right_lp.cpp:396` for the reference pattern.  Direct
  /// volume-based costs (`$/m³ × m³`) need no conversion.
  [[nodiscard]] constexpr auto hydro_spill_cost() const
  {
    return m_options_.model_options.hydro_spill_cost;
  }

  /// @brief Gets the hydro use value (benefit per m³) from model_options.
  ///
  /// **UNIT**: returned in `[$/m³]` (per-volume, like
  /// `hydro_spill_cost`).  Same `× 3600` lift required when used as
  /// a fallback for a flow-column benefit `[$/(m³/s)/h]` — see
  /// `flow_right_lp.cpp:471` and the parallel comment on
  /// `hydro_spill_cost()` above.
  [[nodiscard]] constexpr auto hydro_use_value() const
  {
    return m_options_.model_options.hydro_use_value;
  }

  /// @brief Gets the reserve shortage cost from model_options.
  /// Renamed from `reserve_fail_cost` per §11.10; legacy spelling
  /// still accepted as a JSON alias via the naming-dialects registry.
  [[nodiscard]] constexpr auto reserve_shortage_cost() const
  {
    return m_options_.model_options.reserve_shortage_cost;
  }

  /// @brief Gets the state failure cost from model_options [$/MWh].
  [[nodiscard]] constexpr auto state_violation_cost() const
  {
    return m_options_.model_options.state_violation_cost;
  }

  /// @brief Whether a given phase should use continuous LP relaxation.
  /// Parses the `continuous_phases` string from model_options using
  /// PhaseRangeSet.
  [[nodiscard]] bool is_phase_continuous(int phase_index) const
  {
    const auto& cp = m_options_.model_options.continuous_phases;
    if (!cp.has_value()) {
      return false;
    }
    return PhaseRangeSet(*cp).contains(phase_index);
  }

  /// @brief Gets the line losses mode, with backward-compat fallback.
  ///
  /// Priority: model_options.line_losses_mode (string) →
  ///           model_options.use_line_losses (bool, deprecated) →
  ///           default_line_losses_mode (adaptive).
  [[nodiscard]] constexpr LineLossesMode line_losses_mode() const
  {
    if (m_options_.model_options.line_losses_mode.has_value()) {
      return enum_from_name<LineLossesMode>(
                 *m_options_.model_options.line_losses_mode)
          .value_or(default_line_losses_mode);
    }
    if (m_options_.model_options.use_line_losses.has_value()) {
      return *m_options_.model_options.use_line_losses
          ? default_line_losses_mode
          : LineLossesMode::none;
    }
    return default_line_losses_mode;
  }

  /// @brief Gets the line loss modeling flag from model_options.
  /// @deprecated Use line_losses_mode() instead.
  [[nodiscard]] constexpr auto use_line_losses() const
  {
    return line_losses_mode() != LineLossesMode::none;
  }

  /// @brief Gets the number of piecewise-linear loss segments.
  [[nodiscard]] constexpr auto loss_segments() const
  {
    return m_options_.model_options.loss_segments.value_or(
        default_loss_segments);
  }

  /// @brief Gets the Kirchhoff constraints flag.
  [[nodiscard]] constexpr auto use_kirchhoff() const
  {
    return m_options_.model_options.use_kirchhoff.value_or(
        default_use_kirchhoff);
  }

  /// @brief Gets the Kirchhoff Voltage Law formulation.
  ///
  /// Priority: model_options.kirchhoff_mode (string) →
  ///           default_kirchhoff_mode (node_angle).
  /// An unrecognised string falls back to the default.
  [[nodiscard]] constexpr KirchhoffMode kirchhoff_mode() const
  {
    if (m_options_.model_options.kirchhoff_mode.has_value()) {
      return enum_from_name<KirchhoffMode>(
                 *m_options_.model_options.kirchhoff_mode)
          .value_or(default_kirchhoff_mode);
    }
    return default_kirchhoff_mode;
  }

  /// @brief Gets the single-bus modeling flag.
  [[nodiscard]] constexpr auto use_single_bus() const
  {
    return m_options_.model_options.use_single_bus.value_or(
        default_use_single_bus);
  }

  /// @brief Whether to enforce the per-stage `emin` floor as a HARD lower
  /// bound on the storage `sini` and last-block `efin` columns.
  ///
  /// When `true` (default), both columns get `lowb = stage_emin` — the
  /// strictest per-stage volume-constraint enforcement.  Set to `false` for
  /// the PLP-style relaxation (`lowb = 0`) when the LP needs to dip below
  /// the floor at iter-0 of an SDDP cascade without becoming infeasible.
  [[nodiscard]] constexpr auto strict_storage_emin() const
  {
    return m_options_.model_options.strict_storage_emin.value_or(
        default_strict_storage_emin);
  }

  /// Demand-failure substitution with RHS shift (Option C — renamed
  /// from `demand_option_c` per §11.10 of
  /// `docs/analysis/naming-conventions.md`).  Default false
  /// (Option A: obj_constant baseline).  See
  /// `ModelOptions::demand_fail_rhs_shift` for the full rationale.
  [[nodiscard]] constexpr bool demand_fail_rhs_shift() const
  {
    return m_options_.model_options.demand_fail_rhs_shift.value_or(false);
  }

  /// @brief Whether SOURCE elements eliminate their provably-zero LP
  /// columns/rows.  Default `false` — the un-reduced LP is the honest
  /// model, and modern solver presolve (CPLEX/Gurobi) reduces it to the
  /// same core (often marginally faster).  Enable with `--lp-reduction`
  /// (or `model_options.lp_reduction = true`) for weak-presolve backends
  /// (CLP / CBC / HiGHS) that benefit from the smaller LP up front.
  /// See `ModelOptions::lp_reduction`.
  [[nodiscard]] constexpr bool lp_reduction() const
  {
    return m_options_.model_options.lp_reduction.value_or(false);
  }

  /// @brief Gets the objective function scaling factor.
  ///
  /// Default depends on planning method:
  ///   - SDDP / cascade : ``1.0``  (numerical-conditioning win per
  ///     the juan/IPLP benchmark — see ``default_scale_objective`` doc).
  ///   - Monolithic     : ``1000`` (legacy default; preserves existing
  ///     test fixtures and single-shot LP behaviour).
  ///
  /// User-supplied ``model_options.scale_objective`` always takes
  /// precedence.
  [[nodiscard]] constexpr auto scale_objective() const
  {
    if (m_options_.model_options.scale_objective.has_value()) {
      return *m_options_.model_options.scale_objective;
    }
    return method_type_enum() == MethodType::monolithic
        ? default_scale_objective
        : Real {1.0};
  }

  /// @brief Gets the Kirchhoff threshold.
  [[nodiscard]] constexpr auto kirchhoff_threshold() const
  {
    return m_options_.model_options.kirchhoff_threshold.value_or(
        default_kirchhoff_threshold);
  }

  /// @brief Gets the per-unit reactance floor for DC-line auto-promotion.
  ///
  /// Returns the resolved threshold used by
  /// `PlanningLP::validate_line_reactance` when deciding whether a
  /// line's `|X/V²|` is small enough to warrant rewriting `X` to `0`
  /// (and thus dropping the line out of the Kirchhoff matrix).
  [[nodiscard]] constexpr auto dc_line_reactance_threshold() const
  {
    return m_options_.model_options.dc_line_reactance_threshold.value_or(
        default_dc_line_reactance_threshold);
  }

  /// Returns the resolved threshold used by
  /// `PlanningLP::validate_line_resistance` when deciding whether a
  /// line's `|R/V²|` is small enough to warrant rewriting `R` to `0`
  /// (and thus dropping the line's quadratic loss model).
  [[nodiscard]] constexpr auto dc_line_resistance_threshold() const
  {
    return m_options_.model_options.dc_line_resistance_threshold.value_or(
        default_dc_line_resistance_threshold);
  }

  /// @brief Gets the voltage angle scaling factor.
  [[nodiscard]] constexpr auto scale_theta() const
  {
    return m_options_.model_options.scale_theta.value_or(1.0);
  }

  /// @brief Gets the row-scale factor for the line-loss linking row.
  ///
  /// Returns the resolved factor used by `line_losses` to multiply both
  /// `lossrow[loss_col]` and `lossrow[seg_col]` so the smallest segment
  /// coefficient `seg_width · R · 1 / V²` is lifted into a numerically
  /// tractable range.  When `auto_scale=true` (default) and the option is
  /// unset, `PlanningLP::auto_scale_loss_link` computes a power-of-10
  /// multiplier from `median(R/V²)`; otherwise the fallback is `1.0`
  /// (no scaling).
  [[nodiscard]] constexpr auto scale_loss_link() const
  {
    return m_options_.model_options.scale_loss_link.value_or(1.0);
  }

  /// @brief Gets the per-MWh cost on the per-direction loss columns.
  ///
  /// Returns the resolved global ``loss_cost_eps`` ($/MWh) applied to
  /// every PWL/bidirectional line's ``loss_p``/``loss_n`` column to
  /// strictly break the LP-relax bidirectional-flow degeneracy.  When
  /// unset, returns ``0.0`` (legacy behaviour — no epsilon).  Per-line
  /// ``Line.loss_cost_eps`` overrides this value when present.
  [[nodiscard]] constexpr auto loss_cost_eps() const
  {
    return m_options_.model_options.loss_cost_eps.value_or(0.0);
  }

  /// @brief Gets the global default for ``Line.loss_secant_segments``
  /// (issue #504 L-secant chord — number of segment columns
  /// per (line, block) emitted by ``tangent_signed_flow`` when the
  /// SOS2 L-secant chord is active).  Per-line override beats this
  /// value; ``1`` (single-secant chord) preserves pre-#504 behaviour.
  [[nodiscard]] constexpr auto loss_secant_segments() const
  {
    return m_options_.model_options.loss_secant_segments.value_or(1);
  }

  /// @brief Gets the global default for ``Line.loss_use_sos2``
  /// (issue #504 — SOS2 enforcement on the L-secant segment columns).
  /// Per-line override beats this value; ``false`` preserves pre-#504
  /// behaviour (no SOS2 declared even when ``loss_secant_segments > 1``).
  [[nodiscard]] constexpr auto loss_use_sos2() const
  {
    return m_options_.model_options.loss_use_sos2.value_or(false);
  }

  /// @brief Gets the bound for voltage-angle variables (`θ ∈
  /// [−theta_max, +theta_max]`).
  ///
  /// Priority: model_options.theta_max (explicit) →
  /// `default_theta_max` (`2π`).  `PlanningLP::auto_scale_theta`
  /// computes a topology-aware override (`Σ_l tmax_l · x_τ_l`) and
  /// stores it on `model_options.theta_max` before LP build, so the
  /// `2π` fallback only applies under `auto_scale=false`.  Avoids the
  /// historical `±2π` artifact that secretly capped flows below
  /// `tmax` whenever `tmax · x_τ > 2π` (e.g., on IEEE 9-bus where
  /// `250 MW · 0.0576 = 14.4 rad ≫ 2π`).
  [[nodiscard]] constexpr auto theta_max() const
  {
    return m_options_.model_options.theta_max.value_or(default_theta_max);
  }

  /**
   * @brief Gets the UID filename usage flag, using default if not set
   * @return Whether to use UIDs in filenames
   */
  [[nodiscard]] constexpr auto use_uid_fname() const
  {
    return m_options_.use_uid_fname.value_or(default_use_uid_fname);
  }

  /**
   * @brief Gets the output directory path, using default if not set
   * @return The output directory path
   */
  [[nodiscard]] constexpr auto output_directory() const
  {
    return m_options_.output_directory.value_or(default_output_directory);
  }

  /**
   * @brief Gets the output file format as a string name
   * @return The output file format name
   */
  [[nodiscard]] auto output_format() const -> std::string_view
  {
    return enum_name(m_options_.output_format.value_or(default_output_format));
  }

  /**
   * @brief Gets the output file format as a typed enum
   * @return The output DataFormat enum value
   */
  [[nodiscard]] constexpr auto output_format_enum() const -> DataFormat
  {
    return m_options_.output_format.value_or(default_output_format);
  }

  /// Solution output is long-only — a 6-column non-zero-only shape
  /// `(scenario, stage, block, uid, value)` with `uint16_t` keys and a
  /// `BYTE_STREAM_SPLIT`-encoded value column.  Combined with the matching
  /// `zstd` compression default, this produces a 3-7× smaller on-disk
  /// footprint on the typical 0.1 %-dense gtopt output (e.g. 1 GB → 298 MB on
  /// the 2-year case).  The legacy wide (`uid:N` column-per-element) layout
  /// was removed; convert long files to wide externally if ever needed, e.g.
  /// `df.pivot_table(index=["scenario","stage","block"], columns="uid",
  /// values="value", fill_value=0.0)`.

  /// Decimal places to keep on the on-disk value columns.
  ///
  /// Default `7` (since 2026-05-19) — triggers the **float32**
  /// specialization in `make_field_arrays_long`.  Float32 has ~7
  /// significant decimal digits, so rounding the double to 7
  /// decimals first and then casting to float32 produces a lossless
  /// (within float32 precision) representation that halves the raw
  /// value-column byte budget.  Combined with `BYTE_STREAM_SPLIT +
  /// zstd` this lands the 2-year case at ~265 MB (vs 305 MB at
  /// double + d=8 + zstd; vs ~1 GB at the legacy wide + snappy
  /// default).
  ///
  /// Values `1..7` → float32 storage.  Value `≥ 8` → double storage
  /// with explicit `round_to_digits` (the "I need more than 7 decimal
  /// digits" path).  Value `≤ 0` → no rounding, double storage (the
  /// "verify all 15+ digits" opt-out).
  ///
  /// Bound to JSON `output_round_decimals` and CLI
  /// `--output-round-decimals`.
  static constexpr int default_output_round_decimals = 7;
  [[nodiscard]] constexpr auto output_round_decimals() const noexcept -> int
  {
    return m_options_.output_round_decimals.value_or(
        default_output_round_decimals);
  }

  /**
   * @brief Gets the output compression codec as a string name
   * @return The compression codec name for output files
   */
  [[nodiscard]] auto output_compression() const -> std::string_view
  {
    return enum_name(
        m_options_.output_compression.value_or(default_output_compression));
  }

  /**
   * @brief Gets the output compression codec as a typed enum
   * @return The CompressionCodec enum value
   */
  [[nodiscard]] constexpr auto output_compression_enum() const
      -> CompressionCodec
  {
    return m_options_.output_compression.value_or(default_output_compression);
  }

  /**
   * @brief Gets the annual discount rate, using default if not set
   * @return The annual discount rate for multi-year planning
   */
  /// @brief Gets the annual discount rate (deprecated flat field fallback).
  /// Canonical location is simulation.annual_discount_rate.
  [[nodiscard]] constexpr auto annual_discount_rate() const
  {
    return m_options_.annual_discount_rate.value_or(
        default_annual_discount_rate);
  }

  /**
   * @brief Gets the global LP solver options sub-object.
   *
   * Returns the @c SolverOptions sub-object embedded in the planning JSON
   * @c options block.
   *
   * @return Const reference to the @c SolverOptions from the wrapped
   * PlanningOptions
   */
  [[nodiscard]] constexpr const SolverOptions& solver_options() const noexcept
  {
    return m_options_.solver_options;
  }

  /**
   * @brief Gets the LP debug flag, using default if not set
   * @return Whether to save debug LP files to the log directory
   */
  [[nodiscard]] constexpr auto lp_debug() const
  {
    return m_options_.lp_debug.value_or(false);
  }

  /// When true, write an error LP file on cell infeasibility and keep the
  /// name metadata needed for it (independent of `lp_debug`).
  [[nodiscard]] constexpr auto lp_error() const
  {
    return m_options_.lp_error.value_or(false);
  }

  /// Minimum scene UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_scene_min() const -> OptInt
  {
    return m_options_.lp_debug_scene_min;
  }
  /// Maximum scene UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_scene_max() const -> OptInt
  {
    return m_options_.lp_debug_scene_max;
  }
  /// Minimum phase UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_phase_min() const -> OptInt
  {
    return m_options_.lp_debug_phase_min;
  }
  /// Maximum phase UID for selective LP debug saving (nullopt = no filter).
  [[nodiscard]] constexpr auto lp_debug_phase_max() const -> OptInt
  {
    return m_options_.lp_debug_phase_max;
  }

  /**
   * @brief Gets the LP compression codec for debug LP files as a string.
   *
   * Returns the codec name when set, or empty string (the default) meaning
   * "inherit from output_compression".  `"uncompressed"` disables LP
   * compression regardless of `output_compression`.
   *
   * @return Compression codec string (may be empty = inherit)
   */
  [[nodiscard]] auto lp_compression() const -> std::string_view
  {
    if (m_options_.lp_compression.has_value()) {
      return enum_name(*m_options_.lp_compression);
    }
    return "";
  }

  /**
   * @brief Gets the LP compression codec as a typed enum (nullopt = inherit)
   * @return The CompressionCodec enum value, or nullopt
   */
  [[nodiscard]] constexpr auto lp_compression_enum() const
      -> std::optional<CompressionCodec>
  {
    return m_options_.lp_compression;
  }

  /**
   * @brief Gets the lp_only flag, using default if not set.
   *
   * When true, the solver builds all scene×phase LP matrices but skips
   * solving entirely.  Applies uniformly to both the monolithic solver and
   * the SDDP solver: exit right after LP assembly with no solve at all.
   * Combine with lp_debug=true to save every scene/phase LP file to disk.
   *
   * @return Whether to stop after LP building
   */
  [[nodiscard]] constexpr auto lp_only() const
  {
    return m_options_.lp_only.value_or(false);
  }

  /**
   * @brief Gets the lp_fingerprint flag, using default if not set.
   *
   * When true, write LP fingerprint JSON to the output directory after
   * LP assembly.  The fingerprint captures the structural template
   * (which types of variables/constraints exist) for regression detection.
   *
   * @return Whether to write LP fingerprint files
   */
  [[nodiscard]] constexpr auto lp_fingerprint() const
  {
    return m_options_.lp_fingerprint.value_or(false);
  }

  /// Solver backend pinned at the planning level via
  /// ``options.lp_matrix_options.solver_name`` (programmatic API — the
  /// field is not JSON-mapped).  Empty = no pin; callers fall through to
  /// ``SolverRegistry::default_solver()`` (GTOPT_SOLVER env / priority).
  [[nodiscard]] constexpr auto lp_solver_name() const noexcept
      -> const std::string&
  {
    return m_options_.lp_matrix_options.solver_name;
  }

  /**
   * @brief Gets the LP coefficient ratio threshold for conditioning
   * diagnostics.
   * @return The threshold above which per-scene/phase LP stats are shown
   *         (default 1e7).
   */
  [[nodiscard]] constexpr auto lp_coeff_ratio_threshold() const
  {
    static constexpr double default_lp_coeff_ratio_threshold = 1e7;
    return m_options_.lp_matrix_options.lp_coeff_ratio_threshold.value_or(
        default_lp_coeff_ratio_threshold);
  }

  /// The matrix equilibration method to use.
  ///
  /// Equilibration default depends on network topology:
  ///   - Single-bus mode  → ``none``  (no row/column scaling needed —
  ///     all flows live on one bus, the constraint matrix has uniform
  ///     scales by construction).
  ///   - Multi-bus Kirchhoff → ``ruiz`` (heterogeneous reactance × tmax
  ///     ratios across lines create wide row/column magnitude spread
  ///     that ruiz iteration tames).
  ///   - Multi-bus copper-plate → ``none`` (no Kirchhoff coupling).
  ///
  /// **Single-bus default changed to ``none`` in 2026-05** after the
  /// juan/IPLP scaling benchmark (16-scene, 51-phase, 7,431-cut
  /// hot-start; ``-b 1`` single-bus mode) showed equilibration on
  /// top of ``scale_objective`` and per-element ``auto_scale``
  /// widens basis kappa rather than improving it.  iter-15 kappa:
  ///
  ///   - ``row_max`` (old single-bus default) : 1.27e+08
  ///   - ``none``    (new single-bus default) : 8.10e+07 (1.6× lower)
  ///
  /// Solution quality (gap, LB, UB) is invariant across choices.
  /// Multi-bus Kirchhoff cases (IEEE-9/14/57/118-bus benchmarks)
  /// still benefit from ruiz — the reactance disparity across the
  /// network drives that need — so the multi-bus default is unchanged.
  ///
  /// User-supplied JSON ``options.lp_matrix_options.equilibration_method``
  /// = ``"ruiz" | "row_max" | "none"`` always takes precedence.
  [[nodiscard]] constexpr auto equilibration_method() const noexcept
  {
    if (m_options_.lp_matrix_options.equilibration_method.has_value()) {
      return *m_options_.lp_matrix_options.equilibration_method;
    }
    const bool kirchhoff_multi_bus = use_kirchhoff() && !use_single_bus();
    return kirchhoff_multi_bus ? LpEquilibrationMethod::ruiz
                               : LpEquilibrationMethod::none;
  }

  /// Controls error handling for user constraint resolution.
  /// Default is `strict` — fail on any unresolved reference.
  [[nodiscard]] constexpr auto constraint_mode() const noexcept
  {
    return m_options_.constraint_mode.value_or(ConstraintMode::strict);
  }

  /// Which output fields `OutputContext` should emit.  Default is
  /// `OutputFlags::all` — primal solutions, row duals, and reduced
  /// costs, all unscoped (every element class included).  The
  /// reduced-cost streams are needed by `gtopt_marginal_units` to
  /// identify the marginal unit at each (bus, scene, stage, block) —
  /// without them the attribution must fall back to the static
  /// `gcost` from the planning JSON, which silently fails for
  /// piecewise generators and for hydro / battery units whose true MC
  /// is a reservoir / battery shadow price.  Users who want a leaner
  /// output footprint can pass any of:
  ///
  ///   - `--write-out sol`                  primal only
  ///   - `--write-out sol,dual`             primal + LMPs
  ///   - `--write-out sol,dual,rc:Generator`   primal + duals everywhere,
  ///                                           rc only on Generator
  ///
  /// Bound to CLI `--write-out` and JSON `write_out`.
  [[nodiscard]] auto write_out() const noexcept -> OutputSelection
  {
    // Default emits every primal solution + every dual everywhere, and
    // reduced costs ONLY on Generator and Line — the union of what
    // every current consumer reads:
    //   * `gtopt_marginal_units` → Generator/generation_cost.
    //   * `gtopt_check_output`   → since the rc-based cost-breakdown
    //                              bug was fixed, no rc stream is
    //                              needed; sol × coefficient suffices.
    //   * `gtopt_results_summary` → Generator/generation_cost.
    //   * `gtopt_compare`         → sol only.
    // Line is included in the rc scope because the marginal-units
    // pipeline documents it as part of the recommended recipe (kept
    // forward-compat for any future congestion-rent analysis that may
    // want flowp_cost / flown_cost — both currently emit only when the
    // line has overload slacks, so cost is near-zero on typical cases).
    // Extras (heat-rate slacks, vom/fuel decomposition, line losses,
    // overload slacks, capacity duals) stay off by default; opt in via
    // `--write-out all` or `--write-out ...,extras` /
    // `...,extras:Generator`.
    if (m_options_.write_out.has_value()) {
      return *m_options_.write_out;
    }
    OutputSelection sel;
    sel.atoms =
        OutputFlags::solution | OutputFlags::dual | OutputFlags::reduced_cost;
    sel.rc_classes = {"Generator", "Line"};
    return sel;
  }

  /**
   * @brief Gets the effective SDDP forward-pass solver options.
   *
   * Merges the per-pass forward solver options (if set) with the global
   * solver_options.  Forward-pass-specific options take precedence.
   *
   * @return Resolved SolverOptions for SDDP forward pass
   */
  [[nodiscard]] auto sddp_forward_solver_options() const -> SolverOptions
  {
    auto opts = m_options_.solver_options;
    if (m_options_.sddp_options.forward_solver_options.has_value()) {
      opts.merge(*m_options_.sddp_options.forward_solver_options);
    }
    opts.max_fallbacks = m_options_.sddp_options.forward_max_fallbacks.value_or(
        opts.max_fallbacks);
    apply_sddp_pass_defaults(opts);
    return opts;
  }

  /**
   * @brief Gets the effective SDDP backward-pass solver options.
   *
   * Merges the per-pass backward solver options (if set) with the global
   * solver_options.  Backward-pass-specific options take precedence.
   * Applies backward_max_fallbacks (default: 0).
   *
   * @return Resolved SolverOptions for SDDP backward pass
   */
  [[nodiscard]] auto sddp_backward_solver_options() const -> SolverOptions
  {
    auto opts = m_options_.solver_options;
    if (m_options_.sddp_options.backward_solver_options.has_value()) {
      opts.merge(*m_options_.sddp_options.backward_solver_options);
    }
    opts.max_fallbacks =
        m_options_.sddp_options.backward_max_fallbacks.value_or(0);
    apply_sddp_pass_defaults(opts);
    return opts;
  }

private:
  /// SDDP-pass-specific solver defaults applied to the merged
  /// forward/backward options when the user hasn't overridden them
  /// (struct-default sentinels still present after merge).  Both
  /// passes solve thousands of small-to-medium LPs in parallel via
  /// the SDDP work pool; leaving the CPLEX backend at its own
  /// default (algorithm picked per LP, threads = min(32, ncores))
  /// catastrophically oversubscribes a multi-core host when
  /// multiple cells are in flight.  Pin barrier + a small per-LP
  /// thread count so the WorkPool's scene-level parallelism wins
  /// the CPU budget.
  ///
  /// Defaults only fire on the struct sentinels.  Any explicit
  /// user value (top-level `solver_options` JSON block, per-pass
  /// override, or CLI `--threads`/`--set solver_options.threads=N`)
  /// survives — the JSON-parsed SolverOptionsConstructor already
  /// substitutes its own defaults (barrier, threads=2) for omitted
  /// fields, and neither of those equals the sentinel, so user JSON
  /// wins as expected.
  ///
  /// **Default `threads = 1` (changed from 4 on 2026-05-15)**:
  /// juan/IPLP empirically showed `threads = 1` is faster than
  /// `threads = 4` for SDDP forward/backward passes — at the SDDP
  /// scale the LPs are small enough that CPLEX's per-LP
  /// thread-coordination overhead exceeds the parallel speedup,
  /// and the work pool can fit more concurrent LP solves without
  /// oversubscribing the host.  Observed wall-time speedup on
  /// L0 warmup: ~2.6× per iter (15-17 s → 6-8 s).  L1 / L2 see a
  /// similar 1.5-2× speedup.  CPU utilisation is healthier too
  /// (more threads in R, fewer in `futex_wait_queue`).
  static constexpr void apply_sddp_pass_defaults(SolverOptions& opts) noexcept
  {
    if (opts.algorithm == LPAlgo::default_algo) {
      opts.algorithm = LPAlgo::barrier;
    }
    if (opts.threads == 0) {
      opts.threads = 1;
    }
    // Pin scaling=automatic explicitly (instead of relying on
    // CPLEX's own automatic default).  Functionally identical
    // today — the CPLEX default for `CPX_PARAM_SCAIND` is 0
    // (equilibration + aggressive scaling) which matches our
    // `SolverScaling::automatic` enum — but making the choice
    // explicit at the gtopt level keeps the value visible in
    // `cplex_*.log` parameter dumps alongside threads/algorithm
    // for post-mortem reading, and locks us in if CPLEX ever
    // shifts its default.  `scaling` is `OptSolverScaling`
    // (std::optional) so `has_value()` is the sentinel.
    if (!opts.scaling.has_value()) {
      opts.scaling = SolverScaling::automatic;
    }
  }

public:
  /** @brief Aperture LP timeout in seconds.
   * @return Aperture timeout (default 15s)
   */
  [[nodiscard]] constexpr auto sddp_aperture_timeout() const
  {
    return m_options_.sddp_options.aperture_timeout.value_or(15.0);
  }

  /** @brief Whether to save LP files for infeasible apertures.
   * @return false by default (disabled).
   */
  [[nodiscard]] constexpr auto sddp_save_aperture_lp() const
  {
    return m_options_.sddp_options.save_aperture_lp.value_or(false);
  }

  /** @brief Comma-separated SDDP passes whose LP-debug dump is active
   *         when `lp_debug=true`.  Empty / unset defaults to
   *         `"forward,aperture"` (legacy).
   *  @return empty string by default.
   */
  [[nodiscard]] constexpr auto sddp_lp_debug_passes() const -> std::string_view
  {
    if (m_options_.sddp_options.lp_debug_passes.has_value()) {
      return *m_options_.sddp_options.lp_debug_passes;
    }
    return {};
  }

  /** @brief Whether aperture clones bypass the backend's native `clone()`
   *         and are built via `LinearInterface::clone_from_flat()` instead
   *         (manual route, no global mutex).
   *
   * @return ``true`` by default — measured ~2-3× speed-up on the
   *         juan/iplp aperture phase by escaping the global
   *         ``s_global_clone_mutex`` that serialises ``CPXcloneprob``
   *         calls across the SDDP work pool.  Safe under all
   *         ``LowMemoryMode`` settings: the call site falls back to
   *         the native ``clone()`` route automatically when the source
   *         has no snapshot (``has_snapshot_data() == false``), which
   *         is the only condition where the manual route cannot run.
   *         Set to ``false`` to restore the legacy native-clone path.
   */
  [[nodiscard]] constexpr auto sddp_aperture_use_manual_clone() const
  {
    return m_options_.sddp_options.aperture_use_manual_clone.value_or(true);
  }

  /// Whether to seed each iteration's first backward aperture from the
  /// previous iteration's first-aperture basis (dual warm start).  Default
  /// false — meaningful for every basis-capable (vertex) mode, i.e. all
  /// `aperture_solve_mode`s except `reduced_cost`.
  [[nodiscard]] constexpr auto sddp_aperture_seed_basis() const
  {
    return m_options_.sddp_options.aperture_seed_basis.value_or(false);
  }

  /// Resolved per-task aperture chunk size (sentinel-encoded).
  ///
  ///   *  0 → auto (compute_auto_aperture_chunk_size at SDDPMethod setup).
  ///   *  1 → legacy 1-task-per-aperture.
  ///   * >1 → exactly K apertures per task, serial within.
  ///   * -1 → cap at A_max per phase (single task per scene).
  ///
  /// Default: 0 (auto).  See `SddpOptions::aperture_chunk_size` for
  /// the JSON-facing documentation.
  [[nodiscard]] constexpr auto sddp_aperture_chunk_size() const noexcept
  {
    return m_options_.sddp_options.aperture_chunk_size.value_or(0);
  }

  /// Aperture solve / cut-recovery mode.  Default `warm` (within-chunk
  /// dual-simplex warm chain off the resident basis; combined with the
  /// default `basis_cross_mode = full_cross` below, the chunk's first
  /// aperture is dual-seeded from the forward basis).  plp2gtopt also
  /// emits `warm` explicitly.  See `SddpOptions::aperture_solve_mode`.
  [[nodiscard]] constexpr auto sddp_aperture_solve_mode() const noexcept
  {
    return m_options_.sddp_options.aperture_solve_mode.value_or(
        ApertureSolveMode::warm);
  }

  /// Number of dual-shared aperture cuts re-solved exactly under
  /// `aperture_solve_mode = screened` (picked by largest |intercept
  /// correction|).  Ignored by every other mode.  Default
  /// `default_sddp_aperture_screen_count` (2, `sddp_enums.hpp` — the
  /// single source).  See `SddpOptions::aperture_screen_count`.
  [[nodiscard]] constexpr auto sddp_aperture_screen_count() const noexcept
  {
    return m_options_.sddp_options.aperture_screen_count.value_or(
        default_sddp_aperture_screen_count);
  }

  /// Cross-pass simplex-basis warm-start reuse mode.  Default `full_cross`
  /// (forward→forward warm reuse + forward basis fed into the backward/tgt
  /// and aperture solves).  Combined with the seeded-solve auto-dual logic
  /// this makes every warm SDDP solve run dual simplex off a reused basis
  /// (cold iter-1 solves keep barrier to produce a capturable vertex basis).
  /// Convergence-safe (cuts stay valid vertex duals); on degenerate problems
  /// the trajectory can differ slightly from `off` while still converging.
  /// See `SddpOptions::basis_cross_mode` / `BasisCrossMode`.
  [[nodiscard]] constexpr auto sddp_basis_cross_mode() const noexcept
  {
    return m_options_.sddp_options.basis_cross_mode.value_or(
        BasisCrossMode::full_cross);
  }

  /**
   * @brief Gets the effective monolithic solver options.
   *
   * Merges the per-method monolithic solver options (if set) with the global
   * solver_options.  Per-method options override the global ones.
   *
   * @return Resolved SolverOptions for the monolithic solver
   */
  [[nodiscard]] auto monolithic_solver_options() const -> SolverOptions
  {
    if (m_options_.monolithic_options.solver_options.has_value()) {
      auto opts = m_options_.solver_options;
      opts.merge(*m_options_.monolithic_options.solver_options);
      return opts;
    }
    return m_options_.solver_options;
  }

  // ── Monolithic solver accessors ─────────────────────────────────────────

  /// CSV file with boundary cuts for the monolithic solver (empty = none).
  [[nodiscard]] auto monolithic_boundary_cuts_file() const -> Name
  {
    return m_options_.monolithic_options.boundary_cuts_file.value_or(Name {});
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto monolithic_boundary_max_iterations() const -> int
  {
    return m_options_.monolithic_options.boundary_max_iterations.value_or(0);
  }

  /// Initial-MIP-solution (warm-start) options for the monolithic solver.
  /// Returns the configured `MipStartOptions` or a default (all-unset, i.e.
  /// the feature off) when none was supplied.
  [[nodiscard]] auto mip_start_options() const -> MipStartOptions
  {
    return m_options_.monolithic_options.mip_start.value_or(MipStartOptions {});
  }

  // Default values for SDDP solver settings
  /** @brief Default solver type */
  static constexpr MethodType default_method_type = MethodType::monolithic;
  /** @brief Default cut sharing mode for SDDP */
  static constexpr CutSharingMode default_sddp_cut_sharing_mode =
      CutSharingMode::none;
  /** @brief Default forward-pass sampling mode (persistent per-scene
   * paths — the historical behaviour; `resampled` opts into the
   * per-phase-boundary probability-weighted re-draw that matches the
   * multicut LB's stagewise-resampled process) */
  static constexpr ForwardSamplingMode default_sddp_forward_sampling_mode =
      ForwardSamplingMode::persistent;
  /** @brief Default integer-cut mode (legacy: no strengthening; the
   * `strengthened` mode is the opt-in one-MIP Lagrangian intercept for
   * integer-bearing backward cells — see `IntegerCutsMode`) */
  static constexpr IntegerCutsMode default_sddp_integer_cuts_mode =
      IntegerCutsMode::none;
  /** @brief Default async cut-drain mode (symmetric, deterministic) */
  static constexpr CutDrainMode default_sddp_cut_drain_mode =
      CutDrainMode::iteration;
  /** @brief Default directory for Benders cut files */
  static constexpr auto default_sddp_cut_directory = "cuts";
  /** @brief Default directory for log/trace files */
  static constexpr auto default_log_directory = "logs";
  /** @brief Default for SDDP monitoring API (enabled by default) */
  static constexpr Bool default_sddp_api_enabled = true;
  /** @brief Default iterations to skip between update_lp dispatches (0 = every
   * iteration, matching PLP behaviour) */
  static constexpr Int default_sddp_update_lp_skip = 0;
  /** @brief Default maximum SDDP iterations */
  static constexpr Int default_sddp_max_iterations = 100;
  /** @brief Default minimum iterations before declaring convergence.
   *
   * 1 iteration is the minimum that still lets the convergence check
   * fire on the first solved iter.  Pure SDDP runs that want the
   * 3-iter bootstrap guard (against declaring convergence on iter 0's
   * placeholder LB or iter 1's barely-moved LB) can set
   * ``min_iterations`` higher explicitly; cascade level 0 still does
   * so via ``plp2gtopt`` since the 1-aperture face-value bound there
   * is genuinely noisy.  L1+ inherit a converged envelope and may
   * legitimately exit on their first qualifying iter — leaving the
   * default at 3 forced uninodal to grind two redundant iters past
   * the first time |gap| dropped inside the ceiling (observed on
   * juan/IPLP, 2026-05-14). */
  static constexpr Int default_sddp_min_iterations = 1;
  /** @brief Default relative convergence tolerance.
   *
   * 1 % is the industry-standard SDDP/Benders gap target.  Tighter
   * (0.1 % or 0.01 %) is rarely achievable on production hydro/thermal
   * problems and tends to leave the solver running to
   * ``max_iterations``; looser (5 %) is fine for early prototyping. */
  static constexpr Real default_sddp_convergence_tol = 0.01;
  /** @brief Default elastic slack penalty (per physical unit, scaled
   *         by `var_scale` per link inside `relax_fixed_state_variable`).
   *
   *         Reduced from 1e3 → 1e2: with the Chinneck IIS filter the
   *         penalty no longer needs to dominate natural generation costs
   *         (~50 $/MWh).  Smaller penalties keep the LP matrix
   *         well-conditioned and reduce `rescale_benders_cut` warnings.
   *         Override via `--set sddp_options.elastic_penalty=<p>` or
   *         the `sddp_options.elastic_penalty` JSON field if a particular
   *         case needs a stronger forcing term. */
  // PLP parity: slack obj = 1.0 flat (osicallsc.cpp:658, objs passed as 0
  // → default 1.0 for every slack).  gtopt's elastic_filter_solve zeroes
  // the clone's original obj and prices slacks at this penalty, so 1.0
  // reproduces PLP's pure Chinneck Phase-1 feasibility LP.
  static constexpr Real default_sddp_elastic_penalty = 1.0;
  // α is a free LP variable (no explicit bounds) — see
  // `sddp_method.cpp::initialize_alpha_variables`.  The
  // `default_sddp_alpha_min` / `default_sddp_alpha_max` constants were
  // removed when the bounds were dropped.
  /** @brief Default cut coefficient epsilon for filtering tiny coefficients.
   *
   * PLP parity: PLP's `FactEPS = 1e-8` (getopts.f:231) is the single
   * tolerance used both as the ray-zero threshold and the dx-filter
   * scale in `osi_lp_get_feasible_cut` (osicallsc.cpp:723,727).  We
   * use the same value as `cut_coeff_eps` — the tighter 1e-8 lets
   * weakly-coupled links survive the filter (1e-6 would drop them
   * silently on large-trial-value cases).  Academic / toy fixtures
   * can raise this via JSON if they need the looser legacy 1e-6.
   */
  static constexpr Real default_sddp_cut_coeff_eps = 1e-8;
  /** @brief Default elastic filter mode.
   *
   *  Set to `single_cut` — the classical PLP/Birge-Louveaux Benders
   *  feasibility cut.  A single cut is built from the row duals of
   *  the state-fixing equations at the elastic clone's Phase-1
   *  optimum (see `build_feasibility_cut_physical` in
   *  `benders_cut.cpp` and the matching `plp-agrespd.f::AgrElastici`
   *  reference implementation).  Validated end-to-end by the
   *  PLP-style backtracking unit tests in `test_sddp_method.cpp`
   *  — `single_cut` is the mode that makes the cascade converge.
   *
   *  `chinneck` (IIS-based) and `multi_cut` remain available as
   *  explicit modes when per-bound cuts or IIS filtering are wanted;
   *  neither has been re-validated against the row-dual fcut builder
   *  and they retain their prior semantics. */
  static constexpr ElasticFilterMode default_sddp_elastic_mode =
      ElasticFilterMode::single_cut;
  /** @brief Default multi_cut threshold (auto-switch after this many
   *         cumulative forward-pass infeasibilities at a phase).
   *
   *         Counter is persistent across iterations (no reset on
   *         successful solves), so the threshold counts *total* fcut
   *         events at a (scene, phase), not consecutive ones.  Reduced
   *         Default: 100.  Under `ElasticFilterMode::chinneck` (the
   *         gtopt default), the IIS-filtered fcut alone is usually
   *         enough to cut off the bad trial over subsequent
   *         iterations.  `build_multi_cuts` can install
   *         `source_col {<=,>=} dep_val_phys` bound cuts whose RHS
   *         (`dep_val_phys` from the Chinneck Phase-1 LP) may exceed
   *         the source phase's physically reachable set, making the
   *         master LP infeasible (observed on juan/gtopt_iplp:
   *         reservoir mcut required end-of-phase energy ≥ 330.33 with
   *         a physical cap of ~207.78 + limited inflow).  Raising the
   *         threshold defers the risky bound cuts until many
   *         infeasibility events have accumulated at the same (scene,
   *         phase), signalling that the fcut alone is not sufficient. */
  static constexpr int default_sddp_multi_cut_threshold = 100;
  /** @brief Default stationary-gap tolerance.
   *
   * "Gap stopped moving" threshold — gap_change < tol means the gap is
   * stationary.  Tighter than ``convergence_tol`` (0.5 % vs 1 %) so
   * the safety net only fires when the gap has *clearly* stalled, not
   * while it is still trending. */
  static constexpr Real default_sddp_stationary_tol = 0.005;
  /** @brief Default look-back window for stationary gap check (one
   * season). */
  static constexpr Int default_sddp_stationary_window = 4;
  /** @brief Default confidence level for statistical convergence.
   *
   * 0.0 (DISABLED) — the statistical CI test (PLP-style ``UB - LB <=
   * z·σ``) is opt-in.  Under heterogeneous-scene scatter (σ ≈ 50 % of
   * mean on Maule/juan) z·σ trivially exceeds the absolute gap at
   * 25 % relative, declaring premature convergence.  Set to 0.95 to
   * enable the standard 95 % CI test, or 0.50 for a tighter narrow-CI
   * variant. */
  static constexpr Real default_sddp_convergence_confidence = 0.0;
  /** @brief Default absolute gap ceiling for the secondary convergence
   * tests (stationary + statistical CI).
   *
   * Refuse to declare convergence via stationarity or CI while the
   * relative gap is at or above 5 % — only the primary
   * ``convergence_tol`` test (1 %) can close the run at high gaps.
   *
   * Defends against heterogeneous-scene σ explosion (juan trace_28:
   * gap = 25 % declared converged at iter 2 because σ ≈ 77 M
   * dominated the 38 M absolute gap) and frozen-LB pathologies (LB
   * stuck at 0, gap = 1 flat → gap_change = 0).  Set to 1.0 to
   * disable the ceiling (legacy behaviour). */
  static constexpr Real default_sddp_stationary_gap_ceiling = 0.05;
  /** @brief Default consecutive-structural-failures threshold before a
   * scene is marked terminal.  See
   * ``SDDPMethod::SceneRetryState::terminal`` and
   * ``SDDPOptions::terminal_failure_threshold`` for the full story. */
  static constexpr Int default_sddp_terminal_failure_threshold = 2;
  /** @brief Default for the scene-level fail-stop forward pass.
   *
   *  true (the new default) — when an infeasible phase emits an fcut
   *  on its predecessor, stop the scene's forward pass for the
   *  current iteration.  The next iteration restarts from p1 with
   *  the freshly accumulated cuts.
   *
   *  false — restore the legacy PLP-style backtracking cascade:
   *  decrement `phase_idx` after installing the fcut and re-solve
   *  p-1 in the same forward pass, recursing back through phases
   *  until a feasible point is reached or `forward_max_attempts`
   *  is exhausted. */
  // Default flipped 2026-04-29: every cascade-style test in the suite
  // had to override the prior `true` default with `false`, indicating
  // the option's `true` semantics ("install one fcut + exit scene
  // for this iteration") was at odds with users' intuitive expectation
  // that ``forward_fail_stop = true`` should mean "give up only when
  // the scene is *truly* unrecoverable" — i.e., when the cascade
  // reaches phase 0 / 1 with no recovery, or the elastic filter
  // returns no cuts.  Defaulting to `false` (PLP-style backtracking
  // cascade) aligns the most common use case with the natural
  // intuition.  Production callers that wanted the per-iteration
  // accumulation strategy can still opt in by setting
  // ``forward_fail_stop = true`` explicitly.
  static constexpr Bool default_sddp_forward_fail_stop = false;

  /// Per-scene rollback default — see `SDDPOptions::forward_infeas_rollback`
  /// in `sddp_types.hpp` for full semantics.  Flipped to `true`
  /// 2026-04-30 (plan step 6) after one full juan/gtopt_iplp run
  /// confirmed the feature does not regress legacy paths: every
  /// pre-existing test that doesn't produce a real
  /// `scene_feasible[s] == 0` outcome stays a no-op under the
  /// rollback hook + stall guard, and the stall guard's "no
  /// recovery path" abort only fires when ALL previously-failed
  /// scenes saw zero new cuts since their last failure (a strict
  /// improvement over the legacy "spin until max_iterations" loop).
  /// Callers that want the legacy behaviour can still opt out by
  /// setting `sddp.forward_infeas_rollback: false` in JSON.
  static constexpr Bool default_sddp_forward_infeas_rollback = true;

  /// Re-solve target phase t LP at v̂_{t-1} before extracting cut data.
  /// Default `true` (textbook one-iter-tight SDDP cuts that propagate
  /// through all phases in one pass).  Set `false` per case in JSON to
  /// fall back to the cheaper forward-cached cut data.  See
  /// `SDDPOptions::backward_resolve_target` for the cost trade-off.
  static constexpr Bool default_sddp_backward_resolve_target = true;

  /**
   * @brief Gets the SDDP cut sharing mode as a string name
   * @return The cut sharing mode name
   */
  [[nodiscard]] auto sddp_cut_sharing_mode() const -> std::string_view
  {
    return enum_name(sddp_cut_sharing_mode_enum());
  }

  /**
   * @brief Gets the SDDP cut sharing mode as a typed enum
   * @return The CutSharingMode enum value
   */
  [[nodiscard]] constexpr auto sddp_cut_sharing_mode_enum() const
      -> CutSharingMode
  {
    return m_options_.sddp_options.cut_sharing_mode.value_or(
        default_sddp_cut_sharing_mode);
  }

  /// Scene → Markov-state assignment for `cut_sharing_mode = markov`
  /// (`SddpOptions::markov_states`).  Empty when unset.
  [[nodiscard]] auto sddp_markov_states() const -> std::vector<int>
  {
    return m_options_.sddp_options.markov_states.value_or(Array<int> {});
  }

  /// Row-major M×M Markov transition matrix for
  /// `cut_sharing_mode = markov` (`SddpOptions::markov_transition`).
  /// Empty when unset.
  [[nodiscard]] auto sddp_markov_transition() const -> std::vector<double>
  {
    return m_options_.sddp_options.markov_transition.value_or(Array<double> {});
  }

  /**
   * @brief Gets the SDDP integer-cut mode as a typed enum
   * @return The IntegerCutsMode enum value
   */
  [[nodiscard]] constexpr auto sddp_integer_cuts_mode_enum() const
      -> IntegerCutsMode
  {
    return m_options_.sddp_options.integer_cuts_mode.value_or(
        default_sddp_integer_cuts_mode);
  }

  // (The string-form `sddp_forward_sampling_mode()` accessor was
  // removed 2026-07-08 — it had zero callers; use
  // `enum_name(sddp_forward_sampling_mode_enum())` at a use site.)

  /**
   * @brief Gets the SDDP forward sampling mode as a typed enum
   * @return The ForwardSamplingMode enum value
   */
  [[nodiscard]] constexpr auto sddp_forward_sampling_mode_enum() const
      -> ForwardSamplingMode
  {
    return m_options_.sddp_options.forward_sampling_mode.value_or(
        default_sddp_forward_sampling_mode);
  }

  /**
   * @brief Get the resolved cut-drain mode for the async SDDP path.
   *
   * Default is `CutDrainMode::iteration` (symmetric, run-to-run
   * deterministic).  See `CutDrainMode` in `sddp_enums.hpp` for the
   * full rationale and the comparison against `count` (legacy
   * asymmetric) and `all` (no truncation).
   */
  [[nodiscard]] constexpr auto sddp_cut_drain_mode() const -> CutDrainMode
  {
    return m_options_.sddp_options.cut_drain_mode.value_or(
        default_sddp_cut_drain_mode);
  }

  /**
   * @brief Gets the cut directory for SDDP cut files, using default if not set
   * @return The cut directory path
   */
  [[nodiscard]] constexpr auto sddp_cut_directory() const
  {
    return m_options_.sddp_options.cut_directory.value_or(
        default_sddp_cut_directory);
  }

  /**
   * @brief Gets the log directory for log/trace files.
   *
   * When `log_directory` is explicitly set in the JSON / CLI, that value is
   * used as-is.  Otherwise the default is `output_directory + "/logs"` so
   * that all solver output (results, cuts, logs) is consolidated under a
   * single root directory.
   *
   * @return The log directory path (global — used by both monolithic and SDDP)
   */
  [[nodiscard]] auto log_directory() const -> std::string
  {
    if (m_options_.log_directory.has_value()) {
      return m_options_.log_directory.value();
    }
    return (std::filesystem::path(output_directory()) / "logs").string();
  }

  /**
   * @brief Gets the SDDP monitoring API enabled flag, using default if not set
   * @return Whether the SDDP monitoring API is enabled (default: true)
   */
  [[nodiscard]] constexpr auto sddp_api_enabled() const
  {
    return m_options_.sddp_options.api_enabled.value_or(
        default_sddp_api_enabled);
  }

  /**
   * @brief Gets the global update_lp skip count
   * @return Number of SDDP iterations to skip between update_lp dispatches
   */
  [[nodiscard]] constexpr auto sddp_update_lp_skip() const
  {
    return m_options_.sddp_options.update_lp_skip.value_or(
        default_sddp_update_lp_skip);
  }

  /**
   * @brief Gets the maximum SDDP iterations
   * @return Maximum number of forward/backward iterations (default: 100)
   */
  [[nodiscard]] constexpr auto sddp_max_iterations() const
  {
    return m_options_.sddp_options.max_iterations.value_or(
        default_sddp_max_iterations);
  }

  /**
   * @brief Gets the minimum SDDP iterations before convergence
   * @return Minimum iterations before convergence (default: 2)
   */
  [[nodiscard]] constexpr auto sddp_min_iterations() const
  {
    return m_options_.sddp_options.min_iterations.value_or(
        default_sddp_min_iterations);
  }

  /**
   * @brief Gets the SDDP convergence tolerance
   * @return Relative gap tolerance for convergence (default: 1e-4)
   */
  [[nodiscard]] constexpr auto sddp_convergence_tol() const
  {
    return m_options_.sddp_options.convergence_tol.value_or(
        default_sddp_convergence_tol);
  }

  /**
   * @brief Gets the elastic slack penalty
   * @return Penalty for elastic slack variables (default: 1e6)
   */
  [[nodiscard]] constexpr auto sddp_elastic_penalty() const
  {
    return m_options_.sddp_options.elastic_penalty.value_or(
        default_sddp_elastic_penalty);
  }

  /**
   * @brief Gets the scale divisor for future cost variable α
   * @return α scale divisor (default: 1000, analogous to PLP varphi scale)
   */
  /// Returns the user-provided scale_alpha, or 0 for auto-scale
  /// (computed at runtime as max state-variable var_scale).
  [[nodiscard]] constexpr auto sddp_scale_alpha() const
  {
    return m_options_.sddp_options.scale_alpha.value_or(0.0);
  }

  /**
   * @brief Whether to save cuts after each iteration (default: true)
   */
  [[nodiscard]] constexpr auto sddp_save_per_iteration() const
  {
    return m_options_.sddp_options.save_per_iteration.value_or(true);
  }

  /**
   * @brief Whether the forward pass fail-stops at the scene level
   *        instead of cascading backward through phases (default: true).
   * @return true if scene-level fail-stop is enabled.
   */
  [[nodiscard]] constexpr auto sddp_forward_fail_stop() const
  {
    return m_options_.sddp_options.forward_fail_stop.value_or(
        default_sddp_forward_fail_stop);
  }

  /**
   * @brief Whether per-scene infeasibility rollback is enabled.
   * @return true if forward_infeas_rollback is enabled (default: false).
   */
  [[nodiscard]] constexpr auto sddp_forward_infeas_rollback() const
  {
    return m_options_.sddp_options.forward_infeas_rollback.value_or(
        default_sddp_forward_infeas_rollback);
  }

  /**
   * @brief Whether the backward pass re-solves the target phase LP
   *        before extracting cut data.
   * @return true if backward_resolve_target is enabled (default: true).
   */
  [[nodiscard]] constexpr auto sddp_backward_resolve_target() const
  {
    return m_options_.sddp_options.backward_resolve_target.value_or(
        default_sddp_backward_resolve_target);
  }

  /**
   * @brief Simulation mode: no training iterations, forward-only pass.
   * @return true if simulation mode is enabled (default: false)
   */
  [[nodiscard]] constexpr auto sddp_simulation_mode() const
  {
    return m_options_.sddp_options.simulation_mode.value_or(false);
  }

  /**
   * @brief Gets the input cut file for SDDP hot-start
   * @return Cut file path or empty string for cold start
   */
  [[nodiscard]] auto sddp_cuts_input_file() const -> Name
  {
    return m_options_.sddp_options.cuts_input_file.value_or("");
  }

  /**
   * @brief Gets the sentinel file path for graceful SDDP stop
   * @return Sentinel file path or empty string (no sentinel)
   */
  [[nodiscard]] auto sddp_sentinel_file() const -> Name
  {
    return m_options_.sddp_options.sentinel_file.value_or("");
  }

  /**
   * @brief Gets the elastic filter mode as a string name
   * @return "chinneck" (default), "single_cut", or "multi_cut"
   */
  [[nodiscard]] auto sddp_elastic_mode() const -> std::string_view
  {
    return enum_name(sddp_elastic_mode_enum());
  }

  /**
   * @brief Gets the elastic filter mode as a typed enum
   * @return The ElasticFilterMode enum value
   */
  [[nodiscard]] constexpr auto sddp_elastic_mode_enum() const
      -> ElasticFilterMode
  {
    return m_options_.sddp_options.elastic_mode.value_or(
        default_sddp_elastic_mode);
  }

  /**
   * @brief Gets the cut coefficient tolerance for filtering tiny coefficients
   * @return Absolute tolerance (default: 0.0 = no filtering)
   */
  [[nodiscard]] constexpr auto sddp_cut_coeff_eps() const -> double
  {
    return m_options_.sddp_options.cut_coeff_eps.value_or(
        default_sddp_cut_coeff_eps);
  }

  /**
   * @brief Gets the multi_cut threshold
   * @return Forward-pass infeasibility count before auto-switching to
   *         multi_cut (default: 10; 0 = never auto-switch)
   */
  [[nodiscard]] constexpr auto sddp_multi_cut_threshold() const
  {
    return m_options_.sddp_options.multi_cut_threshold.value_or(
        default_sddp_multi_cut_threshold);
  }

  /// Aperture UIDs for the backward pass.
  /// nullopt = use per-phase apertures; empty = no apertures (Benders).
  [[nodiscard]] const auto& sddp_apertures() const noexcept
  {
    return m_options_.sddp_options.apertures;
  }

  /// First-N selector applied to each phase's `Phase::apertures` list.
  /// nullopt = no truncation (use full per-phase list).
  /// See `SddpOptions::num_apertures` for full semantics.
  [[nodiscard]] const auto& sddp_num_apertures() const noexcept
  {
    return m_options_.sddp_options.num_apertures;
  }

  /// Selection rule used by `num_apertures` (head | stride | tail).
  /// Returns the parsed enum value; defaults to `head` when the
  /// option is unset.
  [[nodiscard]] auto sddp_aperture_selection_mode() const
      -> ApertureSelectionMode
  {
    const auto& opt = m_options_.sddp_options.aperture_selection_mode;
    if (!opt) {
      return ApertureSelectionMode::head;
    }
    return gtopt::require_enum<ApertureSelectionMode>("aperture_selection_mode",
                                                      *opt);
  }

  /// Directory for aperture-specific scenario data (empty = use
  /// input_directory)
  [[nodiscard]] auto sddp_aperture_directory() const -> Name
  {
    return m_options_.sddp_options.aperture_directory.value_or(Name {});
  }

  /// Global default aperture-system file (the simplified backward system
  /// solved per aperture).  Empty = no global aperture system; the
  /// per-phase / per-cascade-level overrides still apply.  See
  /// `SddpOptions::aperture_system_file`.
  [[nodiscard]] auto sddp_aperture_system_file() const -> Name
  {
    return m_options_.sddp_options.aperture_system_file.value_or(Name {});
  }

  /// CSV file with boundary (future-cost) cuts for the last phase.
  /// Empty = no boundary cuts.
  [[nodiscard]] auto sddp_boundary_cuts_file() const -> Name
  {
    return m_options_.sddp_options.boundary_cuts_file.value_or(Name {});
  }

  /// Boundary cuts load mode as a string name.
  [[nodiscard]] auto sddp_boundary_cuts_mode() const -> std::string_view
  {
    return enum_name(sddp_boundary_cuts_mode_enum());
  }

  /// Maximum boundary cut iterations to load (0 = all).
  [[nodiscard]] auto sddp_boundary_max_iterations() const -> int
  {
    return m_options_.sddp_options.boundary_max_iterations.value_or(0);
  }

  /// How to handle cut rows referencing missing state variables.
  [[nodiscard]] constexpr auto sddp_missing_cut_var_mode() const
      -> MissingCutVarMode
  {
    return m_options_.sddp_options.missing_cut_var_mode.value_or(
        MissingCutVarMode::skip_coeff);
  }

  /// Whether to apply α-rebase (mean-shift) on boundary cut load.
  /// Default: **true** — centre α around zero so LP equilibration
  /// sees comparable RHS magnitudes for boundary vs. runtime cuts.
  /// Override with `--set sddp_options.boundary_cuts_mean_shift=false`
  /// when you need raw on-disk RHS magnitudes preserved (e.g. for
  /// byte-identical cut-file round-trips during regression bisects).
  [[nodiscard]] constexpr auto sddp_boundary_cuts_mean_shift() const -> bool
  {
    return m_options_.sddp_options.boundary_cuts_mean_shift.value_or(true);
  }

  // ``sddp_named_cuts_file`` accessor retired 2026-05 along with the
  // CSV named-cut path; hot-start cuts come from ``cuts_input_file``
  // (Parquet) only.

  /// Maximum retained cuts per (scene, phase) LP.  0 = unlimited (default).
  [[nodiscard]] constexpr auto sddp_max_cuts_per_phase() const
  {
    return m_options_.sddp_options.max_cuts_per_phase.value_or(0);
  }

  /// Iterations between cut pruning passes.  Default: 10.
  [[nodiscard]] constexpr auto sddp_cut_prune_interval() const
  {
    return m_options_.sddp_options.cut_prune_interval.value_or(10);
  }

  /// Dual threshold for inactive cut detection.  Default: 1e-8.
  [[nodiscard]] constexpr auto sddp_prune_dual_threshold() const
  {
    return m_options_.sddp_options.prune_dual_threshold.value_or(1e-8);
  }

  /// Use single cut storage (per-scene only).  Default: false.
  [[nodiscard]] constexpr auto sddp_single_cut_storage() const
  {
    return m_options_.sddp_options.single_cut_storage.value_or(false);
  }

  /// Maximum stored cuts per scene.  Default: 0 (unlimited).
  [[nodiscard]] constexpr auto sddp_max_stored_cuts() const
  {
    return m_options_.sddp_options.max_stored_cuts.value_or(0);
  }

  /// Low memory mode: off or compress (default for SDDP/cascade).
  /// Resolved default is `compress` so SDDP/cascade runs release the solver
  /// backend between solves and keep an in-memory compressed flat-LP
  /// snapshot.  Set explicitly to `LowMemoryMode::off` (or `--memory-saving
  /// off`) to keep the solver backend resident — typically only needed for
  /// debugging the backend across solves.
  ///
  /// Environment override: `GTOPT_MEMORY_MODE=off|compress` (aliases
  /// `snapshot`/`rebuild` → `compress`) forces the mode GLOBALLY, taking
  /// precedence over the per-run option.  Useful for test sweeps and
  /// benchmarking the whole suite under one memory mode.  Parsed once
  /// (thread-safe static init); an unset or unrecognised value falls back
  /// to the per-run option / default.
  [[nodiscard]] auto sddp_low_memory() const -> LowMemoryMode
  {
    if (const char* const v = std::getenv("GTOPT_MEMORY_MODE"); v != nullptr) {
      if (const auto forced = enum_from_name<LowMemoryMode>(v)) {
        return *forced;
      }
    }
    return m_options_.sddp_options.low_memory_mode.value_or(
        LowMemoryMode::compress);
  }

  /// In-memory compression codec for low_memory level 2.
  [[nodiscard]] constexpr auto sddp_memory_codec() const
  {
    return m_options_.sddp_options.memory_codec.value_or(
        CompressionCodec::auto_select);
  }

  /** @brief Maximum async iteration spread (default: 0 = synchronous).
   *
   * When > 0 and ``cut_sharing == none``, the SDDP solver takes the
   * **async** path (``solve_async``): each scene progresses through its
   * own forward / backward iteration loop as ``SDDPWorkPool`` tasks, the
   * pool's ``SDDPTaskKey`` priority schedules the slowest-iter scenes
   * first, and the spread between the fastest and slowest scene is
   * bounded by this value.
   *
   * Default **0** routes to the synchronous coordinator path instead:
   * one driver thread per scene per pass, lockstep across iterations.
   * Benchmarking on juan/IPLP (2026-06) found the coordinator/lockstep
   * path faster at every level (warmup −19 %, uninodal −35 %, transport
   * −38 %, bounds identical) because the async path pays an
   * after-convergence overshoot — once a level converges every scene
   * computes and discards a full extra iteration — and funnels all
   * solves through the shared pool's burst-submit dispatch.  In the
   * lockstep path scene-level priority is moot (all scenes share an
   * iteration per pass), so nothing is lost by retiring async as the
   * default.  Set ``> 0`` only when in-flight task heterogeneity (e.g.
   * heavily unbalanced scene complexity) actually justifies overlapping
   * iterations; ``1`` is the tightest non-trivial spread.
   */
  [[nodiscard]] constexpr auto sddp_max_async_spread() const
  {
    return m_options_.sddp_options.max_async_spread.value_or(0);
  }

  /** @brief SDDP work pool CPU over-commit factor (default: 4.0). */
  [[nodiscard]] constexpr auto sddp_pool_cpu_factor() const
  {
    return m_options_.sddp_options.pool_cpu_factor.value_or(4.0);
  }

  /** @brief Whether the user explicitly set the SDDP pool CPU factor.
   *
   * When false, `SDDPMethod` may auto-cap the resolved factor to 1.0 for
   * many-scene runs (the scene×aperture backward tasks already saturate
   * every core, so over-commit only adds scheduler contention).  When
   * true, the resolved value is honored verbatim. */
  [[nodiscard]] constexpr bool sddp_pool_cpu_factor_is_set() const
  {
    return m_options_.sddp_options.pool_cpu_factor.has_value();
  }

  /** @brief LP-build work-pool CPU over-commit factor (default: 2.0).
   *
   * Currently shares the `sddp_options.pool_cpu_factor` field with the
   * SDDP pool so that a single `--cpu-factor` CLI flag controls both
   * pools uniformly.  When the user does **not** set `--cpu-factor`,
   * the LP-build pool falls back to a more conservative 2.0× factor
   * (vs the SDDP pool's 4.0×) — reflecting that LP build is memory-
   * heavy (constructing full SystemLP trees) while SDDP solving is
   * CPU-bound (solver calls).  When the CLI flag is set, both pools
   * pick it up and the user can force a 1-thread serial baseline via
   * `--cpu-factor 0.025` on a typical 20-core box.
   */
  [[nodiscard]] constexpr auto build_pool_cpu_factor() const
  {
    return m_options_.sddp_options.pool_cpu_factor.value_or(2.0);
  }

  /** @brief SDDP work pool memory limit in MB (0 = no limit). */
  [[nodiscard]] constexpr auto sddp_pool_memory_limit_mb() const
  {
    return m_options_.sddp_options.pool_memory_limit_mb.value_or(0.0);
  }

  /** @brief How update_lp elements obtain reservoir/battery volume between
   *  phases (affects seepage, production factor, discharge limit only).
   *  Default: warm_start (no cross-phase lookup). */
  [[nodiscard]] constexpr auto sddp_state_variable_lookup_mode() const
  {
    return m_options_.sddp_options.state_variable_lookup_mode.value_or(
        StateVariableLookupMode::warm_start);
  }

  /**
   * @brief Gets the SDDP convergence mode.
   * @return ConvergenceMode enum (default: gap_stationary).  The
   *         statistical CI test (``ConvergenceMode::statistical``) is
   *         opt-in — it relies on ``convergence_confidence > 0`` and
   *         is too easily fooled by heterogeneous-scene σ scatter
   *         (Maule/juan).  Gap + stationary safety net is the
   *         right default for most users.
   */
  [[nodiscard]] constexpr auto sddp_convergence_mode() const
  {
    return m_options_.sddp_options.convergence_mode.value_or(
        ConvergenceMode::gap_stationary);
  }

  /**
   * @brief Gets the stationary-gap convergence tolerance.
   *
   * When positive, enables the secondary convergence criterion: if the
   * relative change in the gap over the last `stationary_window` iterations
   * falls below this threshold, the solver declares convergence even when the
   * gap exceeds `convergence_tol`.  Default: 0.0 (disabled).
   */
  [[nodiscard]] constexpr auto sddp_stationary_tol() const
  {
    return m_options_.sddp_options.stationary_tol.value_or(
        default_sddp_stationary_tol);
  }

  /**
   * @brief Gets the look-back window for stationary gap detection.
   * @return Number of iterations to look back (default: 10)
   */
  [[nodiscard]] constexpr auto sddp_stationary_window() const
  {
    return m_options_.sddp_options.stationary_window.value_or(
        default_sddp_stationary_window);
  }

  /**
   * @brief Gets the confidence level for statistical convergence.
   * @return Confidence level (0-1), 0.0 = disabled (default)
   */
  [[nodiscard]] constexpr auto sddp_convergence_confidence() const
  {
    return m_options_.sddp_options.convergence_confidence.value_or(
        default_sddp_convergence_confidence);
  }

  /**
   * @brief Gets the absolute gap ceiling for the secondary convergence tests.
   * @return Ceiling in [0, 1]; 1.0 disables the guard.
   */
  [[nodiscard]] constexpr auto sddp_stationary_gap_ceiling() const
  {
    return m_options_.sddp_options.stationary_gap_ceiling.value_or(
        default_sddp_stationary_gap_ceiling);
  }

  /**
   * @brief Gets the consecutive-structural-failure threshold for terminal-skip.
   * @return Iter count; 0 disables the terminal-skip mechanism.
   */
  [[nodiscard]] constexpr auto sddp_terminal_failure_threshold() const
  {
    return m_options_.sddp_options.terminal_failure_threshold.value_or(
        default_sddp_terminal_failure_threshold);
  }

  // ── Cascade options ─────────────────────────────────────────────────────

  /// Cascade level configurations.  Empty → use built-in defaults.
  [[nodiscard]] const auto& cascade_levels() const noexcept
  {
    return m_options_.cascade_options.level_array;
  }

  /// Whether the user specified cascade levels.
  [[nodiscard]] bool has_cascade_levels() const noexcept
  {
    return !m_options_.cascade_options.level_array.empty();
  }

  /// Global cascade SDDP options (serve as defaults for all levels).
  [[nodiscard]] const auto& cascade_sddp_options() const noexcept
  {
    return m_options_.cascade_options.sddp_options;
  }

  // ── Enum-typed accessors ──────────────────────────────────────────────────

  /// Solver type as an enum (MethodType::monolithic or MethodType::sddp).
  [[nodiscard]] constexpr auto method_type_enum() const -> MethodType
  {
    return m_options_.method.value_or(default_method_type);
  }

  /// LP-build mode as an enum.  Defaults to `scene_parallel` — the
  /// pre-00c605d7 per-scene work-pool submission (coarse granularity,
  /// lower pool/malloc-arena contention).  Users may opt into
  /// `full_parallel` via `--build-mode full-parallel` for maximum
  /// concurrency, or `serial` for a genuine in-thread baseline.
  [[nodiscard]] constexpr auto build_mode_enum() const -> BuildMode
  {
    return m_options_.build_mode.value_or(BuildMode::scene_parallel);
  }

  /// SDDP boundary cuts mode as an enum.
  [[nodiscard]] constexpr auto sddp_boundary_cuts_mode_enum() const
      -> BoundaryCutsMode
  {
    return m_options_.sddp_options.boundary_cuts_mode.value_or(
        BoundaryCutsMode::separated);
  }

  /// How terminal/boundary cuts are shared across scenes on the terminal α.
  /// Explicit `boundary_cut_sharing_mode` wins; otherwise derived from the
  /// legacy `boundary_cuts_mode` scope (combined→shared, else per_scene).
  [[nodiscard]] constexpr auto sddp_boundary_cut_sharing_mode_enum() const
      -> BoundaryCutSharingMode
  {
    if (const auto& m = m_options_.sddp_options.boundary_cut_sharing_mode) {
      return *m;
    }
    return sddp_boundary_cuts_mode_enum() == BoundaryCutsMode::combined
        ? BoundaryCutSharingMode::shared
        : BoundaryCutSharingMode::per_scene;
  }

  /// SDDP cut recovery mode as an enum.
  [[nodiscard]] constexpr auto sddp_cut_recovery_mode_enum() const
      -> HotStartMode
  {
    return m_options_.sddp_options.cut_recovery_mode.value_or(
        HotStartMode::none);
  }

  /// SDDP recovery mode as an enum.
  [[nodiscard]] constexpr auto sddp_recovery_mode_enum() const -> RecoveryMode
  {
    return m_options_.sddp_options.recovery_mode.value_or(RecoveryMode::full);
  }

  /// Monolithic solve mode as an enum.
  [[nodiscard]] constexpr auto monolithic_solve_mode_enum() const -> SolveMode
  {
    return m_options_.monolithic_options.solve_mode.value_or(
        SolveMode::monolithic);
  }

  /// Monolithic boundary cuts mode as an enum.
  [[nodiscard]] constexpr auto monolithic_boundary_cuts_mode_enum() const
      -> BoundaryCutsMode
  {
    return m_options_.monolithic_options.boundary_cuts_mode.value_or(
        BoundaryCutsMode::separated);
  }

  /// Terminal-α sharing for monolithic boundary cuts.  When unset, derive
  /// from the boundary-cuts mode (combined → shared, else per_scene),
  /// mirroring `sddp_boundary_cut_sharing_mode_enum`.
  [[nodiscard]] constexpr auto monolithic_boundary_cut_sharing_mode_enum() const
      -> BoundaryCutSharingMode
  {
    if (const auto& m = m_options_.monolithic_options.boundary_cut_sharing_mode)
    {
      return *m;
    }
    return monolithic_boundary_cuts_mode_enum() == BoundaryCutsMode::combined
        ? BoundaryCutSharingMode::shared
        : BoundaryCutSharingMode::per_scene;
  }

  /// Validate all enum-typed option fields and return a list of warnings.
  ///
  /// Since enum fields are now typed (`std::optional<EnumType>`), invalid
  /// JSON strings are silently dropped to `nullopt` during parsing.  This
  /// method detects those cases and reports them.  Callers should log
  /// warnings at an appropriate level (typically WARN).
  [[nodiscard]] static auto validate_enum_options() -> std::vector<std::string>
  {
    // With typed enum fields, validation is no longer needed at this level —
    // invalid JSON strings fail to parse and the optional stays nullopt
    // (which falls back to the default).  Return empty for API compatibility.
    return {};
  }

  /**
   * @brief Gets the variable scale map built from variable_scales entries.
   *
   * The map provides `lookup(class_name, variable, uid)` to resolve scale
   * factors with per-element > per-class > default (1.0) priority.
   *
   * Global scales (`scale_theta`, `scale_alpha`) are auto-injected into
   * this map by `populate_variable_scales()` at construction time.
   * Per-element fields (`Battery::energy_scale`, `Reservoir::energy_scale`)
   * still take precedence over map entries.
   */
  [[nodiscard]] const auto& variable_scale_map() const noexcept
  {
    return m_variable_scale_map_;
  }

private:
  /// @brief Populate variable_scales from dedicated scale options.
  ///
  /// Injects Bus.theta (from scale_theta) and Sddp.alpha (from scale_alpha)
  /// into the variable_scales array unless the user already provided them.
  /// Convention: `physical = LP × scale` for all entries.
  static auto populate_variable_scales(PlanningOptions& opts)
      -> std::span<const VariableScale>
  {
    auto has_entry = [&](std::string_view cls, std::string_view var) -> bool
    {
      return std::ranges::any_of(opts.variable_scales,
                                 [&](const VariableScale& vs)
                                 {
                                   return vs.class_name == cls
                                       && vs.variable == var
                                       && vs.uid == unknown_uid;
                                 });
    };

    // Inject Bus.theta — scale_theta already follows physical = LP × scale.
    // Post-§11 (2026-05-17): legacy top-level `opts.scale_theta` removed;
    // single source is `opts.model_options.scale_theta`.
    if (!has_entry(Bus::class_name, "theta")) {
      const auto st = opts.model_options.scale_theta.value_or(1.0);
      opts.variable_scales.push_back(VariableScale {
          .class_name {Bus::class_name.full_name()},
          .variable {"theta"},
          .scale = st,
      });
    }

    // Inject Sddp.alpha — only when user provided an explicit scale_alpha.
    // When scale_alpha is unset (auto-scale), the SDDP method computes
    // it at runtime from max(var_scale) and sets col_scale directly.
    if (!has_entry(sddp_class_name, "alpha")
        && opts.sddp_options.scale_alpha.has_value())
    {
      opts.variable_scales.push_back(VariableScale {
          .class_name {sddp_class_name},
          .variable {"alpha"},
          .scale = *opts.sddp_options.scale_alpha,
      });
    }

    return opts.variable_scales;
  }

  /** @brief The wrapped PlanningOptions object */
  PlanningOptions m_options_;
  /** @brief Variable scale map built from PlanningOptions::variable_scales */
  VariableScaleMap m_variable_scale_map_;
};

/// @brief Backward-compatibility alias (deprecated — use PlanningOptionsLP)
using OptionsLP = PlanningOptionsLP;

}  // namespace gtopt
