#include <algorithm>
#include <format>
#include <ranges>
#include <string>

#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

LineLP::LineLP(const Line& pline, const InputContext& ic)
    : CapacityBase(pline, ic, ClassName)
    , tmax_ba(ic, ClassName, id(), std::move(line().tmax_ba))
    , tmax_ab(ic, ClassName, id(), std::move(line().tmax_ab))
    , tcost(ic, ClassName, id(), std::move(line().tcost))
    , lossfactor(ic, ClassName, id(), std::move(line().lossfactor))
    , reactance(ic, ClassName, id(), std::move(line().reactance))
    , voltage(ic, ClassName, id(), std::move(line().voltage))
    , resistance(ic, ClassName, id(), std::move(line().resistance))
{
  SPDLOG_DEBUG("LineLP created: uid={} name='{}'", id().first, id().second);
}

// ── compute_loss_params ─────────────────────────────────────────────

LineLP::LossParams LineLP::compute_loss_params(const SystemContext& sc,
                                               const StageLP& stage) const
{
  const auto lf = sc.stage_lossfactor(stage, lossfactor);
  const bool has_lin = lf > 0.0;

  const auto R = resistance.at(stage.uid()).value_or(0.0);
  const auto V = voltage.at(stage.uid()).value_or(0.0);
  const int nseg =
      std::max(1, line().loss_segments.value_or(sc.options().loss_segments()));

  // Quadratic model: resistance > 0, voltage > 0, nseg > 1,
  // use_line_losses enabled, and no explicit lossfactor.
  const bool has_quad = !has_lin && sc.options().use_line_losses() && R > 0.0
      && V > 0.0 && nseg > 1;

  return {
      .lossfactor = lf,
      .resistance = R,
      .V2 = V * V,
      .nseg = nseg,
      .has_linear_loss = has_lin,
      .has_quadratic_loss = has_quad,
      .has_loss = has_lin || has_quad,
  };
}

// ── add_quadratic_flow_direction ────────────────────────────────────
//
// Piecewise-linear approximation of P_loss = R · f² / V².
// Divide [0, tmax] into nseg segments.  Segment k (1-based) has:
//   width      = tmax / nseg
//   loss_coeff = width · R · (2k−1) / V²
//
// Variables created per call:
//   f_total   ∈ [0, tmax]       — total flow (Kirchhoff, capacity, output)
//   f_seg_k   ∈ [0, width]      — segment variables × nseg
//   loss      ∈ [0, +∞)         — total power lost
// Constraints:
//   f_total   = Σ f_seg_k        (linking)
//   loss      = Σ loss_k·f_seg_k (loss tracking)
// Bus balance:
//   sending:   −f_total − loss   (power leaves)
//   receiving: +f_total          (power arrives)
//
// Units: R [Ω], f [MW], V [kV] → P_loss [MW].

LineLP::DirectionResult LineLP::add_quadratic_flow_direction(
    SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    LinearProblem& lp,
    SparseRow& sending_brow,
    SparseRow& receiving_brow,
    double block_tmax,
    double block_tcost,
    const LossParams& loss,
    std::optional<ColIndex> capacity_col,
    std::string_view dir)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (block_tmax <= 0.0) {
    return {};
  }

  const int nseg = loss.nseg;
  const double seg_width = block_tmax / nseg;

  // Total flow variable (for Kirchhoff, capacity, and output)
  const auto flow_col = lp.add_col({
      .name = sc.lp_label(
          scenario, stage, block, cname, std::format("f{}", dir), uid()),
      .lowb = 0,
      .uppb = block_tmax,
      .cost = block_tcost,
  });

  // Receiving bus sees total flow (no loss subtracted)
  receiving_brow[flow_col] = +1;

  // Linking: f_total − Σ f_seg_k = 0
  auto linkrow =
      SparseRow {
          .name = sc.lp_label(
              scenario, stage, block, cname, std::format("lnk{}", dir), uid()),
      }
          .equal(0);
  linkrow.reserve(nseg + 1);
  linkrow[flow_col] = +1.0;

  // Loss variable: tracks total power lost
  const auto loss_col = lp.add_col({
      .name = sc.lp_label(
          scenario, stage, block, cname, std::format("ls{}", dir), uid()),
      .lowb = 0,
      .uppb = CoinDblMax,
  });

  // Loss linking: loss − Σ loss_k · f_seg_k = 0
  auto lossrow =
      SparseRow {
          .name = sc.lp_label(
              scenario, stage, block, cname, std::format("lsl{}", dir), uid()),
      }
          .equal(0);
  lossrow.reserve(nseg + 1);
  lossrow[loss_col] = +1.0;

  // Power leaving sending bus = f_total + loss (both negative in the
  // balance equation: generation − outgoing power = 0).
  sending_brow[flow_col] = -1.0;
  sending_brow[loss_col] = -1.0;

  // Add segment variables with increasing loss coefficients
  for (const auto k : std::views::iota(1, nseg + 1)) {
    const double loss_k = seg_width * loss.resistance * (2 * k - 1) / loss.V2;

    const auto seg_col = lp.add_col({
        .name =
            sc.lp_label(
                scenario, stage, block, cname, std::format("f{}s", dir), uid())
            + std::to_string(k),
        .lowb = 0,
        .uppb = seg_width,
    });

    linkrow[seg_col] = -1.0;
    lossrow[seg_col] = -loss_k;
  }

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  // Capacity constraint on total flow
  std::optional<RowIndex> cap_row;
  if (capacity_col) {
    auto cprow =
        SparseRow {
            .name = sc.lp_label(scenario,
                                stage,
                                block,
                                cname,
                                std::format("cap{}", dir),
                                uid()),
        }
            .greater_equal(0);
    cprow[*capacity_col] = 1;
    cprow[flow_col] = -1;
    cap_row = lp.add_row(std::move(cprow));
  }

  return {.flow_col = flow_col, .loss_col = loss_col, .capacity_row = cap_row};
}

