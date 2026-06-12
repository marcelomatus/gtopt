/*
 *
 */
#include <expected>

#include <gtopt/error.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_zone_lp.hpp>
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
  if (!rr.req && !rr.min) {
    return {};
  }

  BIndexHolder<ColIndex> rr_cols;
  BIndexHolder<RowIndex> rr_rows;
  map_reserve(rr_cols, blocks.size());
  map_reserve(rr_rows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_rreq = rr.req.optval(stage.uid(), block.uid());
    const auto block_rmin = rr.min.optval(stage.uid(), block.uid());
    // Effective requirement = max(time-varying requirement, Min-Provision
    // floor).  Keeping ``min`` separate from ``req`` lets the JSON / PAMPL
    // ``req`` schedule mirror PLEXOS's reported per-block RHS while the static
    // floor still binds on low-requirement blocks (PLEXOS ``Min Provision``).
    double effective = 0.0;
    bool has_req = false;
    if (block_rreq) {
      effective = *block_rreq;
      has_req = true;
    }
    if (block_rmin) {
      effective = std::max(effective, *block_rmin);
      has_req = true;
    }
    if (!has_req) {
      continue;
    }
    const double block_rreq_eff = effective;

    // LP-size: a zero effective requirement makes the requirement row
    // ``Σ pf · prov ≥ 0`` — trivially satisfied because every provision
    // factor ``pf > 0`` and every provision column has lowb ≥ 0, so it
    // can never bind.  The priced branch would also create a fixed-zero
    // slack column (uppb = block_rreq_eff = 0).  Skip both.
    //
    // Write-out rule: a zero-requirement block needs zero reserve
    // shortage, so the (absent) requirement column reads 0 and the
    // (absent) row dual is 0 — the natural zero rendered as no-row.
    if (block_rreq_eff == 0.0) {
      continue;
    }

    // `rr.cost` is now per-(stage, block): resolve per block via the
    // overload that consults `model_options.reserve_shortage_cost` as
    // fallback (mirrors PR-A's `demand_fail_cost(stage, block, fcost)`).
    const auto block_rcost = sc.reserve_shortage_cost(stage, block, rr.cost);

    const auto block_context =
        make_block_context(scenario.uid(), stage.uid(), block.uid());

    SparseRow rr_row {
        .class_name = cname,
        .constraint_name = rname,
        .variable_uid = uid,
        .context = block_context,
    };

    if (block_rcost) {
      // Priced slack branch (soft requirement): emit the bookkeeping
      // requirement column and the row `Σ pf · prov − rcol ≥ 0`.  The
      // column is bounded by `[0, block_rreq]` and carries a negative
      // objective coefficient so the LP picks `rcol = block_rreq`
      // whenever the provisions can supply it, and drops to a lower
      // value (paying the shortage penalty in the objective) otherwise.
      const auto rcol = lp.add_col({
          .lowb = 0.0,
          .uppb = block_rreq_eff,
          .cost = -CostHelper::block_ecost(
              scenario, stage, block, block_rcost.value()),
          .class_name = cname,
          .variable_name = rname,
          .variable_uid = uid,
          .context = block_context,
      });
      rr_cols[buid] = rcol;
      rr_row[rcol] = -1;
      rr_rows[buid] = lp.add_row(std::move(rr_row.greater_equal(0)));
    } else {
      // Hard requirement, no shortage cost: the legacy form built a
      // fully-pinned bookkeeping column (`lowb = uppb = block_rreq,
      // cost = 0`) and a row `Σ pf · prov − rcol ≥ 0`.  Substitute the
      // column out — move `block_rreq` to the row's RHS so the row
      // reads `Σ pf · prov ≥ block_rreq` directly.  Saves one LP
      // column per (zone, scenario, stage, block, up-or-dn) in every
      // configuration that omits a reserve-shortage cost.  The PAMPL
      // `reserve_zone(X).up` / `.dn` accessor is not registered for
      // this zone in the no-cost branch (rr_cols stays empty), so user
      // constraints referencing it on a hard-requirement zone now hit
      // a strict resolver error instead of silently anchoring to the
      // (pinned) column value — see issue #529 for the trade-off.
      rr_rows[buid] =
          lp.add_row(std::move(rr_row.greater_equal(block_rreq_eff)));
    }
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

ReserveZoneLP::Requirement::Requirement(const InputContext& ic,
                                        std::string_view cname,
                                        const Id& id,
                                        auto&& rreq,
                                        auto&& rmin,
                                        auto&& rcost)
    : req(ic, cname, id, std::forward<decltype(rreq)>(rreq))
    , min(ic, cname, id, std::forward<decltype(rmin)>(rmin))
    , cost(ic, cname, id, std::forward<decltype(rcost)>(rcost))
{
}

ReserveZoneLP::ReserveZoneLP(const ReserveZone& preserve_zone,
                             const InputContext& ic)
    : Base(preserve_zone)
    , ur(ic,
         Element::class_name,
         id(),
         std::move(reserve_zone().urreq),
         std::move(reserve_zone().urmin),
         std::move(reserve_zone().urcost))
    , dr(ic,
         Element::class_name,
         id(),
         std::move(reserve_zone().drreq),
         std::move(reserve_zone().drmin),
         std::move(reserve_zone().drcost))
{
}

bool ReserveZoneLP::add_to_lp(const SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  if (auto res = add_requirement(
          cname, sc, scenario, stage, lp, blocks, uid(), ur, UrequirementName);
      !res)
  {
    SPDLOG_WARN("add_requirement (ureq) failed for uid={}: {}",
                uid(),
                res.error().message);
    return false;
  }
  if (auto res = add_requirement(
          cname, sc, scenario, stage, lp, blocks, uid(), dr, DrequirementName);
      !res)
  {
    SPDLOG_WARN("add_requirement (dreq) failed for uid={}: {}",
                uid(),
                res.error().message);
    return false;
  }

  // Register PAMPL-visible requirement columns under the canonical
  // `up` / `dn` spelling.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  if (const auto it = ur.requirement_cols.find(st_key);
      it != ur.requirement_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(ampl_name, uid(), UpName, scenario, stage, it->second);
  }
  if (const auto it = dr.requirement_cols.find(st_key);
      it != dr.requirement_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(ampl_name, uid(), DnName, scenario, stage, it->second);
  }

  return true;
}

bool ReserveZoneLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, UrequirementName, pid, ur.requirement_cols);
  out.add_col_cost(cname, UrequirementName, pid, ur.requirement_cols);
  out.add_row_dual(cname, UrequirementName, pid, ur.requirement_rows);

  out.add_col_sol(cname, DrequirementName, pid, dr.requirement_cols);
  out.add_col_cost(cname, DrequirementName, pid, dr.requirement_cols);
  out.add_row_dual(cname, DrequirementName, pid, dr.requirement_rows);

  return true;
}
}  // namespace gtopt
