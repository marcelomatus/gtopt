/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_set>

#include <gtopt/as_label.hpp>
#include <gtopt/constraint_names.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/line.hpp>
#include <gtopt/lng_terminal.hpp>
#include <gtopt/lp_name_spill.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/memory_monitor.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_file_loader.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// `--memory-limit` is enforced entirely inside the pool factories
// (`make_solver_work_pool` / `make_sddp_work_pool`): with a limit the pool
// starts at one thread and grows toward the CPU ceiling under the live
// measured-memory controller in `BasicWorkPool` (idle-floor + measured
// marginal per-task cost + rate-limited admission).  There are NO a-priori
// per-task size estimates anywhere — this replaced the hardcoded
// kBaselineReservedMB / kLpBuildPerTaskMB / kWriteOutPerTaskMB constants
// that systematically mis-estimated on larger models.

// ── Line impedance validation helpers ─────────────────────────────────────

namespace
{
// Representative scalar voltage for a line: the per-unit default 1.0 when
// unset; the max non-zero entry of a vector schedule (→ smallest per-unit
// ratio → most conservative legitimacy probe); std::nullopt for a FileSched
// (cannot be validated statically).  Shared by the reactance (DC promotion)
// and resistance (lossless promotion) validators.
[[nodiscard]] std::optional<double> line_repr_voltage(const Line& line)
{
  if (!line.voltage.has_value()) {
    return 1.0;
  }
  const auto& sched = *line.voltage;
  if (std::holds_alternative<double>(sched)) {
    return std::get<double>(sched);
  }
  if (std::holds_alternative<std::vector<double>>(sched)) {
    double v_max = 0.0;
    for (const double v : std::get<std::vector<double>>(sched)) {
      v_max = std::max(v_max, std::abs(v));
    }
    return v_max > 0.0 ? std::optional<double> {v_max} : std::nullopt;
  }
  return std::nullopt;
}

// Rewrite to 0, in place, any per-unit schedule entry whose |value/V²| is below
// `threshold` on the `member` schedule of every line.  Returns the number of
// lines touched.  `what`/`action` flavour the warning text.  Backs both
// `validate_line_reactance` (member = &Line::reactance, the LP assembler then
// drops the θ-row) and `validate_line_resistance` (member = &Line::resistance,
// the loss assembler then takes its `R ≤ 0 → lossless` path and omits the
// loss-PWL).  The coefficient driven by `|value/V²|` is `∝ 1/(value/V²)`, so
// the dimensionally-correct probe is the per-unit ratio, not the raw value.
[[nodiscard]] std::size_t clamp_line_pu_schedule_below(
    Planning& planning,
    double threshold,
    OptTRealFieldSched Line::* member,
    std::string_view what,
    std::string_view action)
{
  std::size_t clamped = 0;
  for (auto& line : planning.system.line_array) {
    auto& opt = line.*member;
    if (!opt.has_value()) {
      continue;
    }
    const auto v_opt = line_repr_voltage(line);
    if (!v_opt.has_value() || *v_opt == 0.0) {
      continue;  // unable to compute per-unit ratio — skip.
    }
    const double v = *v_opt;
    const double v_sq = v * v;
    auto& sched = *opt;

    // Scalar schedule.
    if (std::holds_alternative<double>(sched)) {
      const double val = std::get<double>(sched);
      const double pu = val / v_sq;
      if (val != 0.0 && std::abs(pu) < threshold) {
        spdlog::warn(
            "(line_{0}) Line '{1}' (uid={2}): per-unit {0} {0}/V²={3:.3e} "
            "({0}={4:.3e}, V={5:.3e}) is below {6:.0e} — clamping to 0 ({7}).",
            what,
            line.name,
            line.uid,
            pu,
            val,
            v,
            threshold,
            action);
        sched = 0.0;
        ++clamped;
      }
      continue;
    }

    // Vector schedule: clamp any offending entry in place.
    if (std::holds_alternative<std::vector<double>>(sched)) {
      auto& vec = std::get<std::vector<double>>(sched);
      bool any_small = false;
      double min_bad = std::numeric_limits<double>::infinity();
      for (auto& val : vec) {
        const double pu = val / v_sq;
        if (val != 0.0 && std::abs(pu) < threshold) {
          any_small = true;
          min_bad = std::min(min_bad, std::abs(pu));
          val = 0.0;
        }
      }
      if (any_small) {
        spdlog::warn(
            "(line_{0}) Line '{1}' (uid={2}): {0} schedule has per-unit "
            "entries below {3:.0e} (min {0}/V²={4:.3e}, V={5:.3e}) — clamped "
            "to 0 ({6}).",
            what,
            line.name,
            line.uid,
            threshold,
            min_bad,
            v,
            action);
        ++clamped;
      }
      continue;
    }

    // FileSched — can't validate statically; leave alone.
  }
  return clamped;
}

// Raw-MW threshold below which a nonzero power/transmission bound is treated as
// degenerate noise and rewritten to exactly 0.0.  Unlike the per-unit X/R
// thresholds (which scale by V² and are exposed as model_options knobs), MW
// bounds carry no voltage so the probe is the raw value directly, and the
// threshold is a fixed compile-time constant — these bounds are physical MW and
// 1e-4 MW (= 0.1 W) is unambiguously "should be zero".  Clamping the tiny
// nonzero value to *exactly* zero lets the LP-assembly zero-column/row skip
// eliminate the dead column/row cleanly (see source/*_lp.cpp).
constexpr double k_power_bound_threshold = 1e-4;

// Rewrite to 0, in place, any nonzero schedule entry whose |value| is below
// `threshold` on the `member` schedule of every element in `array`.  Returns
// the number of elements touched.  Handles the three `OptTBRealFieldSched`
// alternatives: scalar `double`, 2-D `vector<vector<double>>` (each entry
// clamped), and `FileSched` (skipped — can't validate statically).  Warns once
// per element, mirroring `clamp_line_pu_schedule_below`.  `kind`/`field`
// flavour the warning text (e.g. "Generator"/"pmax").
template<class Element>
[[nodiscard]] std::size_t clamp_tb_sched_below(
    std::vector<Element>& array,
    double threshold,
    OptTBRealFieldSched Element::* member,
    std::string_view kind,
    std::string_view field)
{
  std::size_t clamped = 0;
  for (auto& elem : array) {
    auto& opt = elem.*member;
    if (!opt.has_value()) {
      continue;
    }
    auto& sched = *opt;

    // Scalar schedule.
    if (std::holds_alternative<double>(sched)) {
      const double val = std::get<double>(sched);
      if (val != 0.0 && std::abs(val) < threshold) {
        spdlog::warn(
            "({0}_{1}) {0} '{2}' (uid={3}): {1}={4:.3e} MW is below {5:.0e} "
            "MW — clamping to 0.",
            kind,
            field,
            elem.name,
            elem.uid,
            val,
            threshold);
        sched = 0.0;
        ++clamped;
      }
      continue;
    }

    // 2-D vector schedule: clamp any offending entry in place.
    if (std::holds_alternative<std::vector<std::vector<double>>>(sched)) {
      auto& mat = std::get<std::vector<std::vector<double>>>(sched);
      bool any_small = false;
      double min_bad = std::numeric_limits<double>::infinity();
      for (auto& row : mat) {
        for (auto& val : row) {
          if (val != 0.0 && std::abs(val) < threshold) {
            any_small = true;
            min_bad = std::min(min_bad, std::abs(val));
            val = 0.0;
          }
        }
      }
      if (any_small) {
        spdlog::warn(
            "({0}_{1}) {0} '{2}' (uid={3}): {1} schedule has entries below "
            "{4:.0e} MW (min |{1}|={5:.3e}) — clamped to 0.",
            kind,
            field,
            elem.name,
            elem.uid,
            threshold,
            min_bad);
        ++clamped;
      }
      continue;
    }

    // FileSched — can't validate statically; leave alone.
  }
  return clamped;
}
}  // namespace

// ── Line reactance validation ────────────────────────────────────────────

void PlanningLP::validate_line_reactance(Planning& planning)
{
  // The Kirchhoff row coefficient is x_τ = τ · X / V² (per-unit
  // susceptance), so the dimensionally-correct outlier check is
  // |X/V²| < threshold rather than |X| < threshold (the previous
  // hardcoded 1e-4 raw-X check missed V-vs-kV unit-typo lines on
  // physical-unit inputs and was over-aggressive on per-unit inputs).
  //
  // Lines whose per-unit susceptance falls below the threshold are
  // almost always one of:
  //   1. data-entry mistakes (V entered in V instead of kV → V² 1e6×
  //      too small → x_pu 1e6× too small),
  //   2. HVDC links / phase-shifters / FACTS devices (X = 0 sentinel
  //      or X ≈ ε, which power-system planners genuinely model
  //      *without* a KVL coupling), or
  //   3. very-low-X transformers / busbar segments (KVL technically
  //      valid but contributes nothing — phase-angle drop is below
  //      solver tolerance anyway).
  // In every case the right action is to drop the line out of the
  // Kirchhoff matrix.  We do this by rewriting the line's reactance
  // schedule to scalar 0.0 — the LP assembler in
  // `kirchhoff_node_angle.cpp` then skips the θ-row for that line.
  //
  // Default threshold is `default_dc_line_reactance_threshold` (1e-6
  // p.u.).  Real transmission `x_pu ≥ 1e-3` (4 OOM above) and real
  // distribution `x_pu ≥ 1e-4` (2 OOM above) so genuine lines are
  // never falsely promoted.  Set
  // `model_options.dc_line_reactance_threshold = 0` to disable.
  //
  // Mirrors the plp2gtopt battery_writer clamping of input/output_efficiency.
  // Read the threshold directly from `model_options` — this pass runs
  // before `PlanningOptionsLP` is constructed (so before flat-to-model
  // migration), but the new option is only exposed via `model_options`
  // and has no deprecated flat field, so direct lookup is safe.
  const double threshold =
      planning.options.model_options.dc_line_reactance_threshold.value_or(
          PlanningOptionsLP::default_dc_line_reactance_threshold);
  if (!(threshold > 0.0)) {
    return;
  }

  const std::size_t clamped = clamp_line_pu_schedule_below(
      planning,
      threshold,
      &Line::reactance,
      "reactance",
      "line treated as DC/HVDC, no Kirchhoff constraint");

  if (clamped > 0) {
    spdlog::info(
        "  Line reactance validation: clamped {} line(s) with |x_pu|=|X/V²| "
        "below {:.0e} to zero.",
        clamped,
        threshold);
  }
}

// ── Line resistance validation ────────────────────────────────────────────

