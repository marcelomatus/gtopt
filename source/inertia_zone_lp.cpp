/**
 * @file      inertia_zone_lp.cpp
 * @brief     LP formulation for inertia zones
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements InertiaZoneLP: creates a slack variable per block for the
 * inertia requirement and a >= 0 requirement row.  InertiaProvisionLP
 * injects its provision_factor * r_inertia coefficients into this row.
 */

#include <expected>

#include <gtopt/error.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

std::expected<void, Error> add_requirement(const std::string_view cname,
                                           const SystemContext& sc,
                                           const ScenarioLP& scenario,
                                           const StageLP& stage,
                                           LinearProblem& lp,
                                           const auto& blocks,
                                           const auto uid,
                                           auto& rr,
                                           const auto rname)
{
  using STKey = STBIndexHolder<ColIndex>::key_type;
  if (!rr.req) {
    return {};
  }

  const auto stage_rcost = sc.reserve_fail_cost(stage, rr.cost);

  BIndexHolder<ColIndex> rr_cols;
  BIndexHolder<RowIndex> rr_rows;
  map_reserve(rr_cols, blocks.size());
  map_reserve(rr_rows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_rreq = rr.req.optval(stage.uid(), block.uid());
    if (!block_rreq) {
      continue;
    }

    const auto block_context =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto rcol = stage_rcost
        ? lp.add_col({
              .lowb = 0.0,
              .uppb = block_rreq.value(),
              .cost = -CostHelper::block_ecost(
                  scenario, stage, block, stage_rcost.value()),
              .class_name = cname,
              .variable_name = rname,
              .variable_uid = uid,
              .context = block_context,
          })
        : lp.add_col({
              .lowb = block_rreq.value(),
              .uppb = block_rreq.value(),
              .cost = 0.0,
              .class_name = cname,
              .variable_name = rname,
              .variable_uid = uid,
              .context = block_context,
          });
    rr_cols[buid] = rcol;

    SparseRow rr_row {
        .class_name = cname,
        .constraint_name = rname,
        .variable_uid = uid,
        .context = block_context,
    };
    rr_row[rcol] = -1;
    rr_rows[buid] = lp.add_row(std::move(rr_row.greater_equal(0)));
  }

  // storing the indices for this scenario and stage
  const STKey st_key {scenario.uid(), stage.uid()};
  rr.requirement_cols[st_key] = std::move(rr_cols);
  rr.requirement_rows[st_key] = std::move(rr_rows);

  return {};
}
}  // namespace

namespace gtopt
{

InertiaZoneLP::Requirement::Requirement(const InputContext& ic,
                                        std::string_view cname,
                                        const Id& id,
                                        auto&& rreq,
                                        auto&& rcost)
    : req(ic, cname, id, std::forward<decltype(rreq)>(rreq))
    , cost(ic, cname, id, std::forward<decltype(rcost)>(rcost))
{
}

InertiaZoneLP::InertiaZoneLP(const InertiaZone& inertia_zone,
                             const InputContext& ic)
    : Base(inertia_zone)
    , rr(ic,
         ClassName,
         id(),
         std::move(this->inertia_zone().requirement),
         std::move(this->inertia_zone().cost))
{
}

bool InertiaZoneLP::add_to_lp(const SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.full_name();
  static constexpr auto ampl_name = ClassName.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  if (auto res = add_requirement(
          cname, sc, scenario, stage, lp, blocks, uid(), rr, RequirementName);
      !res)
  {
    SPDLOG_WARN(
        "add_requirement failed for uid={}: {}", uid(), res.error().message);
    return false;
  }

  // Register PAMPL-visible requirement columns
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  if (const auto it = rr.requirement_cols.find(st_key);
      it != rr.requirement_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), ReqName, scenario, stage, it->second);
  }

  return true;
}

bool InertiaZoneLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, RequirementName, pid, rr.requirement_cols);
  out.add_col_cost(cname, RequirementName, pid, rr.requirement_cols);
  out.add_row_dual(cname, RequirementName, pid, rr.requirement_rows);

  return true;
}

}  // namespace gtopt
