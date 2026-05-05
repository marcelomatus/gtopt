/**
 * @file      validate_planning.cpp
 * @brief     Semantic validation of a parsed Planning object
 * @date      Wed Mar 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements referential integrity, range, and completeness checks
 * that run after JSON parsing but before LP construction.
 */

#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtopt/field_sched.hpp>
#include <gtopt/utils.hpp>
#include <gtopt/validate_planning.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Helper: format a SingleId for error messages.
[[nodiscard]] auto format_single_id(const SingleId& sid) -> std::string
{
  return std::visit(
      [](const auto& val) -> std::string
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return std::format("uid={}", val);
        } else {
          return std::format("name=\"{}\"", val);
        }
      },
      sid);
}

/// Check whether a SingleId matches any element in an array by uid or name.
template<typename Elem>
[[nodiscard]] auto single_id_exists(const SingleId& sid, const Array<Elem>& arr)
    -> bool
{
  return std::visit(
      [&arr](const auto& val) -> bool
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return std::ranges::any_of(
              arr, [&val](const auto& e) { return e.uid == val; });
        } else {
          return std::ranges::any_of(
              arr, [&val](const auto& e) { return e.name == val; });
        }
      },
      sid);
}

/// Validate that a SingleId field references a valid element.
template<typename Elem>
void check_ref(ValidationResult& result,
               const SingleId& sid,
               const Array<Elem>& arr,
               std::string_view owner_kind,
               std::string_view owner_name,
               std::string_view field_name,
               std::string_view target_kind)
{
  if (!single_id_exists(sid, arr)) {
    result.errors.push_back(
        std::format("{} '{}': {} references non-existent {} ({})",
                    owner_kind,
                    owner_name,
                    field_name,
                    target_kind,
                    format_single_id(sid)));
  }
}

/// Validate referential integrity of all components.
void check_referential_integrity(ValidationResult& result, const System& sys)
{
  // Generator.bus -> Bus
  for (const auto& gen : sys.generator_array) {
    check_ref(
        result, gen.bus, sys.bus_array, "Generator", gen.name, "bus", "Bus");
  }

  // Demand.bus -> Bus
  for (const auto& dem : sys.demand_array) {
    check_ref(result, dem.bus, sys.bus_array, "Demand", dem.name, "bus", "Bus");
  }

  // Line.bus_a, Line.bus_b -> Bus
  for (const auto& line : sys.line_array) {
    check_ref(
        result, line.bus_a, sys.bus_array, "Line", line.name, "bus_a", "Bus");
    check_ref(
        result, line.bus_b, sys.bus_array, "Line", line.name, "bus_b", "Bus");
  }

  // Turbine.waterway -> Waterway (optional), Turbine.generator -> Generator
  for (const auto& turb : sys.turbine_array) {
    if (turb.waterway.has_value()) {
      check_ref(result,
                turb.waterway.value(),
                sys.waterway_array,
                "Turbine",
                turb.name,
                "waterway",
                "Waterway");
    }
    check_ref(result,
              turb.generator,
              sys.generator_array,
              "Turbine",
              turb.name,
              "generator",
              "Generator");
  }

  // Pump.waterway -> Waterway, Pump.demand -> Demand
  for (const auto& pump : sys.pump_array) {
    check_ref(result,
              pump.waterway,
              sys.waterway_array,
              "Pump",
              pump.name,
              "waterway",
              "Waterway");
    check_ref(result,
              pump.demand,
              sys.demand_array,
              "Pump",
              pump.name,
              "demand",
              "Demand");
  }

  // Flow.junction -> Junction (optional in flow-turbine mode)
  for (const auto& flow : sys.flow_array) {
    if (flow.junction.has_value()) {
      check_ref(result,
                flow.junction.value(),
                sys.junction_array,
                "Flow",
                flow.name,
                "junction",
                "Junction");
    }
  }

  // Waterway.junction_a, junction_b -> Junction
  for (const auto& ww : sys.waterway_array) {
    check_ref(result,
              ww.junction_a,
              sys.junction_array,
              "Waterway",
              ww.name,
              "junction_a",
              "Junction");
    check_ref(result,
              ww.junction_b,
              sys.junction_array,
              "Waterway",
              ww.name,
              "junction_b",
              "Junction");
  }

  // Converter.battery -> Battery, generator -> Generator, demand -> Demand
  for (const auto& conv : sys.converter_array) {
    check_ref(result,
              conv.battery,
              sys.battery_array,
              "Converter",
              conv.name,
              "battery",
              "Battery");
    check_ref(result,
              conv.generator,
              sys.generator_array,
              "Converter",
              conv.name,
              "generator",
              "Generator");
    check_ref(result,
              conv.demand,
              sys.demand_array,
              "Converter",
              conv.name,
              "demand",
              "Demand");
  }

  // Reservoir.junction -> Junction
  for (const auto& res : sys.reservoir_array) {
    check_ref(result,
              res.junction,
              sys.junction_array,
              "Reservoir",
              res.name,
              "junction",
              "Junction");
  }
}