void PlanningLP::validate_line_resistance(Planning& planning)
{
  // The line loss-link row coefficient is `∝ 1/(R/V²)`, so a very-low-R line
  // injects a huge coefficient (the matrix max — e.g. 64102 for an `R/V²` of
  // 5e-8) for a physically negligible loss.  The dimensionally-correct outlier
  // check is therefore `|R/V²| < threshold`, mirroring
  // `validate_line_reactance` on `|X/V²|`.  Lines below the threshold are
  // almost always one of:
  //   1. data-entry mistakes (V in V instead of kV → R/V² 1e6× too small),
  //   2. transformers / busbar segments whose copper loss is below solver
  //      tolerance anyway, or
  //   3. lossless connectors (R = 0 sentinel).
  // In every case the right action is to drop the line out of the loss model.
  // We do this by rewriting the line's resistance schedule to scalar 0.0 — the
  // loss assembler in `line_losses` then takes its `R ≤ 0 → lossless` path and
  // omits the loss-PWL entirely (both the loss-pricing AND the
  // `line_loss*_link` rows), removing the outlier coefficients that blow up
  // `--no-scale` kappa.  Note: unlike the reactance floor (which the solver's
  // Ruiz equilibration can partially absorb under scaling), this matters most
  // on the unscaled path where no equilibration runs.
  //
  // Default threshold is `default_dc_line_resistance_threshold` (1e-6 p.u.).
  // Read directly from `model_options` — this pass runs before
  // `PlanningOptionsLP` is constructed; the option has no deprecated flat
  // field, so the direct lookup is safe.
  const double threshold =
      planning.options.model_options.dc_line_resistance_threshold.value_or(
          PlanningOptionsLP::default_dc_line_resistance_threshold);
  if (!(threshold > 0.0)) {
    return;
  }

  const std::size_t clamped = clamp_line_pu_schedule_below(
      planning,
      threshold,
      &Line::resistance,
      "resistance",
      "line treated as lossless, loss model dropped");

  if (clamped > 0) {
    spdlog::info(
        "  Line resistance validation: clamped {} line(s) with |r_pu|=|R/V²| "
        "below {:.0e} to zero.",
        clamped,
        threshold);
  }
}

// ── Power bound validation ─────────────────────────────────────────────────

void PlanningLP::validate_power_bounds(Planning& planning)
{
  // Generator pmax/pmin carried in raw MW.  A nonzero bound below
  // `k_power_bound_threshold` (1e-4 MW) is degenerate noise — almost always a
  // rounding artefact of an input pipeline (e.g. a capacity that should be 0
  // but ended up at 1e-5).  Rewriting it to *exactly* 0.0 lets the LP-assembly
  // zero-column skip drop the dead generation column cleanly.  Unlike the X/R
  // per-unit thresholds there is no voltage to divide by and no knob — the
  // probe is the raw value and the threshold is fixed.
  const std::size_t clamped =
      clamp_tb_sched_below(planning.system.generator_array,
                           k_power_bound_threshold,
                           &Generator::pmax,
                           "Generator",
                           "pmax")
      + clamp_tb_sched_below(planning.system.generator_array,
                             k_power_bound_threshold,
                             &Generator::pmin,
                             "Generator",
                             "pmin");

  if (clamped > 0) {
    spdlog::info(
        "  Power bound validation: clamped {} generator bound(s) with "
        "|value| below {:.0e} MW to zero.",
        clamped,
        k_power_bound_threshold);
  }
}

// ── Line transmission validation ───────────────────────────────────────────

void PlanningLP::validate_line_transmission(Planning& planning)
{
  // Line tmax_ab/tmax_ba carried in raw MW.  Same rationale as
  // `validate_power_bounds`: a nonzero transfer limit below
  // `k_power_bound_threshold` (1e-4 MW) is rewritten to exactly 0.0 so the
  // LP-assembly zero-row/column skip eliminates the dead flow column/capacity
  // row cleanly.
  const std::size_t clamped = clamp_tb_sched_below(planning.system.line_array,
                                                   k_power_bound_threshold,
                                                   &Line::tmax_ab,
                                                   "Line",
                                                   "tmax_ab")
      + clamp_tb_sched_below(planning.system.line_array,
                             k_power_bound_threshold,
                             &Line::tmax_ba,
                             "Line",
                             "tmax_ba");

  if (clamped > 0) {
    spdlog::info(
        "  Line transmission validation: clamped {} line bound(s) with "
        "|value| below {:.0e} MW to zero.",
        clamped,
        k_power_bound_threshold);
  }
}

// ── Adaptive scale_theta computation ──────────────────────────────────────

void PlanningLP::auto_scale_theta(Planning& planning)
{
  // Only compute when scale_theta is not explicitly set at any level.
  auto& opts = planning.options;
  // Global `auto_scale = false` kill switch (`--no-scale`): skip every
  // auto-scale heuristic so LP coefficients stay in raw physical units.
  if (!opts.model_options.auto_scale.value_or(true)) {
    return;
  }
  // Post-§11 (2026-05-17): the legacy top-level mirrors on
  // `PlanningOptions` were removed.  Single source is
  // `opts.model_options.*`.
  const auto& mo = opts.model_options;
  if (mo.scale_theta.has_value()) {
    return;
  }

  // Don't auto-scale when Kirchhoff is disabled or single-bus mode.
  if (mo.use_kirchhoff.has_value() && !*mo.use_kirchhoff) {
    return;
  }
  if (mo.use_single_bus.has_value() && *mo.use_single_bus) {
    return;
  }

  // Collect scalar x_tau = X / V² values from all lines.
  //
  // The Kirchhoff row emits theta_coeff = 1/|x_tau| (see line_lp.cpp:65-88).
  // The theta column is then multiplied by scale_theta in flatten() (see
  // linear_problem.cpp:358), so the assembled theta coefficient is
  // scale_theta / |x_tau|. To drive this to ~1 for the median line — which
  // makes row-max equilibration produce a near-identity Kirchhoff row — we
  // must pick scale_theta = median(|x_tau|) = median(X/V²).
  //
  // Collecting median(X) directly (the previous heuristic) is wrong because
  // it is computed in a space orthogonal to the true susceptance spread on
  // a mixed-voltage network: high-voltage lines (500 kV) have tiny x_tau
  // but arbitrary raw X, and the median picks the raw-X middle, not the
  // x_tau middle, yielding assembled coefficients ~1e7 instead of ~1.
  const auto sched_to_scalar = [](const auto& sched) -> double
  {
    if (std::holds_alternative<double>(sched)) {
      return std::get<double>(sched);
    }
    if (std::holds_alternative<std::vector<double>>(sched)) {
      const auto& vec = std::get<std::vector<double>>(sched);
      if (!vec.empty()) {
        return vec.front();
      }
    }
    return 0.0;
  };

  // Extract a representative scalar from a (per-stage, per-block)
  // tmax schedule.  Picks the maximum across all entries so the
  // resulting `theta_max` covers the worst-case line capacity
  // realisation (capacity-expansion bumps, peak-stage tmax, etc.).
  const auto tb_sched_to_max = [](const auto& sched) -> double
  {
    if (std::holds_alternative<double>(sched)) {
      return std::get<double>(sched);
    }
    if (std::holds_alternative<std::vector<std::vector<double>>>(sched)) {
      double m = 0.0;
      for (const auto& row : std::get<std::vector<std::vector<double>>>(sched))
      {
        for (const auto v : row) {
          m = std::max(m, v);
        }
      }
      return m;
    }
    return 0.0;
  };

  std::vector<double> x_taus;
  // Cumulative `Σ_l (tmax_l · x_τ_l)` — used as the topology-aware
  // upper bound on `|θ_a − θ_b|` along any path.  See the `theta_max`
  // assignment below for the rationale.
  //
  // Per-line contribution `tmax_l · x_τ_l` is clamped at
  // ``kPerLineThetaContrib`` so a single "no hard cap" sentinel
  // (``tmax_ab >= 1e5`` emitted by converters when a line has no
  // explicit transmission limit) cannot inflate the global
  // ``theta_max`` into the 10^5+ range and destabilise the LP's
  // angle-column bounds (CPLEX/IPM numerics degrade once
  // ``|θ| ≫ 10^3``).  The clamp is generous (`1e3` rad ≈ 160 full
  // rotations) so real fixtures, where per-line contributions are
  // O(10-100) rad, are unaffected.
  constexpr double kPerLineThetaContrib = 1e3;
  double theta_max_accum = 0.0;
  std::size_t clamped_lines = 0;
  for (const auto& line : planning.system.line_array) {
    if (!line.reactance.has_value()) {
      continue;
    }
    const double x = sched_to_scalar(*line.reactance);
    if (x <= 0.0) {
      continue;
    }
    // Voltage defaults to 1.0 (per-unit mode) when omitted — matches the
    // convention in line_lp.cpp:62 and include/gtopt/line.hpp:63-65.
    double v = 1.0;
    if (line.voltage.has_value()) {
      const double vs = sched_to_scalar(*line.voltage);
      if (vs > 0.0) {
        v = vs;
      }
    }
    const double x_tau = x / (v * v);
    if (x_tau > 0.0) {
      x_taus.push_back(x_tau);
      // Per-line max capacity across both directions and all blocks.
      double tmax = 0.0;
      if (line.tmax_ab.has_value()) {
        tmax = std::max(tmax, tb_sched_to_max(*line.tmax_ab));
      }
      if (line.tmax_ba.has_value()) {
        tmax = std::max(tmax, tb_sched_to_max(*line.tmax_ba));
      }
      const double contrib = tmax * x_tau;
      if (contrib > kPerLineThetaContrib) {
        ++clamped_lines;
      }
      theta_max_accum += std::min(contrib, kPerLineThetaContrib);
    }
  }
  if (clamped_lines > 0) {
    spdlog::info(
        "  Clamped {} line(s) at theta-contrib ceiling {:.1f} rad — "
        "their tmax_ab/ba likely encodes a 'no hard cap' sentinel "
        "(e.g. converter default for missing Emergency flow limit). "
        "Soft-cap-with-penalty rows still constrain the actual flow.",
        clamped_lines,
        kPerLineThetaContrib);
  }

  if (x_taus.empty()) {
    return;
  }

  // Compute median x_tau = median(X/V²).
  std::ranges::sort(x_taus);
  const auto n = x_taus.size();
  const double median_x_tau = (n % 2 == 0)
      ? (x_taus[(n / 2) - 1] + x_taus[n / 2]) / 2.0
      : x_taus[n / 2];

  // scale_theta = median(|x_tau|) so that the assembled theta coefficient
  // scale_theta / |x_tau| ≈ 1 for the median line, and row-max
  // equilibration converges to a near-identity Kirchhoff row.
  opts.model_options.scale_theta = median_x_tau;
  spdlog::info(
      "  Auto scale_theta = {:.6g} (median of {} line x_tau = X/V² values)",
      median_x_tau,
      n);

  // theta_max = Σ_l (tmax_l · x_τ_l) — a topology-aware upper bound on
  // the largest possible θ-spread between any two buses in the
  // network.  Each line `l` carrying `f_l = tmax_l` contributes
  // `tmax_l · x_τ_l` to the angle drop along its endpoints; the
  // worst-case θ_a − θ_b across the whole network is at most the sum
  // of those drops along a single path, bounded above by the sum
  // over all lines.  Using this as the bound on every theta column
  // ensures `bus_lp.cpp::lazy_add_theta` never artificially caps line
  // flows below `tmax` (the ±2π hardcoded default would have done so
  // whenever `tmax · x_τ > 2π`, e.g. on IEEE 9-bus where
  // `250 MW · 0.0576 = 14.4 rad ≫ 2π = 6.28 rad`).  Honour an
  // explicit user override.
  if (!opts.model_options.theta_max.has_value()) {
    // Floor at default (`2π`) so very small networks don't end up
    // with theta_max ≪ 1 and lose simplex / IPM headroom.  Ceiling
    // at ``kThetaMaxCeiling`` (1e5 rad) keeps the angle column bound
    // numerically tractable even if many lines hit the per-line
    // contribution ceiling — CPLEX/HiGHS lose precision once
    // ``|θ| ≫ 1e5`` and the IPM normal-equation system becomes
    // ill-conditioned.  Real fixtures fall comfortably inside the
    // band (case14_base auto theta_max ≈ 2007 rad).
    constexpr double kThetaMaxCeiling = 1e5;
    const double theta_max = std::clamp(theta_max_accum,
                                        PlanningOptionsLP::default_theta_max,
                                        kThetaMaxCeiling);
    opts.model_options.theta_max = theta_max;
    if (theta_max_accum > kThetaMaxCeiling) {
      spdlog::info(
          "  Auto theta_max = {:.6g} (capped at {:.0g} — raw "
          "Σ_l tmax_l · x_τ_l = {:.6g} exceeded sanity ceiling)",
          theta_max,
          kThetaMaxCeiling,
          theta_max_accum);
    } else {
      spdlog::info("  Auto theta_max = {:.6g}  (Σ_l tmax_l · x_τ_l = {:.6g})",
                   theta_max,
                   theta_max_accum);
    }
  }

  // P0-3 — unit-consistency validation.
  //
  // The Kirchhoff coefficient x_tau = τ·X/V² must live in a single unit
  // system across the whole network. When a model mixes per-unit lines
  // (V=1) with physical lines (V in kV, X in Ω), the x_tau values span
  // many orders of magnitude and no choice of scale_theta can rescue
  // conditioning. Catch that here, at load time, so the user sees the
  // problem before a spurious LP is solved.
  const double min_x_tau = x_taus.front();
  const double max_x_tau = x_taus.back();
  if (min_x_tau > 0.0) {
    const double spread = max_x_tau / min_x_tau;
    constexpr double kSpreadWarn = 1e4;
    constexpr double kSpreadError = 1e6;
    if (spread > kSpreadError) {
      spdlog::error(
          "  Line x_tau = X/V² spread is {:.3e} (max={:.3e}, min={:.3e}) "
          "across {} lines — this strongly suggests mixed unit systems "
          "(per-unit vs kV/Ω). LP conditioning will be unrecoverable. "
          "Check Line.voltage and Line.reactance fields.",
          spread,
          max_x_tau,
          min_x_tau,
          n);
    } else if (spread > kSpreadWarn) {
      spdlog::warn(
          "  Line x_tau = X/V² spread is {:.3e} (max={:.3e}, min={:.3e}) "
          "across {} lines — this may indicate inconsistent unit systems "
          "and will cap LP conditioning quality. Check Line.voltage and "
          "Line.reactance fields.",
          spread,
          max_x_tau,
          min_x_tau,
          n);
    }
  }
}

