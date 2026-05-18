/**
 * @file      profile_traits.hpp
 * @brief     Per-owner-kind dispatch table for `CapacityProfile` /
 * `CapacityProfileLP`
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `CapacityProfileTraits<OwnerLP>` is the single point where a new profile-able
 * element type is registered.  Each specialization carries:
 *
 *   - `kind`               : `ProfileOwnerKind` discriminator value
 *   - `slack_name`         : per-block slack column display name
 *                            (`"spillover"`, `"unserved"`, …)
 *   - `output_class_name`  : LP-row / CSV label preserved from the legacy
 *                            per-owner profile types (e.g.
 *                            `"GeneratorProfile"`).  Picking this
 *                            string is what keeps existing CSV consumers
 *                            green.
 *   - `cols_at(owner, sc, st)` : returns the per-block column-index map
 *                                of the owner element at (scenario, stage)
 *
 * The legacy `DemandProfileLP` / `GeneratorProfileLP` classes still exist
 * and follow the same dispatch via `ProfileObjectLP::add_profile_to_lp`;
 * this header simply hoists the dispatch into a traits table so the
 * unified `CapacityProfileLP` can switch on `CapacityProfile::owner_kind` at
 * runtime.
 */

#pragma once

#include <gtopt/capacity_profile.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>

namespace gtopt
{

template<typename OwnerLP>
struct CapacityProfileTraits;

template<>
struct CapacityProfileTraits<GeneratorLP>
{
  static constexpr ProfileOwnerKind kind = ProfileOwnerKind::Generator;
  static constexpr std::string_view slack_name = "spillover";
  static constexpr LPClassName output_class_name {"GeneratorProfile"};
  static constexpr std::string_view capacity_warning =
      "GeneratorProfile requires that Generator defines capacity or expansion";

  using SId = GeneratorLPSId;

  [[nodiscard]] static constexpr SId sid(const CapacityProfile& p) noexcept
  {
    return SId {p.owner};
  }

  [[nodiscard]] static constexpr auto&& cols_at(const GeneratorLP& owner,
                                                const ScenarioLP& scenario,
                                                const StageLP& stage)
  {
    return owner.generation_cols_at(scenario, stage);
  }

  [[nodiscard]] static constexpr bool has_capacity(
      const GeneratorLP& owner) noexcept
  {
    return owner.generator().capacity.has_value();
  }
};

template<>
struct CapacityProfileTraits<DemandLP>
{
  static constexpr ProfileOwnerKind kind = ProfileOwnerKind::Demand;
  static constexpr std::string_view slack_name = "unserved";
  static constexpr LPClassName output_class_name {"DemandProfile"};
  static constexpr std::string_view capacity_warning =
      "DemandProfile requires that Demand defines capacity or expansion";

  using SId = DemandLPSId;

  [[nodiscard]] static constexpr SId sid(const CapacityProfile& p) noexcept
  {
    return SId {p.owner};
  }

  [[nodiscard]] static constexpr auto&& cols_at(const DemandLP& owner,
                                                const ScenarioLP& scenario,
                                                const StageLP& stage)
  {
    return owner.load_cols_at(scenario, stage);
  }

  [[nodiscard]] static constexpr bool has_capacity(
      const DemandLP& owner) noexcept
  {
    return owner.demand().capacity.has_value();
  }
};

/// Compile-time visitor over the registered owner-kind ⇄ OwnerLP table.
///
/// Calls `f(std::type_identity<OwnerLP>{})` for the matching kind and
/// returns the result.  Used by `CapacityProfileLP` to fan a single
/// `CapacityProfile::owner_kind` runtime value out to the typed `cols_at` /
/// `sid` / etc. paths above.
///
/// Adding a new kind requires:
///   1. A new `ProfileOwnerKind` enumerator
///   2. A new `CapacityProfileTraits<OwnerLP>` specialization
///   3. A new `case` branch here
template<typename F>
constexpr auto dispatch_profile_owner(ProfileOwnerKind kind, F&& f)
{
  switch (kind) {
    case ProfileOwnerKind::Generator:
      return std::forward<F>(f)(std::type_identity<GeneratorLP> {});
    case ProfileOwnerKind::Demand:
      return std::forward<F>(f)(std::type_identity<DemandLP> {});
  }
  // Unreachable when kind is one of the registered enumerators.
  return std::forward<F>(f)(std::type_identity<GeneratorLP> {});
}

}  // namespace gtopt