// ── Positivity helpers ────────────────────────────────────────────────
//
// Water-right, line, and waterway magnitudes must be physically
// positive (zero allowed when explicitly called out; negative is
// always a schema bug).  These helpers walk the FieldSched variant
// (scalar / vector / file) and report any negative scalar value
// found.  FileSched entries are skipped — they resolve at
// load-time and the file loader is responsible for its own checks.

enum class Positivity : std::uint8_t
{
  strict,  ///< value must be > 0
  non_negative,  ///< value must be >= 0
};

[[nodiscard]] constexpr bool violates(double v, Positivity p) noexcept
{
  return p == Positivity::strict ? !(v > 0.0) : !(v >= 0.0);
}

[[nodiscard]] constexpr std::string_view positivity_name(Positivity p) noexcept
{
  return p == Positivity::strict ? "> 0" : ">= 0";
}

template<typename Vec>
void check_vector_positive(ValidationResult& result,
                           const Vec& values,
                           Positivity p,
                           std::string_view owner_kind,
                           std::string_view owner_name,
                           std::string_view field_name)
{
  if constexpr (std::is_arithmetic_v<
                    std::remove_cvref_t<decltype(values.front())>>)
  {
    for (const auto& [i, v_raw] : enumerate(values)) {
      const auto v = static_cast<double>(v_raw);
      if (violates(v, p)) {
        result.errors.push_back(std::format("{} '{}': {}[{}] = {} must be {}",
                                            owner_kind,
                                            owner_name,
                                            field_name,
                                            i,
                                            v,
                                            positivity_name(p)));
        return;  // one error per field is enough
      }
    }
  } else {
    // Nested vector (TBReal / STBReal).  Recurse one level.
    for (const auto& sub : values) {
      check_vector_positive(result, sub, p, owner_kind, owner_name, field_name);
    }
  }
}

/// Check a FieldSched<Real, ...> variant for positivity.
/// FileSched (string) entries are skipped — those resolve at load-time.
template<typename FieldSched>
void check_field_positive(ValidationResult& result,
                          const FieldSched& fs,
                          Positivity p,
                          std::string_view owner_kind,
                          std::string_view owner_name,
                          std::string_view field_name)
{
  std::visit(
      [&](const auto& val)
      {
        using T = std::remove_cvref_t<decltype(val)>;
        if constexpr (std::is_same_v<T, Real>) {
          if (violates(static_cast<double>(val), p)) {
            result.errors.push_back(std::format("{} '{}': {} = {} must be {}",
                                                owner_kind,
                                                owner_name,
                                                field_name,
                                                val,
                                                positivity_name(p)));
          }
        } else if constexpr (std::is_same_v<T, FileSched>) {
          // Filename reference — defer validation to load-time.
        } else {
          check_vector_positive(
              result, val, p, owner_kind, owner_name, field_name);
        }
      },
      fs);
}

/// Check an optional FieldSched field.  Skips when unset.
template<typename OptFieldSched>
void check_opt_field_positive(ValidationResult& result,
                              const OptFieldSched& opt,
                              Positivity p,
                              std::string_view owner_kind,
                              std::string_view owner_name,
                              std::string_view field_name)
{
  if (!opt.has_value()) {
    return;
  }
  check_field_positive(result, *opt, p, owner_kind, owner_name, field_name);
}

