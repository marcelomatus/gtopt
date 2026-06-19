#include <expected>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <gtopt/error.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <spdlog/spdlog.h>

namespace
{

using namespace gtopt;

/// Build the provision column + coupling rows for a single direction
/// (up or down) and inject the `stage_provision_factor × prov_col`
/// coefficient into **every** active reserve zone's requirement row.
///
/// The previous design called this helper **once per zone** and let
/// each call create its own `prov_col` for the same (provision, block).
/// That worked only when the provision referenced a single zone; with
/// two or more zones it tripped `LinearProblem::add_col`'s duplicate-
/// metadata check (class + variable + uid identical across calls) and
/// silently dropped every zone past the first.  The refactor mirrors
/// `InertiaProvisionLP::add_to_lp`: one column per (provision, block)
/// created outside the zone loop, then per-zone coefficient injection
/// inside.
std::expected<void, Error> add_provision(
    const std::string_view cname,
    const ScenarioLP& scenario,
    const StageLP& stage,
    LinearProblem& lp,
    const std::vector<BlockLP>& blocks,
    auto capacity_col,
    const BIndexHolder<ColIndex>& generation_cols,
    const Uid uid,
    ReserveProvisionLP::Provision& rp,
    const std::string_view pname,
    const std::string_view cap_name,
    std::span<const ElementIndex<ReserveZoneLP>> reserve_zone_indexes,
    const SystemContext& sc,
    auto get_requirement_rows,
    auto provision_row)
{
  // `provision_factor` / `cost` / `capacity_factor` are now
  // per-(stage, block).  An *empty* provision_factor at every block of
  // the stage still means "no provision wired"; we short-circuit when
  // none of the blocks have a positive factor.  Per-block resolution
  // happens inside the block loop below.
  if (!rp.provision_factor.has_value()) {
    return {};
  }
  const auto use_capacity_field =
      capacity_col && rp.capacity_factor.has_value();

  const auto st_k = std::tuple {scenario.uid(), stage.uid()};

  // Pre-filter: collect per-(s,t) requirement-row maps for every
  // **active** zone that has at least one row entry for this
  // (scenario, stage).  If no zone qualifies, skip creating any
  // cols/rows for this provision — preserves the early-out from
  // the previous per-zone helper.
  std::vector<const BIndexHolder<RowIndex>*> req_row_maps;
  req_row_maps.reserve(reserve_zone_indexes.size());
  for (auto&& rzi : reserve_zone_indexes) {
    auto&& rz = sc.element(rzi);
    if (!rz.is_active(stage)) {
      continue;
    }
    const auto& requirement_rows = get_requirement_rows(rz);
    const auto rows_it = requirement_rows.find(st_k);
    if (rows_it != requirement_rows.end() && !rows_it->second.empty()) {
      req_row_maps.push_back(&rows_it->second);
    }
  }
  if (req_row_maps.empty()) {
    return {};
  }

  auto& prov_cols = rp.provision_cols[st_k];
  auto& prov_rows = rp.provision_rows[st_k];
  auto& cap_rows = rp.capacity_rows[st_k];
  map_reserve(prov_cols, blocks.size());
  map_reserve(prov_rows, blocks.size());
  map_reserve(cap_rows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();

    // Resolve the per-(stage, block) provision factor.  When the
    // provision_factor cell at this block is unset OR ≤ 0, the
    // provision is inactive for the block — skip it (preserves the
    // legacy per-stage short-circuit one block at a time).
    const auto block_provision_factor =
        rp.provision_factor.optval(stage.uid(), buid);
    if (!block_provision_factor || (block_provision_factor.value() <= 0.0)) {
      continue;
    }

    const auto block_cost = rp.cost.optval(stage.uid(), buid).value_or(0.0);
    const auto block_capacity_factor =
        rp.capacity_factor.optval(stage.uid(), buid);
    const bool use_capacity = use_capacity_field && block_capacity_factor;

    //
    // create the provision col and row when needed and if possible, i.e.,
    // if there is a rmax provision defined for the stage and block
    //
    const auto gcol_it = generation_cols.find(buid);
    if (gcol_it == generation_cols.end()) {
      continue;
    }
    const auto gcol = gcol_it->second;
    auto block_rmax = rp.max.optval(stage.uid(), buid);
    if (!block_rmax) {
      // No explicit provision max for this (stage, block).  A wired
      // provision (it reached here past the positive-provision_factor
      // gate above, and its generator HAS a generation column — the
      // `gcol_it == end()` guard above already skipped the no-gcol case)
      // can offer up to the generator's own headroom, so fall back to the
      // gcol upper bound regardless of whether a separate
      // `capacity_factor` row is wired.  The previous `else continue`
      // SILENTLY DROPPED the provision column when `max` was unset and no
      // `capacity_factor` was present — even though the generator has
      // capacity (e.g. a BESS with `ur_provision_factor` but no explicit
      // `urmax`).  That dropped column is referenced by reserve-zone
      // requirements and UserConstraints (`csf_rs_*`), so silently
      // removing it rewrote those constraints and made the LP infeasible
      // (PLEXOS 20260517: `csf_rs_min_bat_del_desierto` lost its
      // `uprovision` term → forced a committed unit OFF → infeasible).
      block_rmax = lp.get_col_uppb(gcol);
    }
    // PLEXOS Min Provision floor — when set, clamp the provision col's
    // lower bound to the per-block value.  This seeds the floor as an
    // unconditional ``provision ≥ urmin`` (the col ``lowb``).  For a
    // generator that HAS a commitment, ``CommitmentLP::add_to_lp`` then
    // rewrites it into the commitment-CONDITIONAL linkage row
    // ``provision − urmin·u ≥ 0`` and resets ``lowb = 0`` (see
    // commitment_lp.cpp "PLEXOS Min Provision linkage"), so the floor
    // only binds when the unit is committed (``u = 1``) — matching
    // PLEXOS's "Min Provision gated by Available Units" semantic.  The
    // plexos2gtopt converter therefore emits ``urmin``/``drmin`` ONLY
    // for committed generators; for a generator with no commitment the
    // seed below would stay an always-on floor and could make the LP
    // infeasible when the unit is off / at full output.
    const auto block_rmin = rp.min.optval(stage.uid(), buid).value_or(0.0);

    // LP-size: when both bounds collapse to zero the provision column is
    // fixed at 0 — the capacity row ``cap_factor · cap − prov ≥ 0``
    // reduces to ``cap ≥ 0`` (always true), the provision-bound row is
    // already implied by the generation column's own bounds, and the
    // ``pf · prov`` coefficient stamped into every zone requirement row
    // is identically 0.  Skip the fixed-zero column and its rows.
    // Write-out rule: an absent provision column reads 0 (no reserve
    // provided this block).
    if (sc.options().lp_reduction() && block_rmax.value() == 0.0
        && block_rmin == 0.0)
    {
      continue;
    }

    const auto prov_col = lp.add_col({
        .lowb = block_rmin,
        .uppb = block_rmax.value(),
        .cost = CostHelper::block_ecost(scenario, stage, block, block_cost),
        .class_name = cname,
        .variable_name = pname,
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });

    prov_cols[buid] = prov_col;

    if (use_capacity) {
      auto crow =
          SparseRow {
              .class_name = cname,
              .constraint_name = cap_name,
              .variable_uid = uid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .greater_equal(0);
      crow[capacity_col.value()] = block_capacity_factor.value();
      crow[prov_col] = -1;
      cap_rows[buid] = lp.add_row(std::move(crow));
    }

    prov_rows[buid] = lp.add_row(provision_row(
        gcol,
        prov_col,
        make_block_context(scenario.uid(), stage.uid(), block.uid())));

    //
    // Inject the provision coefficient into every active zone's
    // requirement-balance row for this block.
    //
    for (const auto* req_rows_ptr : req_row_maps) {
      const auto req_row_it = req_rows_ptr->find(buid);
      if (req_row_it != req_rows_ptr->end()) {
        lp.set_coeff(
            req_row_it->second, prov_col, block_provision_factor.value());
      }
    }
  }

  return {};
}

auto make_rzone_indexes(const InputContext& ic,
                        std::span<const SingleId> rzones)
{
  // Typed-array form post-2026-05-16: each `SingleId` in `rzones` is
  // resolved directly via `ic.element_index`.  No string splitting,
  // no Uid-vs-name discriminator, no per-element heap allocation.
  // Replaces the colon/comma-delimited `String` parser that lived
  // here previously (`split` + `is_uid` + `str2uid` +
  // `RZoneId{std::string(...)}` materialisation per zone).
  using RZoneId = ObjectSingleId<ReserveZoneLP>;
  return std::ranges::to<std::vector>(
      rzones
      | std::views::transform([&](const SingleId& rz)
                              { return ic.element_index(RZoneId {rz}); }));
}

}  // namespace

namespace gtopt
{

ReserveProvisionLP::Provision::Provision(const InputContext& ic,
                                         std::string_view cname,
                                         const Id& id,
                                         auto&& rmax,
                                         auto&& rmin,
                                         auto&& rcost,
                                         auto&& rcapf,
                                         auto&& rprof)
    : max(ic, cname, id, std::forward<decltype(rmax)>(rmax))
    , min(ic, cname, id, std::forward<decltype(rmin)>(rmin))
    , cost(ic, cname, id, std::forward<decltype(rcost)>(rcost))
    , capacity_factor(ic, cname, id, std::forward<decltype(rcapf)>(rcapf))
    , provision_factor(ic, cname, id, std::forward<decltype(rprof)>(rprof))
{
}

ReserveProvisionLP::ReserveProvisionLP(
    const ReserveProvision& preserve_provision, const InputContext& ic)
    : Base(preserve_provision)
    , up(ic,
         Element::class_name,
         id(),
         std::move(reserve_provision().urmax),
         std::move(reserve_provision().urmin),
         std::move(reserve_provision().urcost),
         std::move(reserve_provision().ur_capacity_factor),
         std::move(reserve_provision().ur_provision_factor))
    , dp(ic,
         Element::class_name,
         id(),
         std::move(reserve_provision().drmax),
         std::move(reserve_provision().drmin),
         std::move(reserve_provision().drcost),
         std::move(reserve_provision().dr_capacity_factor),
         std::move(reserve_provision().dr_provision_factor))
    , generator_index(ic.element_index(generator_sid()))
    , reserve_zone_indexes(
          make_rzone_indexes(ic, reserve_provision().reserve_zones))
{
}

bool ReserveProvisionLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  try {
    // Use the tolerant lookup variant: when every block of the
    // (scenario, stage) was elided by the P1 zero-pmax optimization
    // (gen column never created — pmax=0 hours), the outer key in
    // ``generation_cols`` is absent.  ``generation_cols_at`` would
    // throw ``flat_map::at`` in that case (verified 2026-05-22 on the
    // CEN PCP daily bundle when ``--use-plexos-gen-cap`` forced
    // hydro generators with zero PLEXOS dispatch to pmax_profile=0
    // every block).  ``lookup_generation_cols`` returns an empty
    // BIndexHolder via ``find_or_empty_inner``, so the per-block
    // ``generation_cols.find(buid)`` checks below safely iterate
    // zero times and the reserve constraints simply aren't emitted
    // for this generator at this (scenario, stage).  This is the
    // correct behaviour: with no dispatchable capacity available,
    // there is no reserve capability to provide either.
    auto&& generation_cols =
        generator_lp.lookup_generation_cols(scenario, stage);

    const auto [opt_capacity, capacity_col] =
        generator_lp.capacity_and_col(stage, lp);

    auto uprov_row = [&](auto gcol, auto rcol, auto context)
    {
      auto rrow =
          SparseRow {
              .class_name = cname,
              .constraint_name = ReserveProvisionLP::UprovisionName,
              .variable_uid = uid(),
              .context = std::move(context),
          }
              .less_equal(lp.get_col_uppb(gcol));
      rrow[gcol] = 1;
      rrow[rcol] = 1;

      if (capacity_col) {
        rrow[*capacity_col] = -1;
        return rrow.less_equal(0.0);
      }
      return rrow;
    };

    auto dprov_row = [&](auto gcol, auto rcol, auto context)
    {
      auto rrow =
          SparseRow {
              .class_name = cname,
              .constraint_name = ReserveProvisionLP::DprovisionName,
              .variable_uid = uid(),
              .context = std::move(context),
          }
              .greater_equal(lp.get_col_lowb(gcol));
      rrow[gcol] = 1;
      rrow[rcol] = -1;
      return rrow;
    };

    const auto& blocks = stage.blocks();

    // `add_provision` now consumes the full zone list and creates one
    // column per (provision, block) — the per-zone work is folded
    // inside its block loop.  See the docstring above for why the
    // outer zone loop disappeared.
    if (auto res = add_provision(
            cname,
            scenario,
            stage,
            lp,
            blocks,
            capacity_col,
            generation_cols,
            uid(),
            up,
            UprovisionName,
            UcapacityName,
            std::span<const ElementIndex<ReserveZoneLP>>(reserve_zone_indexes),
            sc,
            [](const auto& rz) -> const auto&
            { return rz.urequirement_rows(); },
            uprov_row);
        !res)
    {
      SPDLOG_WARN("add_provision (uprov) failed for uid={}: {}",
                  uid(),
                  res.error().message);
      return false;
    }
    if (auto res = add_provision(
            cname,
            scenario,
            stage,
            lp,
            blocks,
            capacity_col,
            generation_cols,
            uid(),
            dp,
            DprovisionName,
            DcapacityName,
            std::span<const ElementIndex<ReserveZoneLP>>(reserve_zone_indexes),
            sc,
            [](const auto& rz) -> const auto&
            { return rz.drequirement_rows(); },
            dprov_row);
        !res)
    {
      SPDLOG_WARN("add_provision (dprov) failed for uid={}: {}",
                  uid(),
                  res.error().message);
      return false;
    }
  } catch (const std::exception& e) {
    SPDLOG_ERROR("ReserveProvisionLP::add_to_lp exception for uid={}: {}",
                 uid(),
                 e.what());
    return false;
  }

  // Register PAMPL-visible provision columns under the canonical
  // `up` / `dn` spelling.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  if (const auto it = up.provision_cols.find(st_key);
      it != up.provision_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(ampl_name, uid(), UpName, scenario, stage, it->second);
  }
  if (const auto it = dp.provision_cols.find(st_key);
      it != dp.provision_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(ampl_name, uid(), DnName, scenario, stage, it->second);
  }

  return true;
}

bool ReserveProvisionLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, UprovisionName, pid, up.provision_cols);
  out.add_col_cost(cname, UprovisionName, pid, up.provision_cols);
  out.add_row_dual(cname, UprovisionName, pid, up.provision_rows);
  out.add_row_dual(cname, UcapacityName, pid, up.capacity_rows);

  out.add_col_sol(cname, DprovisionName, pid, dp.provision_cols);
  out.add_col_cost(cname, DprovisionName, pid, dp.provision_cols);
  out.add_row_dual(cname, DprovisionName, pid, dp.provision_rows);
  out.add_row_dual(cname, DcapacityName, pid, dp.capacity_rows);

  return true;
}

}  // namespace gtopt
