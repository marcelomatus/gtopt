#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>

namespace
{

using namespace gtopt;

constexpr bool add_provision(const SystemContext& sc,
                             const ScenarioLP& scenario,
                             const StageLP& stage,
                             LinearProblem& lp,
                             const auto& blocks,
                             const auto capacity_col,
                             const auto& generation_cols,
                             const Uid uid,
                             auto& rp,
                             const std::string_view pname,
                             const auto& requirement_rows,
                             auto provision_row)
{
  constexpr std::string_view cname = "rprov";

  const auto stage_provision_factor = rp.provision_factor.optval(stage.uid());
  if (!(stage_provision_factor) || (stage_provision_factor.value() <= 0.0)) {
    return true;
  }

  const auto stage_cost = rp.cost.optval(stage.uid()).value_or(0.0);
  const auto stage_capacity_factor = rp.capacity_factor.optval(stage.uid());
  const auto use_capacity = capacity_col && stage_capacity_factor;

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto gcol = generation_cols.at(buid);
    const auto stb_k = std::tuple {scenario.uid(), stage.uid(), buid};

    const auto requirement_row = get_optvalue(requirement_rows, stb_k);
    if (!requirement_row) {
      continue;
    }

    auto provision_col = get_optvalue(rp.provision_cols, stb_k);
    if (!provision_col) {
      //
      // create the provision col and row when needed and if possible, i.e.,
      // if there is a rmax provision defined for the stage and block
      //
      auto block_rmax = rp.max.optval(stage.uid(), buid);
      if (!block_rmax) {
        if (use_capacity) {
          block_rmax = lp.get_col_uppb(gcol);
        } else {
          continue;
        }
      }

      const auto block_rcost =
          sc.block_ecost(scenario, stage, block, stage_cost);
      const auto name = sc.lp_label(scenario, stage, block, cname, pname, uid);
      const auto rcol = lp.add_col(
          {.name = name, .uppb = block_rmax.value(), .cost = block_rcost});

      rp.provision_cols[stb_k] = rcol;
      rp.provision_rows[stb_k] = lp.add_row(provision_row(name, gcol, rcol));

      if (use_capacity) {
        auto crow =
            SparseRow {.name = sc.lp_label("cap", name)}.greater_equal(0);
        crow[capacity_col.value()] = stage_capacity_factor.value();
        crow[rcol] = -1;
        rp.capacity_rows[stb_k] = lp.add_row(std::move(crow));
      }

      provision_col = rcol;
    }

    //
    // add the reserve provision to the requirement balance
    //
    lp.set_coeff(  //
        requirement_row.value(),
        provision_col.value(),
        stage_provision_factor.value());
  }

  return true;
}

constexpr std::vector<std::string> split(std::string_view str, char delim = ' ')
{
  std::vector<std::string> result;

  auto view =
      str | std::views::split(delim)
      | std::views::transform(
          [](auto&& range) { return std::string(range.begin(), range.end()); });

  std::ranges::copy(view, std::back_inserter(result));
  return result;
}

constexpr auto make_rzone_indexes(const InputContext& ic,
                                  const std::string& rzstr)
{
  auto rzones = split(rzstr, ':');

  auto is_uid = [](const auto& s)
  { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); };
  auto str2uid = [](const auto& s) { return static_cast<Uid>(std::stoi(s)); };

  return rzones
      | ranges::views::transform(
             [&](auto rz)
             {
               using RZoneId = ObjectSingleId<ReserveZoneLP>;
               return ic.element_index(is_uid(rz) ? RZoneId {str2uid(rz)}
                                                  : RZoneId {std::move(rz)});
             })
      | ranges::to<std::vector>;
}

}  // namespace

namespace gtopt
{

ReserveProvisionLP::Provision::Provision(const InputContext& ic,
                                         std::string_view cname,
                                         const Id& id,
                                         auto&& rmax,
                                         auto&& rcost,
                                         auto&& rcapf,
                                         auto&& rprof)
    : max(ic, cname, id, std::forward<decltype(rmax)>(rmax))
    , cost(ic, cname, id, std::forward<decltype(rcost)>(rcost))
    , capacity_factor(ic, cname, id, std::forward<decltype(rcapf)>(rcapf))
    , provision_factor(ic, cname, id, std::forward<decltype(rprof)>(rprof))
{
}

ReserveProvisionLP::ReserveProvisionLP(const InputContext& ic,
                                       ReserveProvision preserve_provision)
    : Base(std::move(preserve_provision))
    , up(ic,
         ClassName,
         id(),
         std::move(reserve_provision().urmax),
         std::move(reserve_provision().urcost),
         std::move(reserve_provision().ur_capacity_factor),
         std::move(reserve_provision().ur_provision_factor))
    , dp(ic,
         ClassName,
         id(),
         std::move(reserve_provision().drmax),
         std::move(reserve_provision().drcost),
         std::move(reserve_provision().dr_capacity_factor),
         std::move(reserve_provision().dr_provision_factor))
    , generator_index(ic.element_index(generator()))
    , reserve_zone_indexes(
          make_rzone_indexes(ic, reserve_provision().reserve_zones))
{
}

bool ReserveProvisionLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  auto&& generation_cols = generator_lp.generation_cols_at(scenario, stage);

  const auto [stage_capacity, capacity_col] =
      generator_lp.capacity_and_col(stage, lp);

  auto uprov_row = [&](const auto& row_name, auto gcol, auto rcol)
  {
    auto rrow = SparseRow {.name = row_name}.less_equal(lp.get_col_uppb(gcol));
    rrow[rcol] = rrow[gcol] = 1;

    if (capacity_col) {
      rrow[*capacity_col] = -1;
      return rrow.less_equal(0.0);
    }
    return rrow;
  };

  auto dprov_row = [&](const auto& row_name, auto gcol, auto rcol)
  {
    auto rrow =
        SparseRow {.name = row_name}.greater_equal(lp.get_col_lowb(gcol));
    rrow[gcol] = 1;
    rrow[rcol] = -1;
    return rrow;
  };

  const auto& blocks = stage.blocks();

  for (auto&& reserve_zone_index : reserve_zone_indexes) {
    auto&& reserve_zone = sc.element(reserve_zone_index);
    if (!reserve_zone.is_active(stage)) {
      continue;
    }

    const bool result = add_provision(sc,
                                      scenario,
                                      stage,
                                      lp,
                                      blocks,
                                      capacity_col,
                                      generation_cols,
                                      uid(),
                                      up,
                                      "uprov",
                                      reserve_zone.urequirement_rows(),
                                      uprov_row)
        && add_provision(sc,
                         scenario,
                         stage,
                         lp,
                         blocks,
                         capacity_col,
                         generation_cols,
                         uid(),
                         dp,
                         "dprov",
                         reserve_zone.drequirement_rows(),
                         dprov_row);
    if (!result) {
      return false;
    }
  }

  return true;
}

bool ReserveProvisionLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;
  const auto pid = id();

  out.add_col_sol(cname, "uprovision", pid, up.provision_cols);
  out.add_col_cost(cname, "uprovision", pid, up.provision_cols);
  out.add_row_dual(cname, "uprovision", pid, up.provision_rows);
  out.add_row_dual(cname, "ucapacity", pid, up.capacity_rows);

  out.add_col_sol(cname, "dprovision", pid, dp.provision_cols);
  out.add_col_cost(cname, "dprovision", pid, dp.provision_cols);
  out.add_row_dual(cname, "dprovision", pid, dp.provision_rows);
  out.add_row_dual(cname, "dcapacity", pid, dp.capacity_rows);

  return true;
}

}  // namespace gtopt