/// Check positivity of flow-right / volume-right / line / waterway fields.
void check_positivity(ValidationResult& result, const System& sys)
{
  // FlowRight: discharge and fmax must be >= 0 (consumptive flow).
  for (const auto& fr : sys.flow_right_array) {
    check_field_positive(result,
                         fr.discharge,
                         Positivity::non_negative,
                         "FlowRight",
                         fr.name,
                         "discharge");
    check_opt_field_positive(result,
                             fr.fmax,
                             Positivity::non_negative,
                             "FlowRight",
                             fr.name,
                             "fmax");
  }

  // VolumeRight: storage and demand magnitudes must be >= 0.
  for (const auto& vr : sys.volume_right_array) {
    check_opt_field_positive(result,
                             vr.emin,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emin");
    check_opt_field_positive(result,
                             vr.emax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emax");
    check_opt_field_positive(result,
                             vr.demand,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "demand");
    check_opt_field_positive(result,
                             vr.fmax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "fmax");
    if (vr.eini.has_value() && *vr.eini < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': eini = {} must be >= 0", vr.name, *vr.eini));
    }
    if (vr.efin.has_value() && *vr.efin < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': efin = {} must be >= 0", vr.name, *vr.efin));
    }
  }

  // Line: transfer capacities must be >= 0 (direction is encoded
  // separately in bus_a/bus_b).
  for (const auto& line : sys.line_array) {
    check_opt_field_positive(result,
                             line.tmax_ab,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ab");
    check_opt_field_positive(result,
                             line.tmax_ba,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ba");
  }

  // Waterway (hydro flow): fmin and fmax must be >= 0.  Negative flow
  // would invert the direction, which should be expressed by swapping
  // junction_a/junction_b instead.  fmax == 0 is legal: it means the
  // flow is pinned at 0 (matches PLP's VertMax=0 → qv_k ∈ [0,0] on
  // the vertimiento waterway — see plpcnfce.dat + leecnfce.f:342-343).
  for (const auto& ww : sys.waterway_array) {
    check_opt_field_positive(
        result, ww.fmin, Positivity::non_negative, "Waterway", ww.name, "fmin");
    check_opt_field_positive(
        result, ww.fmax, Positivity::non_negative, "Waterway", ww.name, "fmax");
  }
}

// ── Piecewise-linear feasibility (Reservoir seepage / discharge_limit) ──
//
// Both `ReservoirSeepage` and `ReservoirDischargeLimit` produce a
// stage-level LP constraint of the form
//
//     row(efin) − slope · efin  ≤  intercept
//
// where `(slope, intercept)` is selected by piecewise-linear segment
// against the start-of-stage volume.  At the LOWEST volume boundary
// (`efin = emin`) the constraint reduces to
//
//     row(efin)  ≤  intercept + slope · emin
//
// For `row(efin) ≥ 0` to be a feasible LP, the right-hand side must
// be non-negative — which means the FIRST segment (lowest volume
// breakpoint) must satisfy  `intercept + slope · emin ≥ 0`.  When
// the input data violates this invariant the LP solves cleanly only
// while the reservoir stays above the first segment's lower bound;
// the moment a backward cut pulls efin to emin (which happens
// regularly under SDDP iter-1+), the resolve goes primal-infeasible
// and the trajectory diverges between off (which retains stale
// state on the live backend) and compress (which reverts on
// reconstruct).  Catching the invariant at validation time produces
// a clear "fix the input data" message instead of opaque solver
// fallbacks downstream.
//
// Implementation: walks `system.reservoir_seepage_array` and
// `system.reservoir_discharge_limit_array`, looks up the owning
// reservoir's `emin` (when scalar — vector/file schedules are
// resolved at load time and skipped here), then evaluates the
// first segment's `intercept + slope · emin` and emits a warning
// when the result is negative.

namespace
{
// Try to extract a scalar value from an `OptTRealFieldSched` /
// `OptTBRealFieldSched`-shaped field.  Returns nullopt for
// vector / file schedules — those are validated per-stage at load
// time and can't be sanity-checked here.
template<typename OptField>
[[nodiscard]] std::optional<Real> try_scalar_value(const OptField& field)
{
  if (!field.has_value()) {
    return std::nullopt;
  }
  return std::visit(
      [](const auto& v) -> std::optional<Real>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return v;
        } else {
          return std::nullopt;
        }
      },
      *field);
}

// Look up a reservoir struct by SingleId (Uid or Name).
[[nodiscard]] const Reservoir* find_reservoir(
    const std::vector<Reservoir>& reservoirs, const SingleId& sid)
{
  return std::visit(
      [&](const auto& v) -> const Reservoir*
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Uid>) {
          for (const auto& r : reservoirs) {
            if (r.uid == static_cast<int>(v)) {
              return &r;
            }
          }
        } else if constexpr (std::is_same_v<T, Name>) {
          for (const auto& r : reservoirs) {
            if (r.name == v) {
              return &r;
            }
          }
        }
        return nullptr;
      },
      sid);
}

// Look up a waterway struct by SingleId (Uid or Name).
[[nodiscard]] const Waterway* find_waterway(
    const std::vector<Waterway>& waterways, const SingleId& sid)
{
  return std::visit(
      [&](const auto& v) -> const Waterway*
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Uid>) {
          for (const auto& w : waterways) {
            if (w.uid == static_cast<int>(v)) {
              return &w;
            }
          }
        } else if constexpr (std::is_same_v<T, Name>) {
          for (const auto& w : waterways) {
            if (w.name == v) {
              return &w;
            }
          }
        }
        return nullptr;
      },
      sid);
}

// Compute the active volume range [V_low, V_high] for segment k of a
// segments vector sorted ascending by `.volume`, clipped to the
// reservoir's [emin, emax] envelope.  N is the segment count.
struct SegmentRange
{
  Real v_low {0.0};
  Real v_high {0.0};
};

