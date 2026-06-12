/**
 * @file      profile.hpp
 * @brief     Generic time-varying availability profile (kind-tagged)
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `CapacityProfile` is the unified data-struct that replaces the per-owner
 * `DemandProfile` / `GeneratorProfile` pairs.  A profile multiplies an
 * owner's installed capacity by a per-(scenario, stage, block) factor and
 * introduces a slack column (`unserved` for demand, `spillover` for
 * generator) that absorbs the residual.
 *
 * The owner is identified by the pair (`owner_kind`, `owner`).  See
 * `gtopt/capacity_profile_traits.hpp` for the per-kind dispatch (slack name,
 * legacy class-name string, owner-side accessor).
 *
 * ### JSON example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "g3_solar",
 *   "owner_kind": "generator",
 *   "owner": "g3",
 *   "profile": [0,0,0,0,0,0.1,0.4,0.7,0.9,1,...]
 * }
 * ```
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/// Discriminator identifying the owner element type of a `CapacityProfile`.
///
/// Extending this enum is the canonical way to add a new profile-able
/// element type (e.g. `Line`, `Waterway`).  Each new entry must be paired
/// with a `CapacityProfileTraits<OwnerLP>` specialization in
/// `gtopt/capacity_profile_traits.hpp` and a new row in
/// `profile_owner_kind_entries`.
enum class ProfileOwnerKind : std::uint8_t
{
  Generator = 0,
  Demand = 1,
  // Future: Line, Waterway, Battery, ...
};

inline constexpr auto profile_owner_kind_entries =
    std::to_array<EnumEntry<ProfileOwnerKind>>({
        {.name = "generator", .value = ProfileOwnerKind::Generator},
        {.name = "demand", .value = ProfileOwnerKind::Demand},
    });

[[nodiscard]] constexpr auto enum_entries(
    [[maybe_unused]] ProfileOwnerKind kind) noexcept
{
  return std::span {profile_owner_kind_entries};
}

/**
 * @brief Generic profile element (kind-tagged owner FK).
 *
 * @see CapacityProfileLP, CapacityProfileTraits
 */
struct CapacityProfile
{
  /// Canonical class-name constant used in LP row labels.  Note: per-owner
  /// CSV output preserves the legacy label (`"GeneratorProfile"` /
  /// `"DemandProfile"`) via
  /// `CapacityProfileTraits<OwnerLP>::output_class_name`.
  static constexpr LPClassName class_name {"CapacityProfile"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  ProfileOwnerKind owner_kind {};  ///< Discriminator for `owner`
  SingleId owner {unknown_uid};  ///< FK to the owning element

  STBRealFieldSched profile {};  ///< Capacity-factor profile [p.u.]
  OptTRealFieldSched scost {};  ///< Slack column unit cost override [$/MWh]
};

}  // namespace gtopt
