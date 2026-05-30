/**
 * @file      constraint_directive.hpp
 * @brief     Typed metadata directive attached to a UserConstraint
 * @date      2026-05-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A ``ConstraintDirective`` is the typed-schema replacement for the legacy
 * name-regex / expression-substring classification that lived in the
 * ``plexos2gtopt`` converter (``_RegRange_``, ``Gas_MaxOpDay\d+_``,
 * ``count("reserve_provision(") >= 2``…) and the hardcoded soft-penalty
 * ladder (``_RESERVE_PROVISION_SUM_PENALTY = 1000``, ``_FUEL_OFFTAKE_PENALTY``,
 * …) that decided the policy in Python.  By moving the classification onto
 * the constraint itself, the policy is auditable in the emitted JSON, can be
 * overridden per-bundle without code changes, and the converter side
 * collapses to "tag the directive at construction time and forget about it".
 *
 * Step 1 of the AMPL/PAMPL modernization plan (2026-05-30): scaffold only —
 * the directive is OPTIONAL on every ``UserConstraint``; behaviour is
 * unchanged when absent.  Each subsequent migration site (RegRange,
 * Gas_MaxOpDay, MinProvision) ships independently.
 *
 * The PAMPL surface syntax ``@regrange(penalty=1000)`` is reserved for
 * Step 1b — JSON-emitting converters (the only consumer today) can populate
 * the sibling field directly.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/enum_option.hpp>

namespace gtopt
{

/// Constraint-family discriminator.  Each value selects a typed payload
/// shape (validated by ``ConstraintDirective::valid_for_kind()``) and a
/// policy (penalty tier, daily aggregation, gating) that ``UserConstraintLP``
/// applies at LP-build time.
///
/// Kept ``uint8_t`` so the enclosing ``ConstraintDirective`` stays cheap to
/// pass by value.
enum class DirectiveKind : std::uint8_t
{
  /// PLEXOS regulation-range UC (``*_RegRange_e[12]``).  Couples
  /// ``commitment(g).status`` with ``reserve_provision(g, .).{up|dn}``;
  /// LP-relax needs the directive's soft-penalty override to absorb the
  /// implied-bounds chain into a finite-cost slack (task #51).
  RegRange = 0,

  /// ``Σ_g reserve_provision(g, R).{up|dn} ≥ requirement`` aggregator.
  /// Covers the CSF / CPF / CTF ``*MinProvision`` and ``*UpMinProvision``
  /// shapes.  Same soft-penalty tier as RegRange.
  ReserveProvSum = 1,

  /// Per-day cumulative budget (typical scope tags: ``fuel:GAS``,
  /// ``owner:CSF``, ``region:Z1``).  Implies ``daily_sum = true`` at
  /// LP-build time.  Subsumes the ``_consolidate_gas_maxopday_groups``
  /// post-processor that today rewrites Gas_MaxOpDay rows in
  /// ``plexos2gtopt/parsers.py``.
  DailyBudget = 2,

  /// Reservoir floor gated on a commitment-status variable (``efin ≥ X``
  /// only when the unit is on).  Reserved for the P2 follow-up — wired
  /// here so the JSON schema is stable, but ``UserConstraintLP`` does not
  /// yet honour the gate (treated as a pass-through directive in Step 1).
  HydroFloor = 3,

  /// ``Σ_{τ ∈ window(N h)} startup(g, τ) ≤ MaxStarts(g)``.  Reserved for
  /// the P2 ordered-time primitives (``window(N h, expr)``).  Step 1
  /// honours ``window_hours = 24`` as a DailyBudget alias; other windows
  /// are wired but no-op until P2.
  MaxStartsWindow = 4,
};

inline constexpr auto directive_kind_entries =
    std::to_array<EnumEntry<DirectiveKind>>({
        {
            .name = "regrange",
            .value = DirectiveKind::RegRange,
        },
        {
            .name = "reserve_prov_sum",
            .value = DirectiveKind::ReserveProvSum,
        },
        {
            .name = "daily_budget",
            .value = DirectiveKind::DailyBudget,
        },
        {
            .name = "hydro_floor",
            .value = DirectiveKind::HydroFloor,
        },
        {
            .name = "max_starts_window",
            .value = DirectiveKind::MaxStartsWindow,
        },
    });

[[nodiscard]] constexpr auto enum_entries(DirectiveKind /*tag*/) noexcept
{
  return std::span {directive_kind_entries};
}