template<typename SegmentT>
[[nodiscard]] SegmentRange segment_range(const std::vector<SegmentT>& segments,
                                         std::size_t k,
                                         Real emin,
                                         Real emax)
{
  const auto seg_lo = static_cast<Real>(segments[k].volume);
  const auto v_low = std::max(seg_lo, emin);
  const auto v_high = (k + 1 < segments.size())
      ? static_cast<Real>(segments[k + 1].volume)
      : emax;
  return {.v_low = v_low, .v_high = v_high};
}

// Linear function value at endpoints (no interior extrema for linear).
struct LinearRange
{
  Real f_low {0.0};
  Real f_high {0.0};
  [[nodiscard]] Real fmin() const { return std::min(f_low, f_high); }
  [[nodiscard]] Real fmax() const { return std::max(f_low, f_high); }
};

[[nodiscard]] LinearRange evaluate_linear(Real slope,
                                          Real constant,
                                          const SegmentRange& r)
{
  return {
      .f_low = constant + (slope * r.v_low),
      .f_high = constant + (slope * r.v_high),
  };
}

/// Extract one Real per stage from a 1D `OptTRealFieldSched`-shaped
/// field.  Scalar broadcasts to every stage; vector indexes by stage
/// (truncated/padded to `num_stages` with `default_value`); file-
/// schedule returns `nullopt` (truly deferred to load-time).
template<typename OptField>
[[nodiscard]] std::optional<std::vector<Real>> per_stage_values(
    const OptField& field, std::size_t num_stages, Real default_value)
{
  if (!field.has_value()) {
    return std::vector<Real>(num_stages, default_value);
  }
  return std::visit(
      [&](const auto& v) -> std::optional<std::vector<Real>>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return std::vector<Real>(num_stages, v);
        } else if constexpr (std::is_same_v<T, std::vector<Real>>) {
          std::vector<Real> out(num_stages, default_value);
          for (std::size_t s = 0; s < num_stages && s < v.size(); ++s) {
            out[s] = v[s];
          }
          return out;
        } else {
          // FileSched (string) — truly deferred.
          return std::nullopt;
        }
      },
      *field);
}

/// 2D variant for `OptTBRealFieldSched` (per-stage, per-block).
/// Reduces each stage's block vector to a single scalar via
/// `reducer` (use `std::ranges::min` for upper-bound fields where the
/// strictest stage-level value is the smallest, `std::ranges::max`
/// for lower-bound fields where the strictest is the largest).
template<typename OptField, typename Reducer>
[[nodiscard]] std::optional<std::vector<Real>> per_stage_values_2d(
    const OptField& field,
    std::size_t num_stages,
    Real default_value,
    Reducer reducer)
{
  if (!field.has_value()) {
    return std::vector<Real>(num_stages, default_value);
  }
  return std::visit(
      [&](const auto& v) -> std::optional<std::vector<Real>>
      {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Real>) {
          return std::vector<Real>(num_stages, v);
        } else if constexpr (std::is_same_v<T,
                                            std::vector<std::vector<Real>>>) {
          std::vector<Real> out(num_stages, default_value);
          for (std::size_t s = 0; s < num_stages && s < v.size(); ++s) {
            if (!v[s].empty()) {
              out[s] = reducer(v[s]);
            }
          }
          return out;
        } else {
          return std::nullopt;
        }
      },
      *field);
}

/// Per-stage scan summary for one piecewise segment in one direction
/// (below the lower bound or above the upper bound).  Aggregated
/// across all stages so the validator emits exactly ONE warning per
/// (element, segment, direction) instead of one per stage.
struct StageViolation
{
  std::size_t count {0};  ///< Number of stages where the bound is violated.
  Real worst_diff {0.0};  ///< Largest |bound − f| across the failing stages.
  std::size_t worst_stage {0};  ///< Stage index achieving `worst_diff`.
  LinearRange worst_fr {};  ///< Linear range at the worst stage.
  SegmentRange worst_range {};  ///< Active [V_low, V_high] at the worst stage.
  Real worst_bound {0.0};  ///< Bound value at the worst stage.
};