// ── Scenario probability renormalisation ─────────────────────────────────

void PlanningLP::renormalize_scenario_probabilities(Planning& planning)
{
  auto& scenario_array = planning.simulation.scenario_array;
  auto& scene_array = planning.simulation.scene_array;

  if (scenario_array.empty()) {
    return;
  }

  // Build a per-scenario "is in an active scene" mask.  A scenario is
  // part of the active subproblem iff (a) its parent scene is active
  // (or scene_array is empty, in which case ``create_scene_array``
  // synthesises a single default scene covering all scenarios) AND
  // (b) the scenario itself has ``active`` not explicitly set to
  // false.  We rescale only the scenarios in this set.
  std::vector<bool> in_active_scene(scenario_array.size(), false);
  if (scene_array.empty()) {
    std::ranges::fill(in_active_scene, true);
  } else {
    for (const auto& scene : scene_array) {
      if (!scene.is_active()) {
        continue;
      }
      const auto first = scene.first_scenario;
      const auto count = (scene.count_scenario == std::dynamic_extent)
          ? (scenario_array.size() - first)
          : scene.count_scenario;
      const auto end = std::min(first + count, scenario_array.size());
      for (auto i = first; i < end; ++i) {
        in_active_scene[i] = true;
      }
    }
  }

  // Sum probability mass over the active subset.
  double total = 0.0;
  std::size_t n_active = 0;
  for (std::size_t i = 0; i < scenario_array.size(); ++i) {
    if (!in_active_scene[i]) {
      continue;
    }
    const auto& sc = scenario_array[i];
    if (!sc.is_active()) {
      continue;
    }
    total += sc.probability_factor.value_or(1.0);
    ++n_active;
  }

  // Three outcomes:
  //  * total == 0 → degenerate input (every active scenario has
  //    probability_factor == 0).  Cannot rescale; warn loudly.
  //  * |total - 1| <= 1e-12 → already normalised; silent no-op
  //    (this is the common path for fresh JSON whose authors put
  //    in pre-computed probabilities that already sum to 1).
  //  * otherwise → rescale.
  constexpr double kTotalTol = 1e-12;
  if (total <= 0.0) {
    if (n_active > 0) {
      SPDLOG_WARN(
          "Scenario probabilities: every one of {} active scenario(s) has "
          "probability_factor == 0; LP cost coefficients will be all-zero "
          "for the cost-to-go objective.  Check the input JSON.",
          n_active);
    }
    return;
  }
  if (std::abs(total - 1.0) <= kTotalTol) {
    SPDLOG_DEBUG(
        "Scenario probabilities: {} active scenario(s) already sum to 1.0",
        n_active);
    return;
  }

  // Rescale every in-scope scenario's probability_factor by 1/total.
  // Out-of-scope scenarios (those in inactive scenes or themselves
  // marked inactive) keep their original values — they don't enter
  // any LP, so the values are inconsequential, but leaving them
  // untouched preserves round-trip readability of the JSON.
  const double scale = 1.0 / total;
  for (std::size_t i = 0; i < scenario_array.size(); ++i) {
    if (!in_active_scene[i]) {
      continue;
    }
    auto& sc = scenario_array[i];
    if (!sc.is_active()) {
      continue;
    }
    sc.probability_factor = sc.probability_factor.value_or(1.0) * scale;
  }
  SPDLOG_INFO(
      "Scenario probabilities: renormalised {} active scenario(s) — total "
      "mass {:.6f} → 1.0 (scale = {:.6g})",
      n_active,
      total,
      scale);
}

// ── Adaptive line-loss row scaling ───────────────────────────────────────

void PlanningLP::auto_scale_loss_link(Planning& planning)
{
  auto& opts = planning.options;

  // Global kill switch: respect `auto_scale = false` (`--no-scale`).
  if (!opts.model_options.auto_scale.value_or(true)) {
    return;
  }
  // Honour any explicit setting (model_options or top-level not exposed
  // for this option — model_options is the only path).
  if (opts.model_options.scale_loss_link.has_value()) {
    return;
  }

  // Skip when single-bus mode.  Post-§11: legacy top-level mirror
  // removed; single source is model_options.
  if (opts.model_options.use_single_bus.value_or(false)) {
    return;
  }

  const auto sched_to_scalar = [](const auto& sched) -> double
  {
    if (std::holds_alternative<double>(sched)) {
      return std::get<double>(sched);
    }
    if (std::holds_alternative<std::vector<double>>(sched)) {
      const auto& vec = std::get<std::vector<double>>(sched);
      if (!vec.empty()) {
        return vec.front();
      }
    }
    return 0.0;
  };

  // Collect R/V² values across every line that contributes a PWL loss row.
  // The loss-link row's smallest coefficient is loss_1 = seg_width · R / V²;
  // for global row scaling we drop seg_width (per-line / per-block) and
  // pick a power-of-10 multiplier from median(R/V²) so the per-unit factor
  // is lifted symmetrically across the network.
  std::vector<double> r_over_v2;
  r_over_v2.reserve(planning.system.line_array.size());
  for (const auto& line : planning.system.line_array) {
    if (!line.resistance.has_value()) {
      continue;
    }
    const double R = sched_to_scalar(*line.resistance);
    if (R <= 0.0) {
      continue;
    }
    double v = 1.0;
    if (line.voltage.has_value()) {
      const double vs = sched_to_scalar(*line.voltage);
      if (vs > 0.0) {
        v = vs;
      }
    }
    const double V2 = v * v;
    if (V2 <= 0.0) {
      continue;
    }
    r_over_v2.push_back(R / V2);
  }

  if (r_over_v2.empty()) {
    return;
  }

  std::ranges::sort(r_over_v2);
  const auto n = r_over_v2.size();
  const double median_rv2 = (n % 2 == 0)
      ? (r_over_v2[(n / 2) - 1] + r_over_v2[n / 2]) / 2.0
      : r_over_v2[n / 2];

  if (median_rv2 <= 0.0) {
    return;
  }

  // Power-of-10 lift: pick s = 10^round(−log10(median(R/V²))).  This puts
  // the median line's seg_1 coefficient (≈ seg_width · median_rv2) within a
  // factor of √10 of `seg_width`, which is in MW for typical PLP cases.
  // Clamp to [1, 1e9] so we don't overscale a model that already has
  // R/V² ≈ 1 (per-unit data).
  const double exponent = std::round(-std::log10(median_rv2));
  const double scale_raw = std::pow(10.0, exponent);
  const double scale = std::clamp(scale_raw, 1.0, 1.0e9);

  if (scale <= 1.0) {
    // Already well-conditioned (per-unit input or large R/V²).  Don't set
    // the option; downstream falls back to 1.0 (no-op).
    spdlog::info(
        "  Auto scale_loss_link skipped: median(R/V²) = {:.3e} across "
        "{} line(s) — no lift needed",
        median_rv2,
        n);
    return;
  }

  opts.model_options.scale_loss_link = scale;
  spdlog::info(
      "  Auto scale_loss_link = {:.3e} (median R/V² = {:.3e} across "
      "{} line(s); seg_1 lifts to ~seg_width · {:.3e})",
      scale,
      median_rv2,
      n,
      median_rv2 * scale);
}

// ── Adaptive reservoir energy scaling ────────────────────────────────────

