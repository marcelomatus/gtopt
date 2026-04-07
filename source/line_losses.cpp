// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <string>

#include <gtopt/line_losses.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt::line_losses
{

// ─── Mode resolution ────────────────────────────────────────────────

namespace
{

/// Map `adaptive` → piecewise/bidirectional; `dynamic` → piecewise.
constexpr LineLossesMode resolve_adaptive_dynamic(LineLossesMode mode,
                                                  bool has_expansion)
{
  switch (mode) {
    case LineLossesMode::adaptive:
      return has_expansion ? LineLossesMode::bidirectional
                           : LineLossesMode::piecewise;
    case LineLossesMode::dynamic:
      return LineLossesMode::piecewise;
    default:
      return mode;
  }
}

}  // namespace

LineLossesMode resolve_mode(const Line& line,
                            const PlanningOptionsLP& options,
                            bool has_expansion)
{
  LineLossesMode mode {};

  if (auto m = line.line_losses_mode_enum()) {
    mode = *m;
  } else if (line.use_line_losses.has_value()) {
    if (!*line.use_line_losses) {
      mode = LineLossesMode::none;
    } else {
      // Per-line enables losses: use global mode, but if global is also
      // none fall back to the compiled default (adaptive).
      const auto global = options.line_losses_mode();
      mode = (global != LineLossesMode::none)
          ? global
          : PlanningOptionsLP::default_line_losses_mode;
    }
  } else {
    mode = options.line_losses_mode();
  }

  if (mode == LineLossesMode::dynamic) {
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "line_losses_mode 'dynamic' is not yet implemented; "
          "falling back to 'piecewise'");
      warned = true;
    }
  }

  return resolve_adaptive_dynamic(mode, has_expansion);
}

// ─── Config builder ─────────────────────────────────────────────────

LossConfig make_config(LineLossesMode mode,
                       const Line& /*line*/,
                       LossAllocationMode allocation,
                       double lossfactor,
                       double resistance,
                       double voltage,
                       int loss_segments,
                       double fmax)
{
  const double V2 = voltage * voltage;
  const int nseg = std::max(1, loss_segments);

  // Validate PWL prerequisites; fall back gracefully.
  if (mode == LineLossesMode::piecewise
      || mode == LineLossesMode::bidirectional)
  {
    if (resistance <= 0.0 || V2 <= 0.0 || nseg < 2) {
      if (lossfactor > 0.0) {
        mode = LineLossesMode::linear;
      } else if (resistance > 0.0 && V2 > 0.0 && fmax > 0.0) {
        mode = LineLossesMode::linear;
        lossfactor = resistance * fmax / V2;
      } else {
        mode = LineLossesMode::none;
      }
    }
  }

  // For linear mode, auto-compute lossfactor from R/V if missing.
  // Linearization at rated flow: λ = R · f_max / V²  [2].
  if (mode == LineLossesMode::linear && lossfactor <= 0.0) {
    if (resistance > 0.0 && V2 > 0.0 && fmax > 0.0) {
      lossfactor = resistance * fmax / V2;
    } else {
      mode = LineLossesMode::none;
    }
  }

  return {
      .mode = mode,
      .allocation = allocation,
      .lossfactor = lossfactor,
      .resistance = resistance,
      .V2 = V2,
      .nseg = nseg,
  };
}

// ─── Shared helpers (C++26 style) ───────────────────────────────────

