/**
 * @file      emission_zone_lp.cpp
 * @brief     LP-active wiring for EmissionZone — cap row only
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The per-block `production` column and `balance` row have been
 * substituted out: every emission contribution is wired directly on
 * the generator dispatch column by `EmissionSourceLP`, using the
 * per-block scalar
 *
 *   α_{s,b}  =  weight_z,p · (1 − capture_s,p) · (rate_s + upstream_s) · dur_b
 *
 * (see `emission_source_lp.cpp`).  This file therefore only owns the
 * optional **cap row** (and its slack column for soft caps).
 *
 * Per (scenario, stage), if `cap` is set:
 *
 *   row[cap]:  Σ_b Σ_{s ∈ sources(z)} α_{s,b} · gen_{s,b}  ≤  cap
 *              (+ slack with cap_cost penalty when cap_cost set)
 *
 * The cap row is created empty here — `EmissionSourceLP::add_to_lp`
 * runs afterwards and stamps the per-block α coefficients on the
 * matching generator dispatch columns.  Cap coverage when an
 * allowance pool is configured is mediated by the pool's banked SoC
 * instead, so the per-stage cap row is skipped on those zones.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

EmissionZoneLP::EmissionZoneLP(const EmissionZone& zone, const InputContext& ic)
    : Base(zone, ic, Element::class_name)
    , cap_(ic, Element::class_name, id(), std::move(object().cap))
    , cap_cost_(ic, Element::class_name, id(), std::move(object().cap_cost))
    , price_(ic, Element::class_name, id(), std::move(object().price))
{
}

bool EmissionZoneLP::add_to_lp([[maybe_unused]] const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  const auto stage_uid = stage.uid();
  const auto scen_uid = scenario.uid();
  const auto stage_cap = param_cap(stage_uid);
  const auto stage_cap_cost = param_cap_cost(stage_uid);
  const auto pool_fk = emission_zone().allowance_pool;

  // The cap row is only needed when ``cap`` is set AND the zone is
  // not mediated by an allowance pool (in which case the pool's banked
  // SoC plays the role of a multi-stage cap with banking — the source
  // injects its α coefficient directly onto the pool's energy row).
  // Pure reporting-only zones (no cap, no pool) build no LP rows at
  // all.  The per-stage `price` and `emissions_mode` adders likewise
  // require no row of their own — `EmissionSourceLP` adds them onto
  // each generator dispatch column at the same per-block α scale,
  // mirroring the existing CCS opex adder.
  if (!stage_cap.has_value() || pool_fk.has_value()) {
    return true;
  }

  SparseRow cap_row {
      // Emission-cap dual is a $/tonne scarcity price (commodity /
      // duration-independent → Energy time-basis).
      .cost_scale_type = ConstraintScaleType::Energy,
      .class_name = cname,
      .constraint_name = CapName,
      .variable_uid = uid(),
      .context = make_stage_context(scen_uid, stage_uid),
  };

  // Soft cap: introduce a slack column with cap_cost penalty so the
  // LP can violate the cap at a (per-ton) cost rather than infeas.
  // The cap row sums an absolute per-block tonnage (Σ_b α_{s,b} ·
  // gen_{s,b}), so the overage slack is itself an absolute tonnage
  // [tons] and its penalty is the per-ton ``cap_cost`` scaled only by
  // probability · discount — NO duration (cf. the `:cap_cost`
  // comment in the legacy code).
  const std::tuple st_key {scen_uid, stage_uid};
  if (stage_cap_cost) {
    const auto slack_cost = *stage_cap_cost
        * CostHelper::cost_factor(scenario.probability_factor(),
                                  stage.discount_factor());
    const auto slack = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = slack_cost,
        .class_name = cname,
        .variable_name = CapSlackName,
        .variable_uid = uid(),
        .context = make_stage_context(scen_uid, stage_uid),
    });
    cap_row[slack] = -1.0;
    cap_slack_cols_[st_key] = slack;
  }

  // The cap row is created EMPTY here — its per-block α generator-
  // column coefficients are stamped by `EmissionSourceLP::add_to_lp`
  // afterwards (it runs later in the visitor order, see
  // `lp_element_types_t`).
  cap_rows_[st_key] = lp.add_row(std::move(cap_row).less_equal(*stage_cap));

  return true;
}

bool EmissionZoneLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // Per-(scenario, stage) cap streams.  Production / balance streams
  // are gone — the per-source `EmissionSource/*` streams remain the
  // single source of truth for zone-aggregated emissions (post-
  // processors aggregate by zone if needed).
  out.add_row_dual(cname, CapName, pid, cap_rows_);
  if (!cap_slack_cols_.empty()) {
    out.add_col_sol(cname, CapSlackName, pid, cap_slack_cols_);
    out.add_col_cost(cname, CapSlackName, pid, cap_slack_cols_);
  }

  return true;
}

}  // namespace gtopt