void PlanningLP::auto_scale_reservoirs(Planning& planning,
                                       double solver_infinity)
{
  auto& opts = planning.options;
  auto& sys = planning.system;

  // Global `auto_scale = false` kill switch (`--no-scale`): skip every
  // auto-scale heuristic so LP coefficients stay in raw physical units.
  if (!opts.model_options.auto_scale.value_or(true)) {
    return;
  }

  // Build a set of UIDs already covered by explicit variable_scales entries.
  auto has_entry = [&](Uid uid) -> bool
  {
    return std::ranges::any_of(opts.variable_scales,
                               [uid](const VariableScale& vs)
                               {
                                 return vs.class_name == Reservoir::class_name
                                     && vs.variable == "energy"
                                     && vs.uid == uid;
                               });
  };

  // Helper: extract a representative scalar from a FieldSched optional.
  // Handles per-stage 1-D (``OptTRealFieldSched``) and per-(stage, block)
  // 2-D (``OptTBRealFieldSched``) shapes uniformly.  The 2-D shape
  // collapses to the maximum across all (stage, block) cells, mirroring
  // the per-stage helper that takes ``max`` across the per-stage vector.
  auto scalar_of = []<typename OptFS>(const OptFS& fs) -> std::optional<double>
  {
    if (!fs.has_value()) {
      return std::nullopt;
    }
    if (std::holds_alternative<double>(*fs)) {
      return std::get<double>(*fs);
    }
    if constexpr (std::is_same_v<OptFS, OptTRealFieldSched>) {
      if (std::holds_alternative<std::vector<Real>>(*fs)) {
        const auto& vec = std::get<std::vector<Real>>(*fs);
        if (!vec.empty()) {
          return *std::ranges::max_element(vec);
        }
      }
    } else if constexpr (std::is_same_v<OptFS, OptTBRealFieldSched>) {
      using Mat = std::vector<std::vector<Real>>;
      if (std::holds_alternative<Mat>(*fs)) {
        const auto& mat = std::get<Mat>(*fs);
        std::optional<double> mx;
        for (const auto& row : mat) {
          for (const auto v : row) {
            mx = mx ? std::max(*mx, v) : v;
          }
        }
        return mx;
      }
    }
    return std::nullopt;  // FileSched — can't resolve statically
  };

  // Round v UP to the next power of 10 (clamped so v ≤ 1 yields 1.0).
  // `physical = LP × scale` means LP = physical / scale; rounding up
  // to the next decade puts LP in (0.1, 1.0] for well-conditioned
  // simplex.
  //
  // `is_infinity(v, solver_infinity)` rejects sentinel "no-bound"
  // values (e.g. legacy `Reservoir.emax = 1e30` if it ever appears)
  // so they are NOT folded into `col_scale`.  The threshold comes
  // from the active solver's `infinity()` (CPLEX: 1e20, HiGHS / OSI
  // / Gurobi: 1e30) so the same code is correct under every
  // supported backend.
  auto scale_for = [solver_infinity](double v) -> double
  {
    if (!std::isfinite(v) || v <= 1.0 || is_infinity(v, solver_infinity)) {
      return 1.0;
    }
    return std::pow(10.0, std::ceil(std::log10(v)));
  };

  // ENERGY-ONLY auto-scaling.  Flow scales were intentionally REMOVED
  // 2026-04-30: `Reservoir.fmax` is typically a sentinel "no bound"
  // value (1e30) rather than a real physical magnitude, so deriving
  // `flow_scale = scale_for(fmax)` produced absurd `col_scale = 1e30`
  // entries that broke Benders cut numerics
  // (juan/gtopt_iplp regression).  Energy scales are still derived
  // from `emax / 1000` because emax is a real reservoir capacity
  // (hm³) for which a power-of-ten LP rescale is meaningful.  Users
  // who genuinely want a non-unit flow scale must declare it
  // explicitly via `PlanningOptions::variable_scales`.
  size_t energy_count = 0;
  for (const auto& rsv : sys.reservoir_array) {
    if (has_entry(rsv.uid)) {
      continue;
    }
    const auto emax = scalar_of(rsv.emax);
    if (!emax.has_value()) {
      continue;
    }
    // /1000 divisor keeps energy_scale in the band where Benders cut
    // state-var coefficients `rc * energy_scale / scale_obj` stay
    // O(1e-2)..O(1) for water values rc ~ O(1-100) $/hm³.  Without
    // it, energy_scale = 10^ceil(log10(1453)) = 10^4 drives cut rows
    // to κ ~ 10^10 once the cut pool accumulates.
    const double energy_scale = scale_for(*emax / 1000.0);
    if (energy_scale > 1.0) {
      opts.variable_scales.push_back(VariableScale {
          .class_name = Reservoir::class_name,
          .variable = "energy",
          .uid = rsv.uid,
          .scale = energy_scale,
          .name = rsv.name,
      });
      ++energy_count;
    }
  }

  if (energy_count > 0) {
    spdlog::info(
        "  Auto scale_reservoir: {} energy per-element scale(s) computed",
        energy_count);
  }
}

void PlanningLP::auto_scale_lng_terminals(Planning& planning,
                                          double solver_infinity)
{
  auto& opts = planning.options;
  auto& sys = planning.system;

  // Global `auto_scale = false` kill switch (`--no-scale`): skip every
  // auto-scale heuristic so LP coefficients stay in raw physical units.
  if (!opts.model_options.auto_scale.value_or(true)) {
    return;
  }

  auto has_entry = [&](Uid uid) -> bool
  {
    return std::ranges::any_of(opts.variable_scales,
                               [uid](const VariableScale& vs)
                               {
                                 return vs.class_name == LngTerminal::class_name
                                     && vs.variable == "energy"
                                     && vs.uid == uid;
                               });
  };

  // ``LngTerminal.emax`` is ``OptTBRealFieldSched`` (per-(stage, block))
  // since the 2026-05-18 storage-bound widening.  Extract a representative
  // maximum across all alternatives so the auto-scaling threshold check
  // (``emax > 1000``, ``emax < solver_infinity`` sentinel) keeps working.
  auto scalar_of = [](const OptTBRealFieldSched& fs) -> std::optional<double>
  {
    if (!fs.has_value()) {
      return std::nullopt;
    }
    if (std::holds_alternative<double>(*fs)) {
      return std::get<double>(*fs);
    }
    if (std::holds_alternative<std::vector<std::vector<Real>>>(*fs)) {
      const auto& mat = std::get<std::vector<std::vector<Real>>>(*fs);
      std::optional<double> best;
      for (const auto& row : mat) {
        for (const auto v : row) {
          best = best.has_value() ? std::max(*best, v) : v;
        }
      }
      return best;
    }
    return std::nullopt;
  };

  size_t count = 0;
  for (const auto& lng : sys.lng_terminal_array) {
    if (has_entry(lng.uid)) {
      continue;
    }
    const auto emax = scalar_of(lng.emax);
    if (!emax.has_value() || *emax <= 1000.0
        || is_infinity(*emax, solver_infinity))
    {
      // Skip unset, sub-threshold, AND sentinel-infinity values.  Same
      // rationale as `auto_scale_reservoirs::scale_for` — folding a
      // sentinel into col_scale propagates 1e30+ multipliers into LP
      // coefs and corrupts Benders cut numerics.
      continue;
    }
    const double raw = *emax / 1000.0;
    const double energy_scale = std::pow(10.0, std::ceil(std::log10(raw)));

    opts.variable_scales.push_back(VariableScale {
        .class_name = LngTerminal::class_name,
        .variable = "energy",
        .uid = lng.uid,
        .scale = energy_scale,
        .name = lng.name,
    });
    ++count;
  }

  if (count > 0) {
    spdlog::info(
        "  Auto scale_lng_terminal: computed energy scales for {} LNG "
        "terminals",
        count);
  }
}

void PlanningLP::tighten_scene_phase_links(phase_systems_t& phase_systems,
                                           SimulationLP& simulation)
{
  for (auto& sys : phase_systems) {
    auto& links = sys.pending_state_links();
    for (const auto& link : links) {
      auto prev_var = simulation.state_variable(link.prev_key);
      if (prev_var) {
        prev_var->get().add_dependent_variable(link.here_key, link.here_col);
      } else {
        // Producer-side StateVariable was never registered: the previous
        // phase's element either didn't run or chose not to publish an
        // efin/state column for this (scenario, stage).  Mirrors the
        // build-time warning that lived in storage_lp.hpp before the
        // deferred-linking refactor — kept here so cross-phase coupling
        // gaps remain visible.
        SPDLOG_WARN(
            "tighten_scene_phase_links: no producer StateVariable for "
            "deferred link (class='{}' col='{}' uid={} prev_stage_uid={} "
            "scene={} prev_phase={}). Cross-phase state coupling will be "
            "missing for this element.",
            link.prev_key.class_name,
            link.prev_key.col_name,
            link.prev_key.uid,
            link.prev_key.stage_uid,
            link.prev_key.lp_key.scene_index,
            link.prev_key.lp_key.phase_index);
      }
    }
    links.clear();
    links.shrink_to_fit();
  }

  // Drop write-out-only collections under compress mode.
  // `physical_eini_from_cache` reads `li.get_col_sol()[eini_col]`
  // directly from the current sys — no element lookup, no
  // cross-phase traversal, no StateVariable lookup.  All 22
  // non-HasUpdateLP `Collection<X>` types are therefore dead weight
  // after `add_to_lp` + `flatten`.  `write_out` calls
  // `rebuild_collections_if_needed()` to revive every type from the
  // parsed `System` data when needed.
  for (auto&& sys : phase_systems) {
    sys.clear_disposable_collections();
  }
}

void PlanningLP::build_aperture_systems(const LpMatrixOptions& flat_opts)
{
  // Aperture systems are a SDDP backward-pass construct only; nothing to do
  // for the monolithic single-solve method.
  if (m_options_.method_type_enum() == MethodType::monolithic) {
    return;
  }

  auto&& scenes = m_simulation_.scenes();
  auto&& phases = m_simulation_.phases();
  const auto num_scenes = static_cast<std::size_t>(std::ssize(scenes));
  const auto num_phases = static_cast<std::size_t>(std::ssize(phases));

  // Resolve the file for each phase: Phase override → global sddp_options.
  const auto global_name = m_options_.sddp_aperture_system_file();
  const OptName global_file =
      global_name.empty() ? OptName {} : OptName {global_name};

  StrongIndexVector<PhaseIndex, OptName> per_phase(num_phases);
  bool any = false;
  for (auto&& [pi, ph] : enumerate<PhaseIndex>(phases)) {
    per_phase[pi] =
        resolve_aperture_system_file(ph.aperture_system_file(), global_file);
    any = any || per_phase[pi].has_value();
  }
  if (!any) {
    return;  // leave m_aperture_systems_ empty → aperture_system() == nullptr
  }

  // Resolve solver name + low-memory hint exactly as create_systems does
  // (aperture systems only ever run under sddp/cascade).
  auto resolved_opts = flat_opts;
  if (resolved_opts.solver_name.empty()) {
    resolved_opts.solver_name =
        std::string(SolverRegistry::instance().default_solver());
  }
  if (resolved_opts.low_memory_mode == LowMemoryMode::off) {
    resolved_opts.low_memory_mode = m_options_.sddp_low_memory();
    resolved_opts.memory_codec = m_options_.sddp_memory_codec();
  }
  // Aperture cells spill their label metadata to the same run-lifetime store
  // (keyed per (kind, scene, phase) — the SystemKind discriminator keeps them
  // distinct from their forward siblings).
  resolved_opts.name_store = m_name_store_.get();

  const auto input_dir = std::string(m_options_.input_directory());

  // Load + preprocess each distinct reduced System once, keyed by resolved
  // path.  unordered_map gives reference stability, so the `System&` handed
  // to each SystemLP stays valid as more files are inserted.
  auto get_system = [&](const Name& file) -> System&
  {
    const auto key = resolve_system_file(file, input_dir).string();
    if (auto it = m_aperture_owned_systems_.find(key);
        it != m_aperture_owned_systems_.end())
    {
      return it->second;
    }
    auto loaded = load_system_with_model_options(file, input_dir);
    // NOTE: loaded.model_options is intentionally not applied yet — this
    // milestone swaps the System network only; the model_options override
    // (single-bus / no-Kirchhoff backward model) is a documented follow-up.
    System sys = std::move(loaded.system);
    sys.expand_batteries();
    sys.expand_reservoir_constraints();
    sys.fold_legacy_fuel_emission_factors();
    sys.expand_fuel_emission_sources();
    sys.expand_emission_sources();
    sys.fold_legacy_emission_rate();
    sys.fold_legacy_profiles();
    sys.setup_reference_bus(m_options_);
    return m_aperture_owned_systems_.emplace(key, std::move(sys)).first->second;
  };

  SPDLOG_INFO(
      "  Building aperture systems (SDDP backward-pass reduced model): "
      "{} distinct file(s)",
      [&]
      {
        std::unordered_set<std::string> files;
        for (auto&& f : per_phase) {
          if (f) {
            files.insert(*f);
          }
        }
        return files.size();
      }());

  m_aperture_systems_ = scene_phase_ap_t(num_scenes);
  for (auto&& [si, scene] : enumerate<SceneIndex>(scenes)) {
    auto& row = m_aperture_systems_[si];
    row = phase_ap_systems_t(num_phases);
    for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
      const auto& per_phase_sys = per_phase[pi];
      if (!per_phase_sys.has_value()) {
        continue;
      }
      row[pi].emplace(get_system(*per_phase_sys),
                      m_simulation_,
                      phase,
                      scene,
                      resolved_opts,
                      SystemKind::aperture);
    }
    // Resolve cross-phase aperture state links.  `state_variable(prev_key)`
    // self-routes to the aperture registry via `prev_key.lp_key.kind`
    // (stamped by SystemContext).  Sparse phases whose producer has no
    // aperture system simply find no producer — the backward pass falls
    // back to the forward system for those, so no warning is emitted here.
    for (auto& cell : row) {
      if (!cell.has_value()) {
        continue;
      }
      auto& links = cell->pending_state_links();
      for (const auto& link : links) {
        if (auto prev_var = m_simulation_.state_variable(link.prev_key)) {
          prev_var->get().add_dependent_variable(link.here_key, link.here_col);
        }
      }
      links.clear();
      links.shrink_to_fit();
    }
  }
}

