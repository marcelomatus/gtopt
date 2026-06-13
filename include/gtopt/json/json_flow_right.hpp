/**
 * @file      json_flow_right.hpp
 * @brief     JSON serialization for FlowRight objects
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The JSON binding accepts the legacy `"discharge"` key as an alias for
 * the new `"target"` key.  Setting both is an error.  The legacy
 * `"use_value"` key has been renamed to `"uvalue"` (no alias kept — the
 * narrowing from `OptTBRealFieldSched` to `OptTRealFieldSched` is a
 * compile-time visible change anyway).
 *
 * The bound triple (`fmin`, `fmax`, `target`, and the legacy
 * `discharge` alias) are bound as `OptTBRealFieldSched` so that
 * round-tripped parquet output (one row per (scenario, stage, block))
 * parses without "duplicate uid" warnings.  Scalar / 1-D shapes still
 * parse via the variant fallback and broadcast across blocks.  The
 * cost fields (`fcost`, `uvalue`) are also `OptTBRealFieldSched`
 * (since PR-C) so a per-(stage, block) cost surface round-trips
 * through parquet; scalar shapes still broadcast.  Block-duration
 * weighting is applied at LP-build time via
 * `CostHelper::block_ecost`.
 */

#pragma once

#include <stdexcept>

#include <daw/json/daw_json_link.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_right_bound_rule.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::FlowRight;
using gtopt::RightBoundRule;

/// Custom constructor so that json_class_null<"bound_rule"> maps
/// absent/null JSON to std::nullopt rather than a default RightBoundRule.
/// Also normalises the legacy `"discharge"` key into `target` for
/// back-compat with existing fixtures.
struct FlowRightConstructor
{
  [[nodiscard]] FlowRight operator()(Uid uid,
                                     Name name,
                                     OptActive active,
                                     OptName purpose,
                                     OptSingleId junction,
                                     OptInt direction,
                                     OptTBRealFieldSched fmin,
                                     OptTBRealFieldSched fmax,
                                     OptTBRealFieldSched target,
                                     OptTBRealFieldSched discharge,
                                     OptName flow_mode,
                                     OptBool use_average,
                                     OptTBRealFieldSched fcost,
                                     OptTBRealFieldSched uvalue,
                                     OptReal priority,
                                     std::optional<RightBoundRule> bound_rule,
                                     OptSingleId bypass_junction,
                                     OptReal bypass_cost,
                                     OptSingleId junction_a,
                                     OptSingleId junction_b,
                                     OptBool consumptive) const
  {
    // Back-compat alias: `discharge` is the legacy name of `target`.
    // Setting both is a JSON error — pick one.
    if (target.has_value() && discharge.has_value()) {
      throw std::invalid_argument(
          "FlowRight: cannot set both 'target' and 'discharge' "
          "(discharge is a legacy alias of target)");
    }
    if (!target.has_value() && discharge.has_value()) {
      target = std::move(discharge);
    }

    // Canonical junction names are `junction_a` / `junction_b` (consistent
    // with Waterway/Turbine).  `junction` / `bypass_junction` are accepted
    // as legacy input aliases; the canonical name wins when both are set.
    // NOTE: a follow-up clean-rename drops the legacy keys (see task).
    if (junction_a.has_value()) {
      junction = std::move(junction_a);
    }
    if (junction_b.has_value()) {
      bypass_junction = std::move(junction_b);
    }

    return FlowRight {
        .uid = uid,
        .name = std::move(name),
        .active = std::move(active),
        .purpose = std::move(purpose),
        .junction = std::move(junction),
        .direction = direction,
        .fmin = std::move(fmin),
        .fmax = std::move(fmax),
        .target = std::move(target),
        .flow_mode = std::move(flow_mode),
        .use_average = use_average,
        .fcost = std::move(fcost),
        .uvalue = std::move(uvalue),
        .priority = priority,
        .bound_rule = std::move(bound_rule),
        .bypass_junction = std::move(bypass_junction),
        .bypass_cost = bypass_cost,
        .consumptive = consumptive,
    };
  }
};

template<>
struct json_data_contract<FlowRight>
{
  using constructor_t = FlowRightConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"purpose", OptName>,
      json_variant_null<"junction", OptSingleId, jvtl_SingleId>,
      json_number_null<"direction", OptInt>,
      json_variant_null<"fmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"fmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"target", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"discharge",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_string_null<"flow_mode", OptName>,
      json_bool_null<"use_average", OptBool>,
      json_variant_null<"fcost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"uvalue", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_number_null<"priority", OptReal>,
      json_class_null<"bound_rule", std::optional<RightBoundRule>>,
      json_variant_null<"bypass_junction", OptSingleId, jvtl_SingleId>,
      json_number_null<"bypass_cost", OptReal>,
      // Canonical junction aliases (consistent with Waterway/Turbine);
      // map onto `junction` / `bypass_junction` in the constructor.
      json_variant_null<"junction_a", OptSingleId, jvtl_SingleId>,
      json_variant_null<"junction_b", OptSingleId, jvtl_SingleId>,
      json_bool_null<"consumptive", OptBool>>;

  constexpr static auto to_json_data(FlowRight const& fr)
  {
    // Emit only the new keys; never re-emit the legacy `discharge`
    // alias so round-tripped JSON uses the canonical name.  The
    // discharge slot's static type widened with the other bound fields.
    static const OptTBRealFieldSched empty_discharge {};
    // `junction_a` / `junction_b` are input-only aliases for now; emit the
    // legacy `junction` / `bypass_junction` keys so existing readers and
    // round-trip fixtures stay unchanged until the clean rename (see task).
    static const OptSingleId empty_alias {};
    return std::forward_as_tuple(fr.uid,
                                 fr.name,
                                 fr.active,
                                 fr.purpose,
                                 fr.junction,
                                 fr.direction,
                                 fr.fmin,
                                 fr.fmax,
                                 fr.target,
                                 empty_discharge,
                                 fr.flow_mode,
                                 fr.use_average,
                                 fr.fcost,
                                 fr.uvalue,
                                 fr.priority,
                                 fr.bound_rule,
                                 fr.bypass_junction,
                                 fr.bypass_cost,
                                 empty_alias,
                                 empty_alias,
                                 fr.consumptive);
  }
};
}  // namespace daw::json