/// Walk every stage and accumulate the worst lower-bound violation
/// (`fr.fmin() < lower_bounds[s]`) and worst upper-bound violation
/// (`fr.fmax() > upper_bounds[s]`) for the given segment.  Returns a
/// pair `{below, above}`; either may have `count == 0` for "no
/// violation in that direction".
template<typename Segment>
[[nodiscard]] std::pair<StageViolation, StageViolation> scan_segment_per_stage(
    const std::vector<Segment>& segments,
    std::size_t k,
    Real slope,
    Real constant,
    std::span<const Real> emins,
    std::span<const Real> emaxs,
    std::span<const Real> lower_bounds,
    std::span<const Real> upper_bounds)
{
  StageViolation below;
  StageViolation above;
  const std::size_t num_stages = emins.size();
  for (std::size_t s = 0; s < num_stages; ++s) {
    const auto range = segment_range(segments, k, emins[s], emaxs[s]);
    if (range.v_high < range.v_low) {
      continue;  // segment outside this stage's [emin, emax] envelope
    }
    const auto fr = evaluate_linear(slope, constant, range);

    if (fr.fmin() < lower_bounds[s]) {
      ++below.count;
      const Real diff = lower_bounds[s] - fr.fmin();
      if (diff > below.worst_diff) {
        below.worst_diff = diff;
        below.worst_stage = s;
        below.worst_fr = fr;
        below.worst_range = range;
        below.worst_bound = lower_bounds[s];
      }
    }
    if (fr.fmax() > upper_bounds[s]) {
      ++above.count;
      const Real diff = fr.fmax() - upper_bounds[s];
      if (diff > above.worst_diff) {
        above.worst_diff = diff;
        above.worst_stage = s;
        above.worst_fr = fr;
        above.worst_range = range;
        above.worst_bound = upper_bounds[s];
      }
    }
  }
  return {below, above};
}

/// Per-stage seepage envelopes.  Returns `nullopt` for any field that
/// is file-schedule (truly deferred to load-time validation).
struct SeepageEnvelopes
{
  std::vector<Real> emins;  ///< Reservoir per-stage emin.
  std::vector<Real> emaxs;  ///< Reservoir per-stage emax.
  std::vector<Real>
      fmins;  ///< Strictest per-stage waterway floor (max-reduced).
  std::vector<Real>
      fmaxs;  ///< Strictest per-stage waterway ceiling (min-reduced).
};

[[nodiscard]] std::optional<SeepageEnvelopes> resolve_seepage_envelopes(
    const Reservoir& res, const Waterway& ww, std::size_t num_stages)
{
  using std::ranges::max;
  using std::ranges::min;

  auto emins = per_stage_values(res.emin, num_stages, 0.0);
  auto emaxs = per_stage_values(
      res.emax, num_stages, std::numeric_limits<Real>::infinity());
  // Reduce per-stage block vectors to the strictest stage-level
  // value: max(fmin) (highest required floor), min(fmax) (lowest
  // allowed ceiling).
  auto fmins =
      per_stage_values_2d(ww.fmin,
                          num_stages,
                          0.0,
                          [](const std::vector<Real>& v) { return max(v); });
  auto fmaxs =
      per_stage_values_2d(ww.fmax,
                          num_stages,
                          std::numeric_limits<Real>::infinity(),
                          [](const std::vector<Real>& v) { return min(v); });
  if (!emins.has_value() || !emaxs.has_value() || !fmins.has_value()
      || !fmaxs.has_value())
  {
    return std::nullopt;
  }
  return SeepageEnvelopes {
      .emins = std::move(*emins),
      .emaxs = std::move(*emaxs),
      .fmins = std::move(*fmins),
      .fmaxs = std::move(*fmaxs),
  };
}

/// Per-stage discharge-limit envelopes (only the reservoir
/// [emin, emax] matter; the LP constrains the discharge col bound to
/// `intercept + slope·V ≥ 0`).
struct DischargeLimitEnvelopes
{
  std::vector<Real> emins;
  std::vector<Real> emaxs;
};

[[nodiscard]] std::optional<DischargeLimitEnvelopes>
resolve_discharge_limit_envelopes(const Reservoir& res, std::size_t num_stages)
{
  auto emins = per_stage_values(res.emin, num_stages, 0.0);
  auto emaxs = per_stage_values(
      res.emax, num_stages, std::numeric_limits<Real>::infinity());
  if (!emins.has_value() || !emaxs.has_value()) {
    return std::nullopt;
  }
  return DischargeLimitEnvelopes {
      .emins = std::move(*emins),
      .emaxs = std::move(*emaxs),
  };
}

/// Format the standard "below fmin" warning for a seepage segment.
[[nodiscard]] std::string format_seepage_below_warning(
    const ReservoirSeepage& seep,
    const Reservoir& res,
    const Waterway& ww,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirSeepage '{}' (reservoir '{}', waterway '{}'): "
      "segment {} produces qfilt below waterway fmin in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → qfilt "
      "range [{:.6g}, {:.6g}] vs fmin={:.6g}, slope={:.6g}, "
      "constant={:.6g}); the LP will go primal-infeasible whenever "
      "efin lands in the segment's lower portion.  Adjust the "
      "segment data so that `constant + slope * V ≥ fmin` for all "
      "V in [V_low, V_high].",
      seep.name,
      res.name,
      ww.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      v.worst_bound,
      seep.segments[k].slope,
      seep.segments[k].constant);
}