auto PlanningLP::make_name_store(const LpMatrixOptions& flat_opts,
                                 const PlanningOptionsLP& options)
    -> std::unique_ptr<LpNameSpillStore>
{
  // Spill only when names are kept (lp_file / lp_debug / lp_error →
  // col/row_with_names) AND under a multi-cell method.  Monolithic keeps a
  // single resident LP whose label metadata is cheap and forces
  // low_memory=off (the spill site never fires), so a store there would just
  // be an idle worker thread + empty temp dir.
  const bool names = flat_opts.col_with_names || flat_opts.row_with_names;
  if (!names || options.method_type_enum() == MethodType::monolithic) {
    return nullptr;
  }
  return std::make_unique<LpNameSpillStore>(
      std::filesystem::path(options.log_directory()) / "lp_name_meta");
}

auto PlanningLP::create_systems(System& system,
                                SimulationLP& simulation,
                                const PlanningOptionsLP& options,
                                const LpMatrixOptions& flat_opts,
                                LpNameSpillStore* name_store)
    -> scene_phase_systems_t
{
  system.expand_batteries();
  system.expand_reservoir_constraints();
  system.fold_legacy_fuel_emission_factors();
  system.expand_fuel_emission_sources();
  system.expand_emission_sources();
  system.fold_legacy_emission_rate();
  system.fold_legacy_profiles();
  system.setup_reference_bus(options);

  // Enable per-cell AMPL variable registration when user constraints
  // exist (or any future consumer that calls find_ampl_col).  Without
  // user constraints, the map stays empty and add_ampl_variable is a
  // no-op — saving allocation/hashing overhead.  UserModel bundles
  // (piece 3) carry their own vars + constraints that reference / register
  // AMPL columns, so they enable the registry too.
  if (!system.user_constraint_array.empty() || !system.user_model_array.empty())
  {
    simulation.set_need_ampl_variables(/*v=*/true);
  }

  // Note: AMPL element-name and compound registries are populated by
  // SystemLP's constructor under std::call_once on
  // SimulationLP::ampl_registry_flag(), so the registry is filled
  // exactly once regardless of whether construction goes through
  // PlanningLP or directly via tests.

  auto&& scenes = simulation.scenes();
  auto&& phases = simulation.phases();

  const auto num_scenes = std::ssize(scenes);
  const auto num_phases = std::ssize(phases);

  // Pre-resolve the solver name on the main thread so worker threads
  // don't race through the plugin registry in parallel.  This triggers
  // the "Loaded solver plugin" log before the "Building LP" banner so
  // the output order reflects setup → pool → build.
  auto resolved_opts = flat_opts;
  // Thread the run-lifetime spill store down to each cell's
  // `create_linear_interface` (null disables spilling).
  resolved_opts.name_store = name_store;
  if (resolved_opts.solver_name.empty()) {
    resolved_opts.solver_name =
        std::string(SolverRegistry::instance().default_solver());
  }

  // Propagate low-memory hint from planning options into flat_opts so
  // SystemLP::create_lp can branch on it.  SDDP and cascade methods both
  // honor `sddp_options.low_memory_mode`; the monolithic method is
  // single-solve per (scene, phase) so the lazy-rebuild path adds cost
  // (extra backend reconstruct + decompress) without the iteration
  // amortisation that justifies it under SDDP.  Force `off` for
  // monolithic regardless of any user override, warning when the
  // override is dropped.
  {
    const auto method = options.method_type_enum();
    if (method == MethodType::monolithic) {
      if (resolved_opts.low_memory_mode != LowMemoryMode::off) {
        SPDLOG_WARN(
            "  low_memory={} requested but ignored for monolithic mode "
            "(single solve per cell, no amortisation) — forcing off",
            enum_name(resolved_opts.low_memory_mode));
        resolved_opts.low_memory_mode = LowMemoryMode::off;
      }
    } else if (resolved_opts.low_memory_mode == LowMemoryMode::off
               && (method == MethodType::sddp || method == MethodType::cascade))
    {
      resolved_opts.low_memory_mode = options.sddp_low_memory();
      resolved_opts.memory_codec = options.sddp_memory_codec();
    }
  }

  PlanningLP::scene_phase_systems_t all_systems(scenes.size());

  // `BuildMode` selects the granularity of LP assembly parallelism:
  //   - `serial`:         no work pool; build every (scene, phase) cell
  //                        in the calling thread.  Matches a genuine
  //                        pre-parallel baseline — no pool submit, no
  //                        build buffer, no move/merge overhead.
  //                        Ignores `--cpu-factor`.
  //   - `scene_parallel`: one pool task per scene; each task builds its
  //                        scene's phases sequentially into its own
  //                        `phase_systems_t` and runs
  //                        `tighten_scene_phase_links` before returning.
  //                        Coarser granularity, lower pool-submit and
  //                        malloc-arena contention — the pre-00c605d7
  //                        default and current default.
  //   - `full_parallel`:  one pool task per (scene × phase) cell,
  //                        materialised into a `build_buf` and moved
  //                        into `all_systems` during a sequential merge
  //                        pass.  Maximum concurrency, highest per-cell
  //                        overhead.  The custom SystemLP move-ctor
  //                        re-points the embedded SystemContext back-
  //                        reference and rebuilds its
  //                        `m_collection_ptrs_` interior-pointer table
  //                        to the new owner, so post-move SystemLPs are
  //                        valid even though the build buffer holds
  //                        them at a different address.
  //
  // Cross-phase state-variable links are queued (`PendingStateLink`)
  // inside each SystemLP and resolved by `tighten_scene_phase_links`.
  // That routine only touches state variables for a given scene, so it
  // is safe to run in-thread under `scene_parallel` and `serial`, and
  // in a sequential merge pass under `full_parallel`.
  //
  // Honors the CLI `--cpu-factor` flag (routed through
  // `sddp_options.pool_cpu_factor`) for both parallel modes.
  const auto build_mode = options.build_mode_enum();
  const auto build_cpu_factor = options.build_pool_cpu_factor();

  SPDLOG_INFO("  Building LP: {} scene(s) × {} phase(s) [mode={}]",
              num_scenes,
              num_phases,
              enum_name(build_mode));

  if (resolved_opts.low_memory_mode != LowMemoryMode::off) {
    const auto effective_codec = select_codec(resolved_opts.memory_codec);
    SPDLOG_INFO("  low_memory={} (memory codec: {})",
                enum_name(resolved_opts.low_memory_mode),
                codec_name(effective_codec));
  }

  const auto build_start = std::chrono::steady_clock::now();

  // Parallelism instrumentation: track peak concurrent cell-builders,
  // total per-cell CPU time, and the set of worker thread IDs that
  // actually ran cells.  After the join, we log
  //   parallelism = total_cell_cpu / wall_time
  // so the user can see at a glance whether the pool is truly running
  // tasks concurrently or whether some hidden serialization (global
  // mutex, plugin loader, etc.) is forcing cells through one at a time.
  // Under `serial` the peak stays at 1 and `worker_threads` reports the
  // single calling thread — which is the whole point of that mode.
  std::atomic<int> active_cells {0};
  std::atomic<int> peak_active {0};
  std::atomic<std::int64_t> total_cell_us {0};
  std::atomic<std::size_t> cells_done {0};
  std::mutex tid_mutex;
  std::unordered_set<std::size_t> worker_tids;

  // Shared per-cell timing helper used by all three build modes so the
  // summary log stays consistent regardless of granularity.  Callers
  // pass an `emplace_fn` that performs the actual SystemLP
  // construction into whatever storage slot this mode uses (build_buf
  // cell for `full_parallel`, `phase_systems_t::emplace_back` for the
  // other two).
  auto build_cell = [&]([[maybe_unused]] const auto& scene_ref,
                        [[maybe_unused]] const auto& phase_ref,
                        auto&& emplace_fn)
  {
    const auto cell_start = std::chrono::steady_clock::now();
    const auto cur = active_cells.fetch_add(1, std::memory_order_relaxed) + 1;
    // Monotonically raise peak_active to max(peak_active, cur).
    int prev_peak = peak_active.load(std::memory_order_relaxed);
    while (cur > prev_peak
           && !peak_active.compare_exchange_weak(
               prev_peak, cur, std::memory_order_relaxed))
    {}

    const auto tid = std::hash<std::thread::id> {}(std::this_thread::get_id());
    {
      const std::scoped_lock lock(tid_mutex);
      worker_tids.insert(tid);
    }

    SPDLOG_TRACE("    Building LP scene_uid={} phase_uid={} tid={}",
                 scene_ref.uid(),
                 phase_ref.uid(),
                 tid);
    emplace_fn();

    const auto cell_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - cell_start)
            .count();
    total_cell_us.fetch_add(cell_elapsed, std::memory_order_relaxed);
    active_cells.fetch_sub(1, std::memory_order_relaxed);
    const auto done = cells_done.fetch_add(1, std::memory_order_relaxed) + 1;
    // First-cell marker: proves at least one cell has been built — in
    // the parallel modes, that at least one pool task has started
    // executing before the join loop blocks; in serial mode, that the
    // in-thread loop has made forward progress.
    if (done == 1) {
      SPDLOG_INFO("    First LP cell built on worker tid={} ({:.3f}s)",
                  tid,
                  static_cast<double>(cell_elapsed) / 1e6);
    }
  };

  if (build_mode == BuildMode::serial) {
    // ── Serial path ────────────────────────────────────────────────
    // No pool, no build_buf, no futures.  Directly emplace every cell
    // into its final `phase_systems_t` in the calling thread.  This is
    // the honest serial baseline — `--cpu-factor` is ignored.
    for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
      auto& phase_systems = all_systems[scene_index];
      phase_systems.reserve(phases.size());
      for (const auto& phase : phases) {
        build_cell(scene,
                   phase,
                   [&]
                   {
                     phase_systems.emplace_back(
                         system, simulation, phase, scene, resolved_opts);
                   });
      }
      tighten_scene_phase_links(phase_systems, simulation);
    }
  } else if (build_mode == BuildMode::direct_parallel) {
    // ── Direct-parallel path ─────────────────────────────────────────
    // One jthread per hardware thread; cells are distributed via an
    // atomic counter.  No WorkPool, no scheduling mutex, no CPU/memory
    // monitoring — pure work-stealing with minimal synchronization.
    // Useful for benchmarking whether WorkPool overhead or malloc arena
    // contention is the dominant cost at high thread counts.
    using build_row_t = StrongIndexVector<PhaseIndex, std::optional<SystemLP>>;
    StrongIndexVector<SceneIndex, build_row_t> build_buf(scenes.size());
    for (auto& row : build_buf) {
      row.resize(phases.size());
    }

    const auto total_cells = scenes.size() * phases.size();
    std::atomic<std::size_t> next_cell {0};

    // LP assembly is pure CPU work — use physical cores as the base
    // (cpu_factor=1).  The build_cpu_factor can still scale it down
    // via --cpu-factor but is clamped to 1.0 max for raw threads.
    const auto effective_factor = std::min(build_cpu_factor, 1.0);
    const auto max_threads = static_cast<std::size_t>(
        std::max(1L, std::lround(effective_factor * physical_concurrency())));
    const auto n_threads = std::min(max_threads, total_cells);

    std::vector<std::jthread> workers;
    workers.reserve(n_threads);
    for (std::size_t t = 0; t < n_threads; ++t) {
      workers.emplace_back(
          [&]
          {
            while (true) {
              const auto cell_id =
                  next_cell.fetch_add(1, std::memory_order_relaxed);
              if (cell_id >= total_cells) {
                break;
              }
              const auto si = SceneIndex {
                  static_cast<int>(cell_id / phases.size()),
              };
              const auto pi = PhaseIndex {
                  static_cast<int>(cell_id % phases.size()),
              };
              const auto& scene = scenes[si];
              const auto& phase = phases[pi];
              build_cell(scene,
                         phase,
                         [&]
                         {
                           build_buf[si][pi].emplace(
                               system, simulation, phase, scene, resolved_opts);
                         });
            }
          });
    }
    // jthread destructor joins automatically.
    workers.clear();

    // Sequential merge + cross-phase link resolution.
    for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
      PlanningLP::phase_systems_t phase_systems;
      phase_systems.reserve(phases.size());
      for (auto& slot : build_buf[scene_index]) {
        // `build_buf` is guaranteed populated by the build pool before
        // we reach this merge step — every slot must hold a value.  A
        // missing slot indicates a silent pool-task failure and is a
        // crash-worthy programmer bug, so let `.value()`'s exception
        // propagate.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        phase_systems.emplace_back(std::move(slot).value());
      }
      tighten_scene_phase_links(phase_systems, simulation);
      all_systems[scene_index] = std::move(phase_systems);
    }
  } else {
    // Both WorkPool modes share the pool allocation.  The factory applies
    // the measured-baseline memory clamp + growth ceiling internally.
    auto pool = make_solver_work_pool(
        build_cpu_factor,
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/options.sddp_pool_memory_limit_mb(),
        /*pool_label=*/"PlanningLP initial build pool");

    if (build_mode == BuildMode::scene_parallel) {
      // ── Scene-parallel path (pre-00c605d7 behavior, current default) ──
      // One pool task per scene; each task builds its phases in order
      // into its own `phase_systems_t` and resolves cross-phase links
      // inside the worker — no shared build buffer, no post-merge
      // pass.  Writes go directly into `all_systems[scene_index]`,
      // which is safe because every task owns a distinct slot of the
      // pre-sized outer vector.
      std::vector<std::future<void>> futures;
      futures.reserve(scenes.size());
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        // `scene` rebinds on each iteration; capture by pointer so the
        // lambda sees its own stable reference.
        const auto* const scene_ptr = &scene;
        auto result = pool->submit(
            [&, scene_index, scene_ptr]
            {
              auto& phase_systems = all_systems[scene_index];
              phase_systems.reserve(phases.size());
              for (const auto& phase : phases) {
                build_cell(
                    *scene_ptr,
                    phase,
                    [&]
                    {
                      phase_systems.emplace_back(
                          system, simulation, phase, *scene_ptr, resolved_opts);
                    });
              }
              tighten_scene_phase_links(phase_systems, simulation);
            });
        if (!result.has_value()) {
          throw std::runtime_error(std::format(
              "Failed to submit scene {} to work pool", scene_index));
        }
        futures.push_back(std::move(*result));
      }
      SPDLOG_INFO(
          "  Submitted {} scene tasks to work pool, waiting for workers...",
          futures.size());
      for (auto& fut : futures) {
        fut.get();
      }
    } else {
      // ── Full-parallel path (post-00c605d7, opt-in) ────────────────
      // One pool task per (scene, phase) cell.  Each task builds into a
      // pre-allocated slot in `build_buf`; a sequential merge pass then
      // moves cells into their final `phase_systems_t` and runs
      // `tighten_scene_phase_links` per scene.
      using build_row_t =
          StrongIndexVector<PhaseIndex, std::optional<SystemLP>>;
      StrongIndexVector<SceneIndex, build_row_t> build_buf(scenes.size());
      for (auto& row : build_buf) {
        row.resize(phases.size());
      }

      std::vector<std::future<void>> futures;
      futures.reserve(scenes.size() * phases.size());
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        for (auto&& [phase_index, phase] : enumerate<PhaseIndex>(phases)) {
          const auto* const phase_ptr = &phase;
          const auto* const scene_ptr = &scene;
          auto result = pool->submit(
              [&, scene_index, phase_index, phase_ptr, scene_ptr]
              {
                build_cell(*scene_ptr,
                           *phase_ptr,
                           [&]
                           {
                             build_buf[scene_index][phase_index].emplace(
                                 system,
                                 simulation,
                                 *phase_ptr,
                                 *scene_ptr,
                                 resolved_opts);
                           });
              });
          if (!result.has_value()) {
            throw std::runtime_error(
                std::format("Failed to submit scene {} phase {} to work pool",
                            scene_index,
                            phase_index));
          }
          futures.push_back(std::move(*result));
        }
      }
      SPDLOG_INFO(
          "  Submitted {} LP cells to work pool, waiting for workers...",
          futures.size());
      for (auto& fut : futures) {
        fut.get();
      }

      // Sequentially merge cells into per-scene phase_systems_t and
      // resolve deferred cross-phase state-variable links.
      for (auto&& [scene_index, scene] : enumerate<SceneIndex>(scenes)) {
        PlanningLP::phase_systems_t phase_systems;
        phase_systems.reserve(phases.size());
        for (auto& slot : build_buf[scene_index]) {
          // See full-parallel merge above — same invariant, same reason
          // for accepting `.value()` to throw on a pool-task bug.
          // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
          phase_systems.emplace_back(std::move(slot).value());
        }
        tighten_scene_phase_links(phase_systems, simulation);
        all_systems[scene_index] = std::move(phase_systems);
      }
    }
  }

  // INVARIANT: every element's ``add_to_lp`` is responsible for emitting
  // a valid LP on its own — no ``update_lp`` should be required to make
  // the freshly-built LP solvable / physically meaningful.  ``update_lp``
  // is a pure refinement that re-evaluates state-dependent coefficients
  // (e.g. volume-dependent production factors, discharge limits, flow
  // rights) using a previous solve's solution.  We therefore skip the
  // historical "initial update_lp pass" here: invoking it before any
  // solve only re-evaluates the curves at ``eini`` (a stale fallback
  // that pessimistically pins coefficients to low-volume values),
  // overwriting the correct ``mean_production_factor`` rates that
  // ``TurbineLP::add_to_lp`` already wrote.  The first real
  // ``update_lp`` happens in the SDDP forward pass after the first
  // solve produces an optimal solution.

  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - build_start)
                             .count();

  // Parallelism summary.  `parallelism` ≈ 1.0 means cells ran
  // effectively serialized through the pool (contention, global lock,
  // or max_threads≈1).  A value >> 1 means wall time was compressed by
  // concurrent execution.  `peak_parallel` is the highest number of
  // cells observed simultaneously in-flight, and `worker_threads` is
  // the count of distinct OS threads that actually executed a cell.
  const auto total_cpu_s =
      static_cast<double>(total_cell_us.load(std::memory_order_relaxed)) / 1e6;
  const auto n_workers = [&]
  {
    const std::scoped_lock lock(tid_mutex);
    return worker_tids.size();
  }();
  const auto parallelism = elapsed > 0.0 ? (total_cpu_s / elapsed) : 0.0;
  SPDLOG_INFO(
      "  Building LP done in {:.3f}s "
      "(mode={}, cells={}, peak_parallel={}, worker_threads={}, "
      "total_cell_cpu={:.3f}s, parallelism={:.2f}x)",
      elapsed,
      enum_name(build_mode),
      cells_done.load(std::memory_order_relaxed),
      peak_active.load(std::memory_order_relaxed),
      n_workers,
      total_cpu_s,
      parallelism);

  return all_systems;
}

