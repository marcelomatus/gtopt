#include <algorithm>
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
  const auto stage_lossfactor = sc.stage_lossfactor(stage, lossfactor);
  const auto has_linear_loss = stage_lossfactor > 0.0;

  // Quadratic loss model: activated when resistance > 0, voltage > 0,
  // no explicit lossfactor, and loss_segments > 1.  Uses a piecewise-linear
  // approximation of P_loss = R · f² / V² following the PLP/FESOP
  // methodology (genpdlin.f).
  //
  // Units: R [Ω], f [MW], V [kV].  The formula yields P_loss in MW
  // because (Ω · MW²) / kV² = MW by dimensional analysis.
  const auto stage_resistance = resistance.at(stage.uid()).value_or(0.0);
  const auto stage_voltage = voltage.at(stage.uid()).value_or(0.0);
  const int nseg =
      std::max(1, line().loss_segments.value_or(sc.options().loss_segments()));
  const bool has_quadratic_loss = !has_linear_loss
      && sc.options().use_line_losses() && stage_resistance > 0.0
      && stage_voltage > 0.0 && nseg > 1;

  const auto has_loss = has_linear_loss || has_quadratic_loss;

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
    const auto balance_row_a = balance_rows_a.at(buid);
    const auto balance_row_b = balance_rows_b.at(buid);

    const auto [block_tmax_ab, block_tmax_ba] = sc.block_maxmin_at(
        stage, block, tmax_ab, tmax_ba, stage_capacity, -stage_capacity);

    const auto block_tcost =
        sc.block_ecost(scenario, stage, block, stage_tcost);

    auto& brow_a = lp.row_at(balance_row_a);
    auto& brow_b = lp.row_at(balance_row_b);

    if (has_quadratic_loss) {
      // ── Quadratic (piecewise-linear) loss model ──────────────────────
      //
      // Divide the flow range [0, tmax] into nseg equal segments.
      // Segment k (1-based) has:
      //   width      = tmax / nseg
      //   loss_coeff = width · R · (2k−1) / V²
      //
      // Each segment has its own flow variable fp_seg_k ∈ [0, width]
      // that enters the bus balance with coefficient:
      //   bus_a:  -(1 + loss_coeff_k)   (sending end, includes loss)
      //   bus_b:  +1                     (receiving end)
      //
      // A "total flow" variable fp_total is linked by a constraint:
      //   fp_total = sum(fp_seg_k)
      // and participates in the Kirchhoff and capacity constraints.
      //
      // A "loss" variable is added that equals the total power lost:
      //   loss_p = sum(loss_coeff_k · fp_seg_k)
      //
      // This follows the PLP/FESOP piecewise-linear methodology from
      // genpdlin.f (CEN65/src/).

      const double V2 = stage_voltage * stage_voltage;

      // ── Positive (A→B) direction ──────────────────────────────────
      if (block_tmax_ab > 0.0) {
        const double seg_width_ab = block_tmax_ab / nseg;

        // Total flow variable (for Kirchhoff, capacity, and output)
        const auto fpc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "fp", uid()),
            .lowb = 0,
            .uppb = block_tmax_ab,
            .cost = block_tcost,
        });
        fpcols[buid] = fpc;

        // Receiving bus sees total flow (no loss subtracted)
        brow_b[fpc] = +1;

        // Linking constraint: fp_total - sum(fp_seg_k) = 0
        auto linkrow_p =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "lnkp", uid()),
            }
                .equal(0);
        linkrow_p.reserve(nseg + 1);
        linkrow_p[fpc] = +1.0;

        // Loss variable: tracks total power lost for output
        const auto lpc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "lsp", uid()),
            .lowb = 0,
            .uppb = CoinDblMax,
        });
        lpcols[buid] = lpc;

        // Loss linking: loss_p - sum(loss_k · fp_seg_k) = 0
        auto lossrow_p =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "lslp", uid()),
            }
                .equal(0);
        lossrow_p.reserve(nseg + 1);
        lossrow_p[lpc] = +1.0;

        // Power leaving bus_a = fp_total + loss_p (both negative in
        // the balance: generation minus outgoing power = 0).
        brow_a[fpc] = -1.0;
        brow_a[lpc] = -1.0;

        for (int k = 1; k <= nseg; ++k) {
          const double loss_k =
              seg_width_ab * stage_resistance * (2 * k - 1) / V2;

          const auto seg_col = lp.add_col({
              .name = sc.lp_label(scenario, stage, block, cname, "fps", uid())
                  + std::to_string(k),
              .lowb = 0,
              .uppb = seg_width_ab,
          });

          linkrow_p[seg_col] = -1.0;
          lossrow_p[seg_col] = -loss_k;
        }

        [[maybe_unused]] auto linkp_idx = lp.add_row(std::move(linkrow_p));
        [[maybe_unused]] auto lossp_idx = lp.add_row(std::move(lossrow_p));
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

      // ── Negative (B→A) direction ──────────────────────────────────
      if (block_tmax_ba > 0.0) {
        const double seg_width_ba = block_tmax_ba / nseg;

        const auto fnc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "fn", uid()),
            .lowb = 0,
            .uppb = block_tmax_ba,
            .cost = block_tcost,
        });
        fncols[buid] = fnc;

        // Receiving bus (bus_a) sees total flow
        brow_a[fnc] = +1;

        auto linkrow_n =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "lnkn", uid()),
            }
                .equal(0);
        linkrow_n.reserve(nseg + 1);
        linkrow_n[fnc] = +1.0;

        const auto lnc = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "lsn", uid()),
            .lowb = 0,
            .uppb = CoinDblMax,
        });
        lncols[buid] = lnc;

        auto lossrow_n =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "lsln", uid()),
            }
                .equal(0);
        lossrow_n.reserve(nseg + 1);
        lossrow_n[lnc] = +1.0;

        // Sending bus (bus_b) sees total flow + total loss
        brow_b[fnc] = -1.0;
        brow_b[lnc] = -1.0;

        for (int k = 1; k <= nseg; ++k) {
          const double loss_k =
              seg_width_ba * stage_resistance * (2 * k - 1) / V2;

          const auto seg_col = lp.add_col({
              .name = sc.lp_label(scenario, stage, block, cname, "fns", uid())
                  + std::to_string(k),
              .lowb = 0,
              .uppb = seg_width_ba,
          });

          linkrow_n[seg_col] = -1.0;
          lossrow_n[seg_col] = -loss_k;
        }

        [[maybe_unused]] auto linkn_idx = lp.add_row(std::move(linkrow_n));
        [[maybe_unused]] auto lossn_idx = lp.add_row(std::move(lossrow_n));

        if (capacity_col) {
          SparseRow cnrow {
              .name = sc.lp_label(scenario, stage, block, cname, "capn", uid()),
          };
          cnrow[capacity_col.value()] = 1;
          cnrow[fnc] = -1;
          cnrows[buid] = lp.add_row(std::move(cnrow.greater_equal(0)));
        }
      }

    } else {
      // ── Linear loss model (existing) ─────────────────────────────────
      if (!has_loss || block_tmax_ab > 0.0) {
        //  adding flowp variable

        const auto fpc = lp.add_col({
            // flow variable
            .name = sc.lp_label(scenario, stage, block, cname, "fp", uid()),
            .lowb = has_loss ? 0 : -block_tmax_ba,
            .uppb = block_tmax_ab,
            .cost = block_tcost,
        });
        fpcols[buid] = fpc;

        // adding flowp to the bus balances
        brow_a[fpc] = -(1 + stage_lossfactor);
        brow_b[fpc] = +1;

        // adding the capacity constraint
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

      if (has_loss && block_tmax_ba > 0.0) {
        //  adding flown variable

        const auto fnc = lp.add_col({
            // flow variable
            .name = sc.lp_label(scenario, stage, block, cname, "fn", uid()),
            .lowb = 0,
            .uppb = block_tmax_ba,
            .cost = block_tcost,
        });

        fncols[buid] = fnc;

        // adding flown to the bus balances
        brow_b[fnc] = -(1 + stage_lossfactor);
        brow_a[fnc] = +1;

        // adding the capacity constraint
        if (capacity_col) {
          SparseRow cnrow {
              .name = sc.lp_label(scenario, stage, block, cname, "capn", uid()),
          };
          cnrow[capacity_col.value()] = 1;
          cnrow[fnc] = -1;

          cnrows[buid] = lp.add_row(std::move(cnrow.greater_equal(0)));
        }
      }
    }
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};

  if (const auto& stage_reactance = sc.stage_reactance(stage, reactance)) {
    const auto& theta_a_cols =
        bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
    const auto& theta_b_cols =
        bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

    if (!theta_a_cols.empty() && !theta_b_cols.empty()) {
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
                .name =
                    sc.lp_label(scenario, stage, block, cname, "theta", uid()),
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

      theta_rows[st_key] = std::move(trows);
    }
  }

  // storing the indices for this scenario and stage
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);
  lossp_cols[st_key] = std::move(lpcols);
  lossn_cols[st_key] = std::move(lncols);

  return true;
}

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