/// Typed metadata payload for a UserConstraint directive.
///
/// Only the fields that ``kind`` admits are expected to be populated; the
/// rest must stay ``std::nullopt``.  ``valid_for_kind()`` is the canonical
/// invariant used by the JSON loader (to reject bad inputs at parse time)
/// and by the LP-build dispatch (defensive check).
///
/// The struct is intentionally tiny — three optionals + an enum — so it can
/// live by-value on ``UserConstraint`` without growing the per-element
/// memory footprint meaningfully.
struct ConstraintDirective
{
  DirectiveKind kind {};

  /// Soft-penalty override [$ / unit-of-violation].  When set, wins over
  /// ``UserConstraint::penalty`` at LP-build time so directive policy is
  /// the single source of truth for the soft tier.  Populated for
  /// ``RegRange`` / ``ReserveProvSum`` (and any directive that wants a
  /// kind-specific cost).  Forbidden for ``HydroFloor`` /
  /// ``MaxStartsWindow``.
  OptReal penalty {};

  /// Aggregation / classification scope tag — free-form ``"key:value"``
  /// pair (e.g. ``"fuel:GAS"``, ``"owner:CSF"``).  Used by
  /// ``DailyBudget`` and ``HydroFloor`` (where it carries the gating
  /// ``element_class:uid`` reference).  Forbidden for the others.
  OptName scope {};

  /// Rolling window length in HOURS.  Required and ``> 0`` for
  /// ``MaxStartsWindow``; forbidden elsewhere.  ``24`` is the canonical
  /// daily window; ``168`` the canonical weekly window.
  OptInt window_hours {};

  /// True when the populated fields are consistent with ``kind``.
  ///
  /// Enforced at JSON-load time (``json_user_constraint.hpp``) and as a
  /// defensive precondition inside ``UserConstraintLP``.
  [[nodiscard]] constexpr bool valid_for_kind() const noexcept
  {
    switch (kind) {
      case DirectiveKind::RegRange:
      case DirectiveKind::ReserveProvSum:
        return !scope.has_value() && !window_hours.has_value();
      case DirectiveKind::DailyBudget:
        return !window_hours.has_value();
      case DirectiveKind::HydroFloor:
        return !penalty.has_value() && !window_hours.has_value();
      case DirectiveKind::MaxStartsWindow:
        return window_hours.has_value() && *window_hours > 0
            && !penalty.has_value() && !scope.has_value();
    }
    return false;
  }

  /// Effective soft-penalty cost for the LP-build layer.  Directive
  /// override wins; falls back to the constraint-level scalar so legacy
  /// JSON that sets ``penalty`` directly keeps working unchanged.
  [[nodiscard]] constexpr auto effective_penalty(
      OptReal scalar_penalty) const noexcept -> OptReal
  {
    if (penalty.has_value()) {
      return penalty;
    }
    return scalar_penalty;
  }

  /// True when the directive implies a per-day aggregator.  Caller uses
  /// this to override ``UserConstraint::daily_sum = true`` at LP-build
  /// time without mutating the underlying ``UserConstraint``.
  ///
  /// ``DailyBudget`` always; ``MaxStartsWindow`` only when the window
  /// happens to be 24h (the daily-aggregator-aliased case).  Wider
  /// windows fall through to Step P2's generic ``window(N h, expr)``
  /// primitive.
  [[nodiscard]] constexpr bool implies_daily_sum() const noexcept
  {
    if (kind == DirectiveKind::DailyBudget) {
      return true;
    }
    if (kind == DirectiveKind::MaxStartsWindow) {
      return window_hours.value_or(0) == 24;
    }
    return false;
  }

  friend constexpr bool operator==(const ConstraintDirective&,
                                   const ConstraintDirective&) = default;
};

/// Optional directive carried on every ``UserConstraint``.
using OptConstraintDirective = std::optional<ConstraintDirective>;

}  // namespace gtopt