void PlanningLP::write_lp(const std::string& filename) const
{
  // Strip a trailing ".lp" so the filename we hand to the backend
  // (which always appends ".lp") never produces "name.lp_scene_0_phase_0.lp"
  // when callers — and the gtopt CLI doc — naturally write the extension.
  auto stem = filename;
  if (stem.ends_with(".lp")) {
    stem.erase(stem.size() - 3);
  }

  // Count cells: monolithic has exactly one (scene, phase), in which
  // case the `_scene_0_phase_0` decoration is noise.  SDDP needs it
  // to disambiguate the many cells it dumps under one stem.
  std::size_t n_cells = 0;
  for (auto&& phase_systems : m_systems_) {
    n_cells += phase_systems.size();
    if (n_cells > 1) {
      break;
    }
  }

  for (auto&& phase_systems : m_systems_) {
    for (auto&& system : phase_systems) {
      const auto result = n_cells == 1
          ? system.linear_interface().write_lp(stem)
          : system.write_lp(stem)
                .transform([](auto&&) {})
                .transform_error([](auto&& e) { return e; });
      if (!result) {
        spdlog::warn("{}", result.error().message);
        return;
      }
    }
  }
}

void PlanningLP::release_cells()
{
  // Drop every SystemLP (and its LinearInterface/solver backend).
  // The inner StrongIndexVectors are destroyed first, then the outer
  // vector itself is cleared and shrunk so the capacity is returned
  // to the allocator.
  //
  // Per-cell destruction is independent and — under `compress` — does
  // real work: freeing the multi-MB flat-LP snapshot, the accumulated
  // cut rows, the col/row metadata and the LP-solver backend for each
  // of the (scene × phase) cells.  For 900+ cells a serial loop costs
  // several seconds at every cascade level boundary (measured ~4.4s on
  // plp/2_years).  Fan the destruction out across the solver work pool,
  // one task per scene, mirroring the scene-parallel build and the
  // post-init "backend-release pool" (`SDDPMethod::initialize_solver`).
  // Each task owns a distinct scene vector, so there is no sharing; the
  // solver was already reset by the caller before this point, so no
  // other thread touches `m_systems_`.
  const auto num_scenes = m_systems_.size();
  if (num_scenes > 1) {
    auto pool = make_solver_work_pool(
        /*cpu_factor=*/1.0,
        /*cpu_threshold_override=*/0.0,
        /*scheduler_interval=*/std::chrono::milliseconds(50),
        /*memory_limit_mb=*/m_options_.sddp_pool_memory_limit_mb(),
        /*pool_label=*/"PlanningLP release-cells pool");
    std::vector<std::future<void>> futures;
    futures.reserve(num_scenes);
    for (auto& phase_systems : m_systems_) {
      auto* const ps_ptr = &phase_systems;
      auto result = pool->submit(
          [ps_ptr]
          {
            ps_ptr->clear();
            ps_ptr->shrink_to_fit();
          });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      } else {
        // Pool refused the task (shutdown / saturation) — fall back to
        // an in-thread release so no cell is leaked.
        ps_ptr->clear();
        ps_ptr->shrink_to_fit();
      }
    }
    for (auto& fut : futures) {
      fut.get();
    }
  } else {
    for (auto& phase_systems : m_systems_) {
      phase_systems.clear();
      phase_systems.shrink_to_fit();
    }
  }
  m_systems_.clear();
  m_systems_.shrink_to_fit();
}