/// Format the standard "above fmax" warning for a seepage segment.
[[nodiscard]] std::string format_seepage_above_warning(
    const ReservoirSeepage& seep,
    const Reservoir& res,
    const Waterway& ww,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirSeepage '{}' (reservoir '{}', waterway '{}'): "
      "segment {} produces qfilt above waterway fmax in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → qfilt "
      "range [{:.6g}, {:.6g}] vs fmax={:.6g}, slope={:.6g}, "
      "constant={:.6g}); the LP will go primal-infeasible whenever "
      "efin lands in the segment's upper portion.",
      seep.name,
      res.name,
      ww.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      v.worst_bound,
      seep.segments[k].slope,
      seep.segments[k].constant);
}

/// Format the standard "negative discharge bound" warning for a
/// discharge-limit segment.
[[nodiscard]] std::string format_discharge_limit_warning(
    const ReservoirDischargeLimit& ddl,
    const Reservoir& res,
    std::size_t k,
    std::size_t num_stages,
    const StageViolation& v)
{
  return std::format(
      "ReservoirDischargeLimit '{}' (reservoir '{}'): segment {} "
      "produces a negative discharge upper bound in {} of {} "
      "stages (worst at stage {}: {:.3g} ≤ efin ≤ {:.3g} → bound "
      "range [{:.6g}, {:.6g}], slope={:.6g}, intercept={:.6g}); "
      "the LP will go primal-infeasible whenever efin lands in "
      "the segment's lower portion.  Adjust the segment so that "
      "`intercept + slope * V >= 0` for all V in [V_low, V_high].",
      ddl.name,
      res.name,
      k,
      v.count,
      num_stages,
      v.worst_stage,
      v.worst_range.v_low,
      v.worst_range.v_high,
      v.worst_fr.fmin(),
      v.worst_fr.fmax(),
      ddl.segments[k].slope,
      ddl.segments[k].intercept);
}

}  // namespace

void check_piecewise_feasibility(ValidationResult& result,
                                 const Planning& planning)
{
  const auto& sys = planning.system;
  const auto num_stages = planning.simulation.stage_array.size();
  // **ReservoirSeepage** — per-segment range feasibility.
  //
  // The LP constraint per (block, stage) is the equality
  //   `qfilt − slope_k · efin = constant_k`
  // → `qfilt = constant_k + slope_k · efin`.
  // The flow `qfilt` lives on the seepage's waterway and is bounded
  // `qfilt ∈ [fmin, fmax]`.  For each segment k, while it's the
  // active piecewise piece (i.e. `efin ∈ [V_low_k, V_high_k]`, where
  // V_low_0 is clamped to the reservoir's emin and V_high_{N-1} is
  // clamped to emax), we need:
  //
  //   fmin  ≤  constant_k + slope_k · efin  ≤  fmax,  ∀ efin ∈ [V_low_k,
  //   V_high_k]
  //
  // For schedule-form `emin/emax/fmin/fmax` (vector-per-stage), the
  // check fires per-stage and is summarised: ONE warning per
  // (element, segment, direction) listing how many stages fail and
  // pointing at the worst offender.  File-schedules (string paths)
  // are still deferred — we don't load them here.
  for (const auto& seep : sys.reservoir_seepage_array) {
    if (seep.segments.empty()) {
      continue;
    }
    const auto* res = find_reservoir(sys.reservoir_array, seep.reservoir);
    if (res == nullptr) {
      continue;  // referential integrity check elsewhere catches missing refs
    }
    const auto* ww = find_waterway(sys.waterway_array, seep.waterway);
    if (ww == nullptr) {
      continue;
    }
    const auto envs = resolve_seepage_envelopes(*res, *ww, num_stages);
    if (!envs.has_value()) {
      continue;  // some field is file-schedule — truly deferred
    }

    for (std::size_t k = 0; k < seep.segments.size(); ++k) {
      const auto& seg = seep.segments[k];
      const auto [below, above] = scan_segment_per_stage(seep.segments,
                                                         k,
                                                         seg.slope,
                                                         seg.constant,
                                                         envs->emins,
                                                         envs->emaxs,
                                                         envs->fmins,
                                                         envs->fmaxs);
      if (below.count > 0) {
        result.warnings.push_back(format_seepage_below_warning(
            seep, *res, *ww, k, num_stages, below));
      }
      if (above.count > 0) {
        result.warnings.push_back(format_seepage_above_warning(
            seep, *res, *ww, k, num_stages, above));
      }
    }
  }

  // **ReservoirDischargeLimit** — per-segment range feasibility.
  //
  // The LP row is `qeh − slope_k · efin ≤ intercept_k`, where qeh has
  // lower bound 0 (default for a free non-negative col).  For each
  // segment k active at efin ∈ [V_low_k, V_high_k] at stage s:
  //
  //   0 ≤ intercept_k + slope_k · efin   for all efin in the range.
  //
  // Per-stage scan with the same dedup semantics as the seepage
  // check above: ONE warning per (DDL, segment) summarising the
  // count of failing stages plus the worst case.  Only the lower
  // bound (≥ 0) matters; upper-bound is +∞ so the "above" branch of
  // `scan_segment_per_stage` never fires.
  const std::vector<Real> ddl_lower(num_stages, 0.0);
  const std::vector<Real> ddl_upper(num_stages,
                                    std::numeric_limits<Real>::infinity());
  for (const auto& ddl : sys.reservoir_discharge_limit_array) {
    if (ddl.segments.empty()) {
      continue;
    }
    const auto* res = find_reservoir(sys.reservoir_array, ddl.reservoir);
    if (res == nullptr) {
      continue;
    }
    const auto envs = resolve_discharge_limit_envelopes(*res, num_stages);
    if (!envs.has_value()) {
      continue;
    }

    for (std::size_t k = 0; k < ddl.segments.size(); ++k) {
      const auto& seg = ddl.segments[k];
      const auto [below, _above] = scan_segment_per_stage(ddl.segments,
                                                          k,
                                                          seg.slope,
                                                          seg.intercept,
                                                          envs->emins,
                                                          envs->emaxs,
                                                          ddl_lower,
                                                          ddl_upper);
      if (below.count > 0) {
        result.warnings.push_back(
            format_discharge_limit_warning(ddl, *res, k, num_stages, below));
      }
    }
  }
}