namespace
{

/// Apply linear loss allocation to bus-balance rows for a single direction.
/// `sending` is the bus where power originates; `receiving` is where it
/// arrives.  The loss factor `lf` is allocated per [2].
void apply_linear_allocation(SparseRow& sending,
                             SparseRow& receiving,
                             ColIndex col,
                             double lf,
                             LossAllocationMode allocation)
{
  switch (allocation) {
    case LossAllocationMode::sender:
      sending[col] = -(1.0 + lf);
      receiving[col] = +1.0;
      break;
    case LossAllocationMode::split:
      sending[col] = -(1.0 + (lf / 2.0));
      receiving[col] = +(1.0 - (lf / 2.0));
      break;
    case LossAllocationMode::receiver:
    default:
      sending[col] = -1.0;
      receiving[col] = +(1.0 - lf);
      break;
  }
}

/// Apply loss-variable allocation to bus-balance rows.
/// Used by piecewise (on shared loss) and bidirectional (per-direction loss).
void apply_loss_allocation(SparseRow& sending,
                           SparseRow& receiving,
                           ColIndex loss_col,
                           LossAllocationMode allocation)
{
  switch (allocation) {
    case LossAllocationMode::sender:
      sending[loss_col] = -1.0;
      break;
    case LossAllocationMode::split:
      sending[loss_col] = -0.5;
      receiving[loss_col] = -0.5;
      break;
    case LossAllocationMode::receiver:
    default:
      receiving[loss_col] = -1.0;
      break;
  }
}

/// Add a capacity constraint: capacity_col − flow_col ≥ 0.
auto add_capacity_row(LinearProblem& lp,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      const BlockLP& block,
                      std::string_view label,
                      Uid uid,
                      ColIndex capacity_col,
                      ColIndex flow_col) -> RowIndex
{
  auto row =
      SparseRow {
          .class_name = "Line",
          .constraint_name = label,
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .greater_equal(0);
  row[capacity_col] = 1;
  row[flow_col] = -1;
  return lp.add_row(std::move(row));
}

/// Add piecewise-linear segment variables to linking and loss rows.
///
/// Segment k (1-based) has:
///   width      = seg_width
///   loss_coeff = seg_width · R · (2k−1) / V²   [1]
///
/// This approximates P_loss = R · f² / V² via a piecewise affine function.
void add_segments(LinearProblem& lp,
                  const ScenarioLP& scenario,
                  const StageLP& stage,
                  const BlockLP& block,
                  std::string_view seg_label,
                  Uid uid,
                  double seg_width,
                  double resistance,
                  double V2,
                  int nseg,
                  SparseRow& linkrow,
                  SparseRow& lossrow)
{
  for (const auto k : iota_range(1, nseg + 1)) {
    const double loss_k =
        seg_width * resistance * static_cast<double>((2 * k) - 1) / V2;

    const auto seg_col = lp.add_col({
        .lowb = 0,
        .uppb = seg_width,
        .class_name = "Line",
        .variable_name = seg_label,
        .variable_uid = uid,
        .context =
            make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
    });

    linkrow[seg_col] = -1.0;
    lossrow[seg_col] = -loss_k;
  }
}

/// Labels for one direction of the bidirectional model.
struct DirLabels
{
  std::string_view flow;
  std::string_view seg;
  std::string_view loss;
  std::string_view link;
  std::string_view loss_link;
  std::string_view cap;
};

inline constexpr DirLabels positive_labels {
    .flow = "fp",
    .seg = "fps",
    .loss = "lsp",
    .link = "lnkp",
    .loss_link = "lslp",
    .cap = "capp",
};

inline constexpr DirLabels negative_labels {
    .flow = "fn",
    .seg = "fns",
    .loss = "lsn",
    .link = "lnkn",
    .loss_link = "lsln",
    .cap = "capn",
};

// ─── Per-mode implementations ───────────────────────────────────────

/// No losses: single bidirectional flow variable.
/// Flow balance: P_send = P_recv (no dissipation).
BlockResult add_none(const ScenarioLP& scenario,
                     const StageLP& stage,
                     const BlockLP& block,
                     LinearProblem& lp,
                     SparseRow& brow_a,
                     SparseRow& brow_b,
                     double block_tmax_ab,
                     double block_tmax_ba,
                     double block_tcost,
                     Uid uid)
{
  const auto fpc = lp.add_col({
      .lowb = -block_tmax_ba,
      .uppb = block_tmax_ab,
      .cost = block_tcost,
      .class_name = "Line",
      .variable_name = "fp",
      .variable_uid = uid,
      .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
  });
  brow_a[fpc] = -1.0;
  brow_b[fpc] = +1.0;

  return {
      .fp_col = fpc,
      .fn_col = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
  };
}

/// Linear loss factor: P_loss = λ · |f|.
/// Ref: Wood & Wollenberg [2], Ch. 13.
BlockResult add_linear(const LossConfig& config,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       const BlockLP& block,
                       LinearProblem& lp,
                       SparseRow& brow_a,
                       SparseRow& brow_b,
                       double block_tmax_ab,
                       double block_tmax_ba,
                       double block_tcost,
                       std::optional<ColIndex> capacity_col,
                       Uid uid)
{
  BlockResult result;

  // A→B direction: bus_a sends, bus_b receives.
  if (block_tmax_ab > 0.0) {
    const auto fpc = lp.add_col({
        .lowb = 0.0,
        .uppb = block_tmax_ab,
        .cost = block_tcost,
        .class_name = "Line",
        .variable_name = "fp",
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fp_col = fpc;
    apply_linear_allocation(
        brow_a, brow_b, fpc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capp_row = add_capacity_row(
          lp, scenario, stage, block, "capp", uid, *capacity_col, fpc);
    }
  }

  // B→A direction: bus_b sends, bus_a receives.
  if (block_tmax_ba > 0.0) {
    const auto fnc = lp.add_col({
        .lowb = 0.0,
        .uppb = block_tmax_ba,
        .cost = block_tcost,
        .class_name = "Line",
        .variable_name = "fn",
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fn_col = fnc;
    apply_linear_allocation(
        brow_b, brow_a, fnc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capn_row = add_capacity_row(
          lp, scenario, stage, block, "capn", uid, *capacity_col, fnc);
    }
  }

  return result;
}

/// Piecewise-linear (single-direction): shared segments for |f| = fp + fn.
///
/// Approximates P_loss = R · f² / V²  [1] with K segments.
/// Variables per block: fp, fn, seg_1..seg_K, loss.
/// Constraints per block: 1 linking + 1 loss-tracking = 2 rows.
///
/// Linking:      fp + fn − Σ seg_k = 0
/// Loss track:   loss − Σ loss_k · seg_k = 0
///
/// Ref: Macedo et al. [1], single-direction formulation.
BlockResult add_piecewise(const LossConfig& config,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          const BlockLP& block,
                          LinearProblem& lp,
                          SparseRow& brow_a,
                          SparseRow& brow_b,
                          double block_tmax_ab,
                          double block_tmax_ba,
                          double block_tcost,
                          std::optional<ColIndex> capacity_col,
                          Uid uid)
{
  BlockResult result;
  const double fmax = std::max(block_tmax_ab, block_tmax_ba);
  if (fmax <= 0.0) {
    return result;
  }

  const int nseg = config.nseg;
  const auto reserve_sz = static_cast<size_t>(nseg) + 2;
  const double seg_width = fmax / nseg;

  // Directional flow variables (for bus balance + Kirchhoff).
  if (block_tmax_ab > 0.0) {
    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto fpc = lp.add_col({
        .lowb = 0,
        .uppb = block_tmax_ab,
        .cost = block_tcost,
        .class_name = "Line",
        .variable_name = "fp",
        .variable_uid = uid,
        .context = block_ctx,
    });
    result.fp_col = fpc;
    brow_a[fpc] = -1.0;
    brow_b[fpc] = +1.0;
  }

  if (block_tmax_ba > 0.0) {
    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto fnc = lp.add_col({
        .lowb = 0,
        .uppb = block_tmax_ba,
        .cost = block_tcost,
        .class_name = "Line",
        .variable_name = "fn",
        .variable_uid = uid,
        .context = block_ctx,
    });
    result.fn_col = fnc;
    brow_b[fnc] = -1.0;
    brow_a[fnc] = +1.0;
  }

  // Loss variable: tracks total power dissipated.
  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Line",
      .variable_name = "ls",
      .variable_uid = uid,
      .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
  });
  result.lossp_col = loss_col;

  apply_loss_allocation(brow_a, brow_b, loss_col, config.allocation);

  // Linking: fp + fn − Σ seg_k = 0
  auto linkrow =
      SparseRow {
          .class_name = "Line",
          .constraint_name = "lnk",
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .equal(0);
  linkrow.reserve(reserve_sz);
  if (result.fp_col) {
    linkrow[*result.fp_col] = +1.0;
  }
  if (result.fn_col) {
    linkrow[*result.fn_col] = +1.0;
  }

  // Loss tracking: loss − Σ loss_k · seg_k = 0
  auto lossrow =
      SparseRow {
          .class_name = "Line",
          .constraint_name = "lsl",
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .equal(0);
  lossrow.reserve(reserve_sz);
  lossrow[loss_col] = +1.0;

  add_segments(lp,
               scenario,
               stage,
               block,
               "seg",
               uid,
               seg_width,
               config.resistance,
               config.V2,
               nseg,
               linkrow,
               lossrow);

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  // Capacity constraints (only for expansion lines).
  if (capacity_col) {
    if (result.fp_col) {
      result.capp_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         "capp",
                                         uid,
                                         *capacity_col,
                                         *result.fp_col);
    }
    if (result.fn_col) {
      result.capn_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         "capn",
                                         uid,
                                         *capacity_col,
                                         *result.fn_col);
    }
  }

  return result;
}

/// Add PWL loss model for one direction of the bidirectional model.
///
/// Creates K segment variables, 1 linking row, 1 loss-tracking row.
/// Ref: FERC [3], bidirectional decomposition.
struct DirResult
{
  std::optional<ColIndex> flow_col;
  std::optional<ColIndex> loss_col;
  std::optional<RowIndex> capacity_row;
};

DirResult add_direction(const LossConfig& config,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        const BlockLP& block,
                        LinearProblem& lp,
                        SparseRow& sending_brow,
                        SparseRow& receiving_brow,
                        double block_tmax,
                        double block_tcost,
                        std::optional<ColIndex> capacity_col,
                        Uid uid,
                        const DirLabels& labels)
{
  if (block_tmax <= 0.0) {
    return {};
  }

  const int nseg = config.nseg;
  const auto reserve_sz = static_cast<size_t>(nseg) + 1;
  const double seg_width = block_tmax / nseg;

  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());
  const auto flow_col = lp.add_col({
      .lowb = 0,
      .uppb = block_tmax,
      .cost = block_tcost,
      .class_name = "Line",
      .variable_name = labels.flow,
      .variable_uid = uid,
      .context = block_ctx,
  });

  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Line",
      .variable_name = labels.loss,
      .variable_uid = uid,
      .context = block_ctx,
  });

  // Bus balance: flow and loss allocation.
  sending_brow[flow_col] = -1.0;
  receiving_brow[flow_col] = +1.0;
  apply_loss_allocation(
      sending_brow, receiving_brow, loss_col, config.allocation);

  // Linking: f_total − Σ f_seg_k = 0
  auto linkrow =
      SparseRow {
          .class_name = "Line",
          .constraint_name = labels.link,
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .equal(0);
  linkrow.reserve(reserve_sz);
  linkrow[flow_col] = +1.0;

  // Loss tracking: loss − Σ loss_k · f_seg_k = 0
  auto lossrow =
      SparseRow {
          .class_name = "Line",
          .constraint_name = labels.loss_link,
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .equal(0);
  lossrow.reserve(reserve_sz);
  lossrow[loss_col] = +1.0;

  add_segments(lp,
               scenario,
               stage,
               block,
               labels.seg,
               uid,
               seg_width,
               config.resistance,
               config.V2,
               nseg,
               linkrow,
               lossrow);

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  std::optional<RowIndex> cap_row;
  if (capacity_col) {
    cap_row = add_capacity_row(
        lp, scenario, stage, block, labels.cap, uid, *capacity_col, flow_col);
  }

  return {
      .flow_col = flow_col,
      .loss_col = loss_col,
      .capacity_row = cap_row,
  };
}

/// Bidirectional piecewise-linear: independent segments per direction.
/// Total: 4 rows + 2(K+2) columns per block.
/// Ref: FERC [3].
BlockResult add_bidirectional(const LossConfig& config,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              const BlockLP& block,
                              LinearProblem& lp,
                              SparseRow& brow_a,
                              SparseRow& brow_b,
                              double block_tmax_ab,
                              double block_tmax_ba,
                              double block_tcost,
                              std::optional<ColIndex> capacity_col,
                              Uid uid)
{
  auto [fp, lsp, capp] = add_direction(config,
                                       scenario,
                                       stage,
                                       block,
                                       lp,
                                       brow_a,
                                       brow_b,
                                       block_tmax_ab,
                                       block_tcost,
                                       capacity_col,
                                       uid,
                                       positive_labels);

  auto [fn, lsn, capn] = add_direction(config,
                                       scenario,
                                       stage,
                                       block,
                                       lp,
                                       brow_b,
                                       brow_a,
                                       block_tmax_ba,
                                       block_tcost,
                                       capacity_col,
                                       uid,
                                       negative_labels);

  return {
      .fp_col = fp,
      .fn_col = fn,
      .lossp_col = lsp,
      .lossn_col = lsn,
      .capp_row = capp,
      .capn_row = capn,
  };
}

}  // namespace

// ─── Dispatcher ─────────────────────────────────────────────────────

BlockResult add_block(const LossConfig& config,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      const BlockLP& block,
                      LinearProblem& lp,
                      SparseRow& brow_a,
                      SparseRow& brow_b,
                      double block_tmax_ab,
                      double block_tmax_ba,
                      double block_tcost,
                      std::optional<ColIndex> capacity_col,
                      Uid uid)
{
  switch (config.mode) {
    case LineLossesMode::none:
      return add_none(scenario,
                      stage,
                      block,
                      lp,
                      brow_a,
                      brow_b,
                      block_tmax_ab,
                      block_tmax_ba,
                      block_tcost,
                      uid);

    case LineLossesMode::linear:
      return add_linear(config,
                        scenario,
                        stage,
                        block,
                        lp,
                        brow_a,
                        brow_b,
                        block_tmax_ab,
                        block_tmax_ba,
                        block_tcost,
                        capacity_col,
                        uid);

    case LineLossesMode::piecewise:
      return add_piecewise(config,
                           scenario,
                           stage,
                           block,
                           lp,
                           brow_a,
                           brow_b,
                           block_tmax_ab,
                           block_tmax_ba,
                           block_tcost,
                           capacity_col,
                           uid);

    case LineLossesMode::bidirectional:
      return add_bidirectional(config,
                               scenario,
                               stage,
                               block,
                               lp,
                               brow_a,
                               brow_b,
                               block_tmax_ab,
                               block_tmax_ba,
                               block_tcost,
                               capacity_col,
                               uid);

    default:
      return add_none(scenario,
                      stage,
                      block,
                      lp,
                      brow_a,
                      brow_b,
                      block_tmax_ab,
                      block_tmax_ba,
                      block_tcost,
                      uid);
  }
}

}  // namespace gtopt::line_losses