void PlanningLP::drop_sim_snapshots() noexcept
{
  // Discard each cell's compressed flat-LP snapshot after SDDP
  // simulation Pass 1 converged.  Called by
  // `SDDPMethod::simulation_pass` under low_memory != off.
  //
  // The Phase-2a cache on each LinearInterface (col_sol / col_cost /
  // row_dual + scalars) carries everything `PlanningLP::write_out`
  // needs, and `rebuild_collections_if_needed()` re-flattens from the
  // live `System` element arrays — so the snapshot is no longer
  // required for any downstream step.
  for (auto& phase_systems : m_systems_) {
    for (auto& system : phase_systems) {
      system.linear_interface().clear_snapshot();
    }
  }
}

void PlanningLP::build_all_lps_eagerly()
{
  const auto num_scenes = m_systems_.size();
  if (num_scenes == 0) {
    return;
  }
  const auto num_phases = m_systems_.front().size();
  if (num_phases == 0) {
    return;
  }

  // Parallel dispatch: one task per (scene, phase) cell.  Each task
  // drives `ensure_lp_built()`, which is mode-agnostic — rebuild
  // invokes the callback (flatten + load_flat), compress reloads from
  // snapshot, off is a no-op.  Independent across cells (no shared
  // state) so we can fan out to the full thread budget.
  const auto build_start = std::chrono::steady_clock::now();
  SPDLOG_INFO(
      "  Eager LP build: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  auto pool = make_solver_work_pool(
      /*cpu_factor=*/1.0,
      /*cpu_threshold_override=*/0.0,
      /*scheduler_interval=*/std::chrono::milliseconds(50),
      /*memory_limit_mb=*/m_options_.sddp_pool_memory_limit_mb(),
      /*pool_label=*/"PlanningLP eager build pool");
  std::vector<std::future<void>> futures;
  futures.reserve(num_scenes * num_phases);
  for (auto& phase_systems : m_systems_) {
    for (auto& sys : phase_systems) {
      auto* const sys_ptr = &sys;
      auto result = pool->submit([sys_ptr] { sys_ptr->ensure_lp_built(); });
      if (result.has_value()) {
        futures.push_back(std::move(*result));
      }
    }
  }
  for (auto& fut : futures) {
    fut.get();
  }
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - build_start)
                             .count();
  SPDLOG_INFO("  Eager LP build done in {:.3f}s", elapsed);
  log_lp_memory_breakdown("post-eager-build");
}

PlanningLP::~PlanningLP() noexcept = default;

void PlanningLP::log_lp_memory_breakdown(std::string_view label) const
{
  // Opt-in only — keep normal runs silent and allocation-free.
  if (std::getenv("GTOPT_LOG_LP_MEMORY") == nullptr) {
    return;
  }
  std::size_t cells = 0;
  std::size_t cells_collections_resident = 0;
  std::size_t cells_flat_resident = 0;
  std::uint64_t compressed_bytes = 0;
  std::uint64_t cache_bytes = 0;
  std::uint64_t active_cuts = 0;
  for (const auto& phase_systems : m_systems_) {
    for (const auto& sys : phase_systems) {
      ++cells;
      if (sys.collections_resident()) {
        ++cells_collections_resident;
      }
      const auto d = sys.linear_interface().diag_resident_bytes();
      compressed_bytes += d.compressed_bytes;
      cache_bytes += d.cache_bytes;
      active_cuts += d.active_cuts;
      cells_flat_resident += d.flat_resident ? 1U : 0U;
    }
  }
  const double rss_mb =
      MemoryMonitor::get_system_memory_snapshot().process_rss_mb;
  constexpr double kMiB = 1024.0 * 1024.0;
  const double accounted_mb =
      (static_cast<double>(compressed_bytes) + static_cast<double>(cache_bytes))
      / kMiB;
  SPDLOG_INFO(
      "LP-MEM [{}]: cells={} (collections_resident={}, flat_resident={}); "
      "compressed_lp={:.0f} MB, solution_cache={:.0f} MB, active_cuts={}; "
      "accounted={:.0f} MB vs process RSS={:.0f} MB → unaccounted (XLP "
      "collections + heap/allocator)={:.0f} MB ({:.0f} MB/collections-cell)",
      label,
      cells,
      cells_collections_resident,
      cells_flat_resident,
      static_cast<double>(compressed_bytes) / kMiB,
      static_cast<double>(cache_bytes) / kMiB,
      active_cuts,
      accounted_mb,
      rss_mb,
      rss_mb - accounted_mb,
      cells_collections_resident > 0 ? (rss_mb - accounted_mb)
              / static_cast<double>(cells_collections_resident)
                                     : 0.0);
}

void PlanningLP::write_out_cell(SystemLP& system)
{
  // Fast path 0: the sim pass flagged this cell as belonging to a
  // scene it declared infeasible (no valid primal/dual).  Skip —
  // rehydrate + re-solve would just reproduce the same failure and
  // write garbage.
  if (system.output_skipped()) {
    return;
  }
  // Fast path A: sim-pass cells already wrote output — skip
  // ensure_lp_built entirely so we don't needlessly rehydrate the
  // backend just to find m_output_written_ == true.
  //
  // Drop the per-cell flat-LP snapshot here: the sim-pass write_out
  // path doesn't drop it, and PlanningLP::resolve() is the last
  // LP-touching step in the run, so no future caller will
  // reconstruct.  Idempotent (the snapshot was already dropped by
  // the sim-pass mid-run hook in
  // `sddp_iteration.cpp::run_scene_simulation` cleanup).
  if (system.output_written()) {
    system.linear_interface().clear_snapshot();
    // End-of-life drop: collections + replay journal + cached
    // primal/dual.  See `SystemLP::drop_for_write_out_done` doc.
    // Largest single saving on recovery hot-starts that loaded
    // tens of thousands of cuts into `m_replay_` — those records
    // never replay again past write_out.
    system.drop_for_write_out_done();
    return;
  }
  // Fast path B: under any non-`off` low_memory mode, Phase 2a
  // cached the solution vectors at release time and
  // `SystemLP::write_out` repopulates XLP col indices via a
  // throw-away flatten (`rebuild_collections_if_needed()`).  That
  // combo lets us emit without the expensive
  // `reconstruct_backend` → re-solve round-trip.  Both compress and
  // rebuild hit this path now — `rebuild_collections_if_needed`
  // handles both modes uniformly.
  const auto& li = system.linear_interface();
  if (system.low_memory_mode() != LowMemoryMode::off && li.is_backend_released()
      && li.is_optimal())
  {
    system.write_out();
    // Drop XLP collections rebuilt inside — the LinearInterface is
    // already released so this is a no-op on the backend side.
    system.release_backend();
    // Opportunity A — drop the compressed flat-LP snapshot now that
    // the cell's parquet output is written.  `PlanningLP::resolve()`
    // is the last LP-touching step in the run, so no future caller
    // will reconstruct this cell.  At ~3-5 MB compressed × 816 cells
    // on Juan-scale this frees ~3-4 GB of RSS just before the writer
    // thread exits.  Safe because the only remaining consumers
    // (status JSON, post-resolve aggregations) read cached scalars
    // from `m_cache_`, not the flat-LP snapshot.
    system.linear_interface().clear_snapshot();
    // End-of-life drop — see comment on the fast-path-A clone above.
    system.drop_for_write_out_done();
    return;
  }
  system.ensure_lp_built();
  system.write_out();
  system.release_backend();
  system.linear_interface().clear_snapshot();
  // End-of-life drop — see comment on the fast-path-A clone above.
  system.drop_for_write_out_done();
}

void PlanningLP::set_output_delegate(
    std::unique_ptr<PlanningLP> delegate) noexcept
{
  m_output_delegate_ = std::move(delegate);
}