/// Validate range constraints on simulation parameters.
void check_ranges(ValidationResult& result, const Planning& planning)
{
  const auto& sim = planning.simulation;

  // Block duration > 0
  for (const auto& blk : sim.block_array) {
    if (blk.duration <= 0.0) {
      result.errors.push_back(std::format(
          "Block uid={}: duration ({}) must be > 0", blk.uid, blk.duration));
    }
  }

  // Stage count_block > 0
  for (const auto& stg : sim.stage_array) {
    if (stg.count_block == 0) {
      result.errors.push_back(
          std::format("Stage uid={}: count_block must be > 0", stg.uid));
    }
  }

  // Generator capacity non-negative (warning only)
  for (const auto& gen : planning.system.generator_array) {
    if (gen.capacity.has_value()) {
      const auto& cap = gen.capacity.value();
      if (std::holds_alternative<Real>(cap) && std::get<Real>(cap) < 0.0) {
        result.warnings.push_back(
            std::format("Generator '{}': capacity ({}) is negative",
                        gen.name,
                        std::get<Real>(cap)));
      }
    }
  }
}

/// Validate that every UID listed in `phase.apertures` resolves to an
/// entry in `simulation.aperture_array`.
///
/// Pre-fix history: silent fallback at runtime — phases with
/// dangling aperture UIDs would emit
/// `SDDP Aperture [...]: source_scenario X not found and no aperture
/// cache, skipping` for each broken reference, then proceed with the
/// remaining apertures.  Easy to misread as expected behaviour
/// (see the original juan/gtopt_iplp configuration where 2 of 16
/// per-phase aperture UIDs referenced uids 1 and 2 but the global
/// `aperture_array` defined uids 51..66 only).  Surfacing this as a
/// validation error catches the input-data bug at parse time, before
/// the SDDP run silently quietly drops cuts.
void check_aperture_references(ValidationResult& result,
                               const Planning& planning)
{
  const auto& sim = planning.simulation;

  // Build a set of aperture UIDs declared in the global array.
  // Empty aperture_array is fine — it disables apertures globally.
  if (sim.aperture_array.empty()) {
    return;
  }

  std::vector<int> known_uids;
  known_uids.reserve(sim.aperture_array.size());
  for (const auto& ap : sim.aperture_array) {
    known_uids.push_back(static_cast<int>(ap.uid));
  }
  std::ranges::sort(known_uids);

  for (const auto& ph : sim.phase_array) {
    if (ph.apertures.empty()) {
      continue;
    }
    for (const auto& uid : ph.apertures) {
      const auto u = static_cast<int>(uid);
      if (!std::ranges::binary_search(known_uids, u)) {
        result.errors.push_back(std::format(
            "Phase uid={}: aperture uid={} listed in `apertures` does not "
            "exist in `simulation.aperture_array` (declared uids range from "
            "{} to {}).  Either add the aperture definition or remove the "
            "uid from this phase's list.",
            static_cast<int>(ph.uid),
            u,
            known_uids.front(),
            known_uids.back()));
      }
    }
  }
}

