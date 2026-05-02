/**
 * @file      inertia_provision_lp.cpp
 * @brief     LP formulation for inertia provisions
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements InertiaProvisionLP: creates an r_inertia variable per block
 * and adds a downward coupling constraint:
 *   p - r_inertia >= 0  (generator must produce at least r_inertia)
 *
 * The provision_factor (FE = H*S/Pmin [MWs/MW]) is injected as a
 * coefficient on r_inertia into the InertiaZone requirement row.
 */

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <spdlog/spdlog.h>

namespace
{

using namespace gtopt;

std::vector<std::string> split(std::string_view str, char delim = ' ')
{
  std::vector<std::string> result;

  auto view =
      str | std::views::split(delim)
      | std::views::transform(
          [](auto&& range) { return std::string(range.begin(), range.end()); });

  std::ranges::copy(view, std::back_inserter(result));
  return result;
}

auto make_izone_indexes(const InputContext& ic, const std::string& izstr)
{
  auto izones = split(izstr, ':');

  auto is_uid = [](const auto& s)
  { return !s.empty() && std::ranges::all_of(s, ::isdigit); };
  auto str2uid = [](const auto& s) { return static_cast<Uid>(std::stoi(s)); };

  return std::ranges::to<std::vector>(
      izones
      | std::views::transform(
          [&](auto iz)
          {
            using IZoneId = ObjectSingleId<InertiaZoneLP>;
            return ic.element_index(is_uid(iz) ? IZoneId {str2uid(iz)}
                                               : IZoneId {std::move(iz)});
          }));
}

}  // namespace

namespace gtopt
{

InertiaProvisionLP::InertiaProvisionLP(const InertiaProvision& ip,
                                       const InputContext& ic)
    : Base(ip)
    , generator_index_(ic.element_index(generator_sid()))
    , inertia_zone_indexes_(
          make_izone_indexes(ic, inertia_provision().inertia_zones))
    , provision_max_(ic,
                     Element::class_name,
                     id(),
                     std::move(inertia_provision().provision_max))
    , provision_factor_(ic,
                        Element::class_name,
                        id(),
                        std::move(inertia_provision().provision_factor))
    , cost_(ic, Element::class_name, id(), std::move(inertia_provision().cost))
{
}

bool InertiaProvisionLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index_);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  try {
    auto&& generation_cols = generator_lp.generation_cols_at(scenario, stage);
    const auto& blocks = stage.blocks();

    // Compute provision_factor: explicit schedule > H*S/Pmin > 1.0
    const auto stage_pf = provision_factor_.optval(stage.uid());
    const auto& ip = inertia_provision();

    const auto st_key = std::tuple {scenario.uid(), stage.uid()};
    BIndexHolder<ColIndex> pcols;
    BIndexHolder<RowIndex> crows;
    map_reserve(pcols, blocks.size());
    map_reserve(crows, blocks.size());

    for (const auto& block : blocks) {
      const auto buid = block.uid();

      const auto gcol_it = generation_cols.find(buid);
      if (gcol_it == generation_cols.end()) {
        continue;
      }
      const auto gcol = gcol_it->second;

      const auto gen_pmin = lp.get_col_lowb(gcol);
      const auto gen_pmax = lp.get_col_uppb(gcol);

      // Resolve provision_factor for this block
      double pf_value = 1.0;
      if (stage_pf) {
        pf_value = stage_pf.value();
      } else if (ip.inertia_constant && ip.rated_power) {
        const auto pmin_eff =
            (gen_pmin > 0.0) ? gen_pmin : gen_pmax;  // fallback
        if (pmin_eff > 0.0) {
          pf_value =
              ip.inertia_constant.value() * ip.rated_power.value() / pmin_eff;
        }
      }

      if (pf_value <= 0.0) {
        continue;
      }

      // Resolve provision_max: schedule > gen_pmin > gen_pmax
      auto block_pmax = provision_max_.optval(stage.uid(), buid)
                            .value_or(gen_pmin > 0.0 ? gen_pmin : gen_pmax);

      const auto stage_cost = cost_.optval(stage.uid()).value_or(0.0);

      // Create provision variable r_inertia
      const auto pcol = lp.add_col({
          .uppb = block_pmax,
          .cost = CostHelper::block_ecost(scenario, stage, block, stage_cost),
          .class_name = cname,
          .variable_name = ProvisionName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      pcols[buid] = pcol;

      // Downward coupling: p - r_inertia >= 0
      auto crow =
          SparseRow {
              .class_name = cname,
              .constraint_name = CouplingName,
              .variable_uid = uid(),
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .greater_equal(0.0);
      crow[gcol] = 1.0;
      crow[pcol] = -1.0;
      crows[buid] = lp.add_row(std::move(crow));

      // Inject provision_factor * r_inertia into each zone's requirement row
      for (auto&& izone_index : inertia_zone_indexes_) {
        auto&& izone = sc.element(izone_index);
        if (!izone.is_active(stage)) {
          continue;
        }

        const auto& req_rows = izone.requirement_rows();
        const auto req_rows_it = req_rows.find(st_key);
        if (req_rows_it == req_rows.end() || req_rows_it->second.empty()) {
          continue;
        }
        const auto req_row_it = req_rows_it->second.find(buid);
        if (req_row_it != req_rows_it->second.end()) {
          lp.set_coeff(req_row_it->second, pcol, pf_value);
        }
      }
    }

    // Store index holders
    if (!pcols.empty()) {
      provision_cols_[st_key] = std::move(pcols);
    }
    if (!crows.empty()) {
      coupling_rows_[st_key] = std::move(crows);
    }

    // Register PAMPL-visible provision columns
    if (const auto it = provision_cols_.find(st_key);
        it != provision_cols_.end() && !it->second.empty())
    {
      sc.add_ampl_variable(
          ampl_name, uid(), ProvName, scenario, stage, it->second);
    }
  } catch (const std::exception& e) {
    SPDLOG_ERROR("InertiaProvisionLP::add_to_lp exception for uid={}: {}",
                 uid(),
                 e.what());
    return false;
  }

  return true;
}

bool InertiaProvisionLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, ProvisionName, pid, provision_cols_);
  out.add_col_cost(cname, ProvisionName, pid, provision_cols_);
  out.add_row_dual(cname, CouplingName, pid, coupling_rows_);

  return true;
}

}  // namespace gtopt