// `write_out` calls its delegate's `write_out` (see below); clang-tidy
// flags that as a recursive call chain.  `set_output_delegate` is
// only called by cascade with an owned LP that itself has no delegate
// installed, so the recursion depth is at most 1 — but the invariant
// is not expressible in the type system.  Suppress at the function
// level since the warning is attached to the whole function, not the
// specific call.
void PlanningLP::write_out()
{
  // Cascade hand-off: when a non-level-0 cascade pass transferred
  // ownership of the final-level LP here (see
  // `CascadePlanningMethod::solve`), our own `m_systems_` is empty
  // (released at the level 0→1 cleanup) and the delegate holds the
  // populated grid.  Forward the call so `solution.csv` + element
  // parquets come from the level that actually solved.
  if (m_output_delegate_) {
    m_output_delegate_->write_out();
    return;
  }

  const auto num_scenes = std::ssize(m_systems_);
  const auto num_phases =
      num_scenes > 0 ? std::ssize(m_systems_.front()) : std::ptrdiff_t {0};
  SPDLOG_INFO(
      "  Writing output: {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // Parallelism strategy:
  //   – one task per scene (not per cell)
  //   – phases are processed sequentially within a scene task
  //   – each phase: ensure_lp_built → write_out → release_backend
  //
  // Memory rationale: a per-cell fan-out would hydrate every released
  // backend concurrently, costing tens of GB for 816 cells under
  // low-memory modes and blowing the process out of RAM.  Scene-parallel
  // matches the scene-parallel build-mode footprint that already fits.
  //
  // Solution invariance across low-memory modes: the SDDP simulation
  // forward pass calls `system.write_out()` itself, right before
  // `release_backend()`, so every cell it solves is emitted while the
  // backend still holds the real primal/dual vectors.  `SystemLP`
  // guards against double-emit via `m_output_written_`, so the per-cell
  // call below is a fast no-op for sim-pass cells.  The only cells it
  // actually processes are the ones not written by the simulation pass
  // — monolithic cells, cascade levels that skipped sim emission, or
  // SDDP cells whose simulation solve fell into the elastic branch.
  //
  // Parallelism strategy: one pool task per (scene, phase) CELL.  Parquet
  // writes dominate the wall time (hundreds of element shards per cell on
  // realistic grids), and each cell writes to its own scene=/phase=
  // partition under each element directory — no cross-cell contention on
  // disk.  Scene-level batching bounded the speedup to `phases` per
  // worker; cell-level lets the full thread pool grind through the
  // matrix.  Memory: each concurrent cell peaks at one flat-LP worth of
  // XLP wrappers under compress (fast-path does a single flatten) or
  // zero extra under off (backend was never released).
  //
  // cpu_factor=1.0 (NOT the SDDP pool's 4.0) is deliberate.  Arrow's
  // default memory pool and parquet encoders are not lock-free; more
  // threads in flight cause pathological contention (observed on
  // juan/iplp: 80 threads → oc.write 60 s per cell; 40 threads was
  // better, but 20 threads should reduce contention further while
  // still using every physical core).
  // Forward `--memory-limit` through the same path as every other
  // pool so the 60 GB RSS cap is honoured during write_out.
  //
  // History on cpu_factor (measured on 2-year case, Release build,
  // gtopt_28 vs gtopt_29):
  //   * 1.0 → per-cell wall 611 ms, write_out wall 27.5 s
  //   * 1.5 → per-cell wall 1319 ms, write_out wall 50.6 s (2.16×
  //     SLOWER per cell despite avg per-task CPU climbing from 55 %
  //     → 95 %).  The extra concurrency hits the Arrow memory-pool /
  //     parquet encoder contention noted in the 2026-05-12 comment
  //     above on juan/IPLP and dominates the marginal parallelism
  //     gain.  Confirmed that 1.0 is the sweet spot for this code
  //     path: the per-task CPU "headroom" at 1.0 is I/O wait that
  //     the parquet encoder uses internally — adding more pool
  //     threads on top just queues them on Arrow's locks.
  auto pool = make_solver_work_pool(
      /*cpu_factor=*/1.0,
      /*cpu_threshold_override=*/0.0,
      /*scheduler_interval=*/std::chrono::milliseconds(50),
      /*memory_limit_mb=*/m_options_.sddp_pool_memory_limit_mb(),
      /*pool_label=*/"PlanningLP write_out pool");

  std::vector<std::future<void>> futures;
  futures.reserve(static_cast<std::size_t>(num_scenes)
                  * static_cast<std::size_t>(num_phases));

  // Hybrid chunked cell-parallel write (mirrors `aperture_chunk_size`).
  //
  // One task per CELL would spread `num_cells` write_outs across the pool
  // — but the parallelism is then capped by whatever dispatches first, and
  // on low-scene cases the scene-parallel sim write left cores idle.  Here
  // we flatten ALL (scene, phase) cells and hand each pool task a
  // contiguous CHUNK to process serially (rebuild → write → drop per
  // cell).  Because each task drops every cell as soon as it is written,
  // at most ~one cell per concurrently-running task is resident, so the
  // memory ceiling is ~`num_running_tasks × per-cell` regardless of how
  // many scenes there are.
  //
  // Chunk size is a GLOBAL estimate: `num_cells / cores / 4`.  Dividing by
  // cores spreads the work evenly; the extra `/4` makes ~4× as many tasks
  // as cores so a slow cell can't leave a core idle while its neighbours
  // finish (load balancing) — and keeps each task's serial run short
  // enough that the resident set stays bounded.  Floor of 1 cell/chunk.
  std::vector<SystemLP*> cells;
  cells.reserve(static_cast<std::size_t>(num_scenes)
                * static_cast<std::size_t>(num_phases));
  for (auto& phase_systems : m_systems_) {
    for (auto& system : phase_systems) {
      cells.push_back(&system);
    }
  }
  const auto cores = std::max<std::size_t>(
      1, static_cast<std::size_t>(physical_concurrency()));
  const std::size_t total_cells = cells.size();
  const std::size_t write_chunk =
      std::max<std::size_t>(1, total_cells / cores / 4);
  SPDLOG_INFO("  write_out: {} cells in chunks of {} ({} task(s)), {} cores",
              total_cells,
              write_chunk,
              (total_cells + write_chunk - 1) / write_chunk,
              cores);

  for (std::size_t start = 0; start < total_cells; start += write_chunk) {
    const auto end = std::min(total_cells, start + write_chunk);
    auto result = pool->submit(
        [&cells, start, end]
        {
          for (std::size_t i = start; i < end; ++i) {
            PlanningLP::write_out_cell(*cells[i]);
          }
        },
        {
            .priority = TaskPriority::Low,
            .name = as_label("write_out_chunk", "start", start, "end", end),
        });
    if (result.has_value()) {
      futures.push_back(std::move(*result));
    } else {
      // Synchronous fallback preserves parity if the pool refuses the
      // task (e.g. memory-limit throttling raised mid-run).
      SPDLOG_WARN(
          "Failed to submit write_out chunk [{}, {}), running synchronously",
          start,
          end);
      for (std::size_t i = start; i < end; ++i) {
        PlanningLP::write_out_cell(*cells[i]);
      }
    }
  }

  for (auto& fut : futures) {
    fut.get();
  }

  // ── Write consolidated solution.csv ─────────────────────────────────────
  // All parallel write tasks have completed; now collect solution values
  // from every (scene, phase) system and write solution.csv in scene/phase
  // order.  This avoids any concurrent file access and guarantees a
  // deterministic, sorted output regardless of task completion order.

  struct SolutionRow
  {
    SceneUid scene_uid;
    PhaseUid phase_uid;
    int status;
    double obj_value;
    double kappa;
    double max_kappa;
    double gap;  ///< Final SDDP gap (0.0 for monolithic)
    double
        gap_change;  ///< Final SDDP stationary gap-change (1.0 for monolithic)
  };

  std::vector<SolutionRow> rows;
  rows.reserve(static_cast<std::size_t>(num_scenes)
               * static_cast<std::size_t>(num_phases));

  const auto& sddp = m_sddp_summary_;

  for (const auto& phase_systems : m_systems_) {
    for (const auto& system : phase_systems) {
      const auto& li = system.linear_interface();
      // Normalise status for unsolved cells: report -1 / "unknown"
      // regardless of low_memory_mode.  Otherwise `low_memory=off`
      // would leave a live CPLEX/HiGHS backend that returns status 3
      // ("iteration_limit" / not-yet-solved) while `compress` would
      // report -1 (cached !is_optimal after release_backend).  Both
      // mean the same thing semantically — the cell was never solved
      // — and differing values break solution.csv invariance across
      // the two modes.
      const auto status = li.is_optimal() ? li.get_status() : -1;
      rows.push_back({
          .scene_uid = system.scene().uid(),
          .phase_uid = system.phase().uid(),
          .status = status,
          .obj_value = li.get_obj_value(),
          // Backend returns std::nullopt when kappa is unavailable; we
          // store -1 in the CSV to mirror the "unset" convention used
          // elsewhere in the stats pipeline.  Downstream readers treat
          // negative kappa as "unknown" and must not fold it into max.
          .kappa = li.get_kappa().value_or(-1.0),
          .max_kappa = sddp.max_kappa,
          .gap = sddp.gap,
          .gap_change = sddp.gap_change,
      });
    }
  }

  // Sort rows by (scene_uid, phase_uid) for a deterministic output order.
  std::ranges::sort(rows,
                    [](const SolutionRow& a, const SolutionRow& b)
                    {
                      if (a.scene_uid != b.scene_uid) {
                        return a.scene_uid < b.scene_uid;
                      }
                      return a.phase_uid < b.phase_uid;
                    });

  const auto out_dir = std::filesystem::path(m_options_.output_directory());
  const auto sol_path = out_dir / "solution.csv";

  std::ofstream sol_file(sol_path.string(), std::ios::out);
  if (!sol_file) [[unlikely]] {
    SPDLOG_CRITICAL("Cannot open solution file '{}' for writing",
                    sol_path.string());
    return;
  }

  // Status names: CLP convention (0=optimal, 1=primal infeasible,
  // 2=dual infeasible/unbounded, 3=iteration limit, 4=error, 5=not solved)
  static constexpr auto status_name = [](int s) constexpr -> std::string_view
  {
    switch (s) {
      case 0:
        return "optimal";
      case 1:
        return "infeasible";
      case 2:
        return "unbounded";
      case 3:
        return "iteration_limit";
      case 4:
        return "error";
      default:
        return "unknown";
    }
  };

  sol_file << "scene,phase,status,status_name,obj_value,kappa,max_kappa,"
              "gap,gap_change\n";
  for (const auto& row : rows) {
    sol_file << std::format("{},{},{},{},{},{},{},{},{}\n",
                            row.scene_uid,
                            row.phase_uid,
                            row.status,
                            status_name(row.status),
                            row.obj_value,
                            row.kappa,
                            row.max_kappa,
                            row.gap,
                            row.gap_change);
    SPDLOG_DEBUG(
        "  solution.csv: scene={} phase={} status={} obj_value={} "
        "kappa={} max_kappa={} gap={} gap_change={}",
        row.scene_uid,
        row.phase_uid,
        row.status,
        row.obj_value,
        row.kappa,
        row.max_kappa,
        row.gap,
        row.gap_change);
  }
}

std::expected<void, Error> PlanningLP::resolve_scene_phases(
    SceneIndex scene_index,
    phase_systems_t& phase_systems,
    const SolverOptions& lp_opts)
{
  [[maybe_unused]] const auto num_phases = std::ssize(phase_systems);
  SPDLOG_DEBUG("  Solving scene {} ({} phase(s))", scene_index, num_phases);

  // Configure per-scene/phase solver log files when detailed mode is active
  const auto log_mode = lp_opts.log_mode.value_or(SolverLogMode::nolog);
  const auto log_dir = m_options_.log_directory();

  for (auto&& [phase_index, system_sp] : enumerate<PhaseIndex>(phase_systems)) {
    SPDLOG_TRACE("    Solving scene {} phase {} ({} cols, {} rows)",
                 scene_index,
                 phase_index,
                 system_sp.linear_interface().get_numcols(),
                 system_sp.linear_interface().get_numrows());

    if (log_mode == SolverLogMode::detailed && !log_dir.empty()) {
      std::filesystem::create_directories(log_dir);
      auto& li = system_sp.linear_interface();
      li.set_log_file(
          (std::filesystem::path(log_dir)
           / std::format(
               "{}_sc{}_ph{}", li.solver_name(), scene_index, phase_index))
              .string());
    }

    // Tag the LP with the Monolithic context so fallback warnings carry
    // the same (scene, phase) key as the surrounding info logs.
    system_sp.linear_interface().set_log_tag(
        std::format("Monolithic [s{} p{}]", scene_index, phase_index));

    if (auto result = system_sp.resolve(lp_opts); !result) {
      // Log the solver error with scene/phase context before writing the LP
      spdlog::warn("  Scene {} phase {}: {}",
                   scene_index,
                   phase_index,
                   result.error().message);

      // On error, write the problematic model to the log directory for
      // debugging
      const auto log_dir = m_options_.log_directory();
      std::filesystem::create_directories(log_dir);
      const auto filename = (std::filesystem::path(log_dir)
                             / as_label("error", scene_index, phase_index))
                                .string();
      if (auto lp_result = system_sp.write_lp(filename)) {
        spdlog::error("  Infeasible LP written to: {}", *lp_result);
      } else {
        spdlog::warn("{}", lp_result.error().message);
      }

      // LP diagnostic analysis is performed by run_gtopt after the solver
      // exits.  It scans the log directory for error*.lp files and runs
      // gtopt_check_lp on them with richer output (AI, IIS, parallel).

      auto error = std::move(result.error());
      error.message += std::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(std::move(error));
    }

    SPDLOG_DEBUG("    Scene {} phase {} solved ok", scene_index, phase_index);

    // Update state-variable dependents with the last solution.  Read
    // in physical space (`get_col_sol`) so `LinearInterface`'s optimal-
    // only bound-clamp scrubs solver-tolerance noise before the pin —
    // otherwise a `col_sol = uppb + eps` would propagate into a
    // target column's equality pin and make the dependent LP trivially
    // infeasible.  The physical value is descaled once inside each
    // target `set_col` call (target's col_scale), so no extra raw-
    // space arithmetic re-introduces noise.
    const auto solution_vector = system_sp.linear_interface().get_col_sol();

    for (auto&& state_var :
         simulation().state_variables(scene_index, phase_index)
             | std::views::values)
    {
      const double solution_value_phys = solution_vector[state_var.col()];

      for (auto&& dep_var : state_var.dependent_variables()) {
        auto& target_system =
            system(dep_var.scene_index(), dep_var.phase_index());
        target_system.linear_interface().set_col(dep_var.col(),
                                                 solution_value_phys);
      }
    }
  }

  return {};
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  const auto num_phases = simulation().phases().size();
  auto solver = make_planning_method(m_options_, num_phases);
  return solver->solve(*this, lp_opts);
}

}  // namespace gtopt