/// Validate structural completeness.
void check_completeness(ValidationResult& result, const Planning& planning)
{
  if (planning.system.bus_array.empty()) {
    result.errors.emplace_back("System has no buses defined");
  }

  if (planning.simulation.block_array.empty()) {
    result.errors.emplace_back("Simulation has no blocks defined");
  }

  if (planning.simulation.stage_array.empty()) {
    result.errors.emplace_back("Simulation has no stages defined");
  }
}

/// Tolerance for probability sum comparison.
constexpr double prob_tolerance = 1e-6;

/// Check and optionally rescale scenario probabilities per scene.
void check_scenario_probabilities(ValidationResult& result, Planning& planning)
{
  const auto mode = planning.simulation.probability_rescale.value_or(
      ProbabilityRescaleMode::runtime);
  const bool do_rescale = (mode != ProbabilityRescaleMode::none);

  auto& scenarios = planning.simulation.scenario_array;
  const auto& scenes = planning.simulation.scene_array;

  if (scenarios.empty()) {
    return;
  }

  // When there are no explicit scenes, all scenarios form one implicit scene.
  // Check that the total probability sums to 1.0.
  if (scenes.empty()) {
    double total = 0.0;
    for (const auto& sc : scenarios) {
      total += sc.probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scenario probability_factor values sum to {:.6f} (expected 1.0)",
          total));

      if (do_rescale && total > 0.0) {
        for (auto& sc : scenarios) {
          const double p = sc.probability_factor.value_or(1.0);
          sc.probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled {} scenario probability_factor values to sum 1.0",
            scenarios.size()));
      }
    }
    return;
  }

  // Per-scene: check that scenario probabilities within each scene sum to 1.0.
  // Skip scenes with a single scenario — they are trivially normalised and
  // their probability_factor represents the global weight used for
  // cross-scene aggregation (e.g. SDDP with one scenario per scene).
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;

    if (first + count > scenarios.size()) {
      result.errors.push_back(std::format(
          "Scene '{}': first_scenario ({}) + count_scenario ({}) exceeds "
          "scenario_array size ({})",
          scene.name.value_or("?"),
          first,
          count,
          scenarios.size()));
      continue;
    }

    // Single-scenario scenes need no intra-scene normalisation.
    if (count <= 1) {
      continue;
    }

    double total = 0.0;
    for (std::size_t i = first; i < first + count; ++i) {
      total += scenarios[i].probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scene '{}': scenario probability_factor values sum to {:.6f} "
          "(expected 1.0)",
          scene.name.value_or("?"),
          total));

      if (do_rescale && total > 0.0) {
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled scenario probabilities in scene '{}' to sum 1.0",
            scene.name.value_or("?")));
      }
    }
  }

  // Check that scene-level probability totals sum to 1.0 across all scenes.
  double scene_total = 0.0;
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;
    if (first + count > scenarios.size()) {
      continue;  // already reported above
    }
    for (std::size_t i = first; i < first + count; ++i) {
      scene_total += scenarios[i].probability_factor.value_or(1.0);
    }
  }

  if (std::abs(scene_total - 1.0) > prob_tolerance) {
    result.warnings.push_back(
        std::format("Total scene probability sums to {:.6f} across {} scenes "
                    "(expected 1.0)",
                    scene_total,
                    scenes.size()));

    if (do_rescale && scene_total > 0.0) {
      for (const auto& scene : scenes) {
        const auto first = scene.first_scenario;
        const auto count = (scene.count_scenario == std::dynamic_extent)
            ? (scenarios.size() - first)
            : scene.count_scenario;
        if (first + count > scenarios.size()) {
          continue;
        }
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / scene_total;
        }
      }
      result.warnings.push_back(
          std::format("Rescaled all scenario probabilities across {} scenes to "
                      "sum 1.0",
                      scenes.size()));
    }
  }
}

}  // namespace

[[nodiscard]] ValidationResult validate_planning(Planning& planning)
{
  ValidationResult result;

  check_referential_integrity(result, planning.system);
  check_ranges(result, planning);
  check_positivity(result, planning.system);
  check_piecewise_feasibility(result, planning);
  check_aperture_references(result, planning);
  check_completeness(result, planning);
  check_scenario_probabilities(result, planning);

  // Log all findings
  for (const auto& warn : result.warnings) {
    spdlog::warn("Validation: {}", warn);
  }
  for (const auto& err : result.errors) {
    spdlog::error("Validation: {}", err);
  }

  if (result.ok()) {
    spdlog::info("Planning validation passed");
  } else {
    spdlog::error("Planning validation failed with {} error(s)",
                  result.errors.size());
  }

  return result;
}

}  // namespace gtopt
