#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/system_lp.hpp>

namespace
{
using namespace gtopt;

constexpr bool add_requirement(const std::string_view cname,
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
  if (!(rr.req.has_value())) {
    return true;
  }

  const auto stage_rcost = sc.reserve_fail_cost(stage, rr.cost);

  BIndexHolder<ColIndex> rr_cols;
  BIndexHolder<RowIndex> rr_rows;
  rr_cols.reserve(blocks.size());
  rr_rows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_rreq = rr.req.optval(stage.uid(), block.uid());
    if (!block_rreq) {
      continue;
    }

    const auto name = sc.lp_label(scenario, stage, block, cname, rname, uid);
    const auto rcol = stage_rcost
        ? lp.add_col({
              .name = name,
              .lowb = 0.0,
              .uppb = block_rreq.value(),
              .cost =
                  -sc.block_ecost(scenario, stage, block, stage_rcost.value()),
          })
        : lp.add_col({
              .name = name,
              .lowb = block_rreq.value(),
              .uppb = block_rreq.value(),
              .cost = 0.0,
          });
    rr_cols[buid] = rcol;

    SparseRow rr_row {.name = std::move(name)};
    rr_row[rcol] = -1;
    rr_rows[buid] = lp.add_row(std::move(rr_row.greater_equal(0)));
  }

  // storing the indices for this scenario and stage
  const STKey st_key {scenario.uid(), stage.uid()};
  rr.requirement_cols[st_key] = std::move(rr_cols);
  rr.requirement_rows[st_key] = std::move(rr_rows);

  return true;
}
}  // namespace

namespace gtopt
{

ReserveZoneLP::Requirement::Requirement(const InputContext& ic,
                                        const std::string_view& cname,
                                        const Id& id,
                                        auto&& rreq,
                                        auto&& rcost)
    : req(ic, cname, id, std::forward<decltype(rreq)>(rreq))
    , cost(ic, cname, id, std::forward<decltype(rcost)>(rcost))
{
}

ReserveZoneLP::ReserveZoneLP(ReserveZone preserve_zone, const InputContext& ic)
    : Base(std::move(preserve_zone))
    , ur(ic,
         ClassName,
         id(),
         std::move(reserve_zone().urreq),
         std::move(reserve_zone().urcost))
    , dr(ic,
         ClassName,
         id(),
         std::move(reserve_zone().drreq),
         std::move(reserve_zone().drcost))
{
}

bool ReserveZoneLP::add_to_lp(const SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  return add_requirement(
             cname, sc, scenario, stage, lp, blocks, uid(), ur, "ureq")
      && add_requirement(
             cname, sc, scenario, stage, lp, blocks, uid(), dr, "dreq");
}

bool ReserveZoneLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "urequirement", pid, ur.requirement_cols);
  out.add_col_cost(cname, "urequirement", pid, ur.requirement_cols);
  out.add_row_dual(cname, "urequirement", pid, ur.requirement_rows);

  out.add_col_sol(cname, "drequirement", pid, dr.requirement_cols);
  out.add_col_cost(cname, "drequirement", pid, dr.requirement_cols);
  out.add_row_dual(cname, "drequirement", pid, dr.requirement_rows);

  return true;
}
}  // namespace gtopt