// ── add_kirchhoff_rows ──────────────────────────────────────────────

void LineLP::add_kirchhoff_rows(SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp,
                                const BusLP& bus_a_lp,
                                const BusLP& bus_b_lp,
                                const BIndexHolder<ColIndex>& fpcols,
                                const BIndexHolder<ColIndex>& fncols)
{
  static constexpr std::string_view cname = ClassName.short_name();

  const auto& stage_reactance = sc.stage_reactance(stage, reactance);
  if (!stage_reactance) {
    return;
  }

  const auto& blocks = stage.blocks();
  const auto& theta_a_cols =
      bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
  const auto& theta_b_cols =
      bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

  if (theta_a_cols.empty() || theta_b_cols.empty()) {
    return;
  }

  const double scale_theta = sc.options().scale_theta();
  const double X = stage_reactance.value();
  const double V = voltage.at(stage.uid()).value_or(1);
  const double x = scale_theta * (X / (V * V));

  BIndexHolder<RowIndex> trows;
  map_reserve(trows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto trow =
        SparseRow {
            .name = sc.lp_label(scenario, stage, block, cname, "theta", uid()),
        }
            .equal(0);

    trow.reserve(4);

    trow[theta_a_cols.at(buid)] = -1.0;
    trow[theta_b_cols.at(buid)] = +1.0;
    if (!fpcols.empty()) {
      trow[fpcols.at(buid)] = +x;
    }
    if (!fncols.empty()) {
      trow[fncols.at(buid)] = -x;
    }

    trows[buid] = lp.add_row(std::move(trow));
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  theta_rows[st_key] = std::move(trows);
}

// ── add_to_lp ───────────────────────────────────────────────────────

bool LineLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (is_loop()) {
    return true;
  }

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus_a_lp = sc.element<BusLP>(bus_a_sid());
  const auto& bus_b_lp = sc.element<BusLP>(bus_b_sid());
  if (!bus_a_lp.is_active(stage) || !bus_b_lp.is_active(stage)) {
    return true;
  }

  const auto& balance_rows_a = bus_a_lp.balance_rows_at(scenario, stage);
  const auto& balance_rows_b = bus_b_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);
  const auto stage_tcost = tcost.at(stage.uid()).value_or(0.0);
  const auto loss = compute_loss_params(sc, stage);

  BIndexHolder<ColIndex> fpcols;
  BIndexHolder<RowIndex> cprows;
  BIndexHolder<ColIndex> fncols;
  BIndexHolder<RowIndex> cnrows;
  BIndexHolder<ColIndex> lpcols;
  BIndexHolder<ColIndex> lncols;
  map_reserve(fpcols, blocks.size());
  map_reserve(cprows, blocks.size());
  map_reserve(fncols, blocks.size());
  map_reserve(cnrows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto& brow_a = lp.row_at(balance_rows_a.at(buid));
    auto& brow_b = lp.row_at(balance_rows_b.at(buid));

    const auto [block_tmax_ab, block_tmax_ba] = sc.block_maxmin_at(
        stage, block, tmax_ab, tmax_ba, stage_capacity, -stage_capacity);
    const auto block_tcost =
        sc.block_ecost(scenario, stage, block, stage_tcost);

    if (loss.has_quadratic_loss) {
      // ── Quadratic model: A→B direction ────────────────────────────
      auto [fp, lp_col, cp] = add_quadratic_flow_direction(sc,
                                                           scenario,
                                                           stage,
                                                           block,
                                                           lp,
                                                           brow_a,
                                                           brow_b,
                                                           block_tmax_ab,
                                                           block_tcost,
                                                           loss,
                                                           capacity_col,
                                                           "p");

      if (fp) {
        fpcols[buid] = *fp;
      }
      if (lp_col) {
        lpcols[buid] = *lp_col;
      }
      if (cp) {
        cprows[buid] = *cp;
      }

      // ── Quadratic model: B→A direction ────────────────────────────
      auto [fn, ln_col, cn] = add_quadratic_flow_direction(sc,
                                                           scenario,
                                                           stage,
                                                           block,
                                                           lp,
                                                           brow_b,
                                                           brow_a,
                                                           block_tmax_ba,
                                                           block_tcost,
                                                           loss,
                                                           capacity_col,
                                                           "n");

      if (fn) {
        fncols[buid] = *fn;
      }
      if (ln_col) {
        lncols[buid] = *ln_col;
      }
      if (cn) {
        cnrows[buid] = *cn;
      }

    } else {
      // ── Linear loss model ─────────────────────────────────────────

      if (!loss.has_loss || block_tmax_ab > 0.0) {
        const auto fpc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "fp", uid()),
            .lowb = loss.has_loss ? 0.0 : -block_tmax_ba,
            .uppb = block_tmax_ab,
            .cost = block_tcost,
        });
        fpcols[buid] = fpc;

        brow_a[fpc] = -(1 + loss.lossfactor);
        brow_b[fpc] = +1;

        if (capacity_col) {
          auto cprow =
              SparseRow {
                  .name =
                      sc.lp_label(scenario, stage, block, cname, "capp", uid()),
              }
                  .greater_equal(0);
          cprow[*capacity_col] = 1;
          cprow[fpc] = -1;
          cprows[buid] = lp.add_row(std::move(cprow));
        }
      }

      if (loss.has_loss && block_tmax_ba > 0.0) {
        const auto fnc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "fn", uid()),
            .lowb = 0,
            .uppb = block_tmax_ba,
            .cost = block_tcost,
        });
        fncols[buid] = fnc;

        brow_b[fnc] = -(1 + loss.lossfactor);
        brow_a[fnc] = +1;

        if (capacity_col) {
          auto cnrow =
              SparseRow {
                  .name =
                      sc.lp_label(scenario, stage, block, cname, "capn", uid()),
              }
                  .greater_equal(0);
          cnrow[*capacity_col] = 1;
          cnrow[fnc] = -1;
          cnrows[buid] = lp.add_row(std::move(cnrow));
        }
      }
    }
  }

  // ── Kirchhoff (DC OPF) constraints ────────────────────────────────
  add_kirchhoff_rows(
      sc, scenario, stage, lp, bus_a_lp, bus_b_lp, fpcols, fncols);

  // Store all indices for this (scenario, stage)
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);
  lossp_cols[st_key] = std::move(lpcols);
  lossn_cols[st_key] = std::move(lncols);

  return true;
}

// ── add_to_output ───────────────────────────────────────────────────

bool LineLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  if (is_loop()) {
    return true;
  }

  const auto pid = id();

  out.add_col_sol(cname, "flowp", pid, flowp_cols);
  out.add_col_cost(cname, "flowp", pid, flowp_cols);

  out.add_col_sol(cname, "flown", pid, flown_cols);
  out.add_col_cost(cname, "flown", pid, flown_cols);

  out.add_col_sol(cname, "lossp", pid, lossp_cols);
  out.add_col_sol(cname, "lossn", pid, lossn_cols);

  out.add_row_dual(cname, "capacityp", pid, capacityp_rows);
  out.add_row_dual(cname, "capacityn", pid, capacityn_rows);

  out.add_row_dual(cname, "theta", pid, theta_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
