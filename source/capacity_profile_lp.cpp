/**
 * @file      capacity_profile_lp.cpp
 * @brief     Unified `CapacityProfileLP` implementation (kind-dispatched)
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/capacity_profile_lp.hpp>
#include <gtopt/capacity_profile_traits.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Pick the legacy per-owner class-name string at construction time.
/// Used as the `cname` of the inherited `ProfileObjectLP` so the
/// schedule resolver looks for input files under the same directory
/// the legacy `DemandProfile` / `GeneratorProfile` types used
/// (`input_directory/GeneratorProfile/...`, etc.).
[[nodiscard]] LPClassName legacy_class_name_for(ProfileOwnerKind kind) noexcept
{
  return dispatch_profile_owner(
      kind,
      []<typename OwnerLP>(std::type_identity<OwnerLP>)
      { return CapacityProfileTraits<OwnerLP>::output_class_name; });
}

}  // namespace

CapacityProfileLP::CapacityProfileLP(const CapacityProfile& pprofile,
                                     InputContext& ic)
    : ProfileObjectLP(pprofile, ic, legacy_class_name_for(pprofile.owner_kind))
{
}

bool CapacityProfileLP::add_to_lp(const SystemContext& sc,
                                  const ScenarioLP& scenario,
                                  const StageLP& stage,
                                  LinearProblem& lp)
{
  const auto& p = profile_element();
  return dispatch_profile_owner(
      p.owner_kind,
      [&]<typename OwnerLP>(std::type_identity<OwnerLP>)
      {
        using Traits = CapacityProfileTraits<OwnerLP>;

        auto&& owner = sc.element<OwnerLP>(Traits::sid(p));
        if (!owner.is_active(stage)) {
          return true;
        }

        auto&& element_cols = Traits::cols_at(owner, scenario, stage);

        const auto [opt_capacity, capacity_col] =
            owner.capacity_and_col(stage, lp);
        const double stage_capacity =
            opt_capacity.value_or(LinearProblem::DblMax);

        if (!capacity_col && !Traits::has_capacity(owner)) {
          SPDLOG_WARN("{}", Traits::capacity_warning);
          return false;
        }

        return add_profile_to_lp(Traits::output_class_name.full_name(),
                                 scenario,
                                 stage,
                                 lp,
                                 Traits::slack_name,
                                 element_cols,
                                 capacity_col,
                                 stage_capacity);
      });
}

bool CapacityProfileLP::add_to_output(OutputContext& out) const
{
  const auto kind = profile_element().owner_kind;
  return dispatch_profile_owner(
      kind,
      [&]<typename OwnerLP>(std::type_identity<OwnerLP>)
      {
        using Traits = CapacityProfileTraits<OwnerLP>;
        return add_profile_to_output(
            Traits::output_class_name.full_name(), out, Traits::slack_name);
      });
}

bool CapacityProfileLP::update_aperture(
    LinearInterface& li,
    const ScenarioLP& base_scenario,
    const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
    const StageLP& stage) const
{
  return ProfileObjectLP::update_aperture(li, base_scenario, value_fn, stage);
}

}  // namespace gtopt
