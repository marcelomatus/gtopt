// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

#include <gtopt/constraint_names.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt::line_losses
{

// ─── Mode resolution ────────────────────────────────────────────────

namespace
{

/// Map `adaptive` → piecewise/piecewise_direct/bidirectional;
/// `dynamic` → piecewise; demote `piecewise_direct` → `piecewise` if
/// expansion is active.
///
/// `adaptive` picks the smallest-LP piecewise-linear option for the
/// active KVL formulation:
///   - has expansion           → `bidirectional` (2K+4 cols, 4 rows)
///   - no expansion + cycle_basis → `piecewise_direct` (2K cols, 0 rows)
///   - no expansion + node_angle  → `piecewise`        (K+3 cols, 2 rows)
///
/// Under cycle_basis the per-cycle KVL row already supports stamping
/// segments directly (see ``kirchhoff_cycle_basis.cpp:379-390``), so
/// the aggregator + link + loss rows of `piecewise` add no information
/// — picking `piecewise_direct` saves 2 rows per (line, block, scenario,
/// stage) at the cost of skipping the per-line `flowp`/`flown` solution
/// columns.  AMPL access to `line.flow` is preserved via the multi-col
/// segment-sum registration in ``line_lp.cpp``.
///
/// `piecewise_direct` is selectable explicitly in either KVL mode.
constexpr LineLossesMode resolve_adaptive_dynamic(LineLossesMode mode,
                                                  bool has_expansion,
                                                  KirchhoffMode kirchhoff_mode)
{
  switch (mode) {
    case LineLossesMode::adaptive:
      if (has_expansion) {
        return LineLossesMode::bidirectional;
      }
      return (kirchhoff_mode == KirchhoffMode::cycle_basis)
          ? LineLossesMode::piecewise_direct
          : LineLossesMode::piecewise;
    case LineLossesMode::dynamic:
      return LineLossesMode::piecewise;
    case LineLossesMode::piecewise_direct:
      return has_expansion ? LineLossesMode::piecewise
                           : LineLossesMode::piecewise_direct;
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

  if (mode == LineLossesMode::piecewise_direct && has_expansion) {
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "line_losses_mode 'piecewise_direct' requires no capacity "
          "column; falling back to 'piecewise' on expandable lines");
      warned = true;
    }
  }

  return resolve_adaptive_dynamic(
      mode, has_expansion, options.kirchhoff_mode());
}

// ─── Config builder ─────────────────────────────────────────────────

LossConfig make_config(LineLossesMode mode,
                       const Line& /*line*/,
                       LossAllocationMode allocation,
                       double lossfactor,
                       double resistance,
                       double voltage,
                       int loss_segments,
                       double fmax,
                       double loss_row_scale)
{
  const double V2 = voltage * voltage;
  const int nseg = std::max(1, loss_segments);

  // Validate PWL prerequisites; fall back gracefully.
  if (mode == LineLossesMode::piecewise || mode == LineLossesMode::bidirectional
      || mode == LineLossesMode::piecewise_direct)
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
      .loss_row_scale = (loss_row_scale > 0.0) ? loss_row_scale : 1.0,
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
          .class_name = Line::class_name.full_name(),
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

/// Tolerance below which a segment's loss coefficient is treated as
/// numerically negligible and the segment is skipped from the
/// lossrow.  Mirrors the
/// `PlanningOptionsLP::default_dc_line_reactance_threshold` pattern (1e-6 p.u.)
/// used by `validate_line_reactance` — when the LP coefficient is this small,
/// it adds no measurable physical effect and just pollutes the matrix
/// coefficient-range statistics.
///
/// The minimum representative segment-1 loss coefficient on
/// realistic transmission inputs is several orders of magnitude
/// above this threshold (a 500 kV line with R=0.1 Ω and tmax≈1000 MW
/// gives `loss_k_1 = (tmax/nseg)·R/V² ≈ 1.3e-6 · k_factor` — well
/// above 1e-6 for any non-degenerate input).  Lines that fall below
/// it are virtually-lossless transformers / busbar segments / data
/// errors where the loss model contributes nothing.
constexpr double kLossCoeffTolerance = 1e-6;

/// Add piecewise-linear segment variables to linking and loss rows.
///
/// Segment k (1-based) has:
///   width      = seg_width
///   loss_coeff = seg_width · R · (2k−1) / V²   [1]
///
/// This approximates P_loss = R · f² / V² via a piecewise affine function.
///
/// `loss_row_scale` multiplies the segment coefficients in `lossrow`
/// only (the link row is untouched, so the bus-balance stamp on each
/// seg variable is unaffected).  Caller must apply the same scale to
/// `lossrow[loss_col]` so `s · loss − Σ s · loss_k · seg_k = 0` stays
/// consistent.
///
/// Per-segment dropout: when `|loss_k| < kLossCoeffTolerance` the
/// segment column is still created and stamped into the link row
/// (preserving the line's full piecewise capacity) but is NOT
/// stamped into the loss row.  Folding a `~1e-7`-scale coefficient
/// into the LP matrix would just pollute the row coefficient-range
/// statistics without contributing measurable physical loss.  See
/// the `kLossCoeffTolerance` block above for the rationale and the
/// matching `validate_line_reactance` clamp pattern in
/// `planning_lp.cpp`.
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
                  SparseRow& lossrow,
                  double loss_row_scale)
{
  for (const auto k : iota_range(1, nseg + 1)) {
    const double loss_k =
        seg_width * resistance * static_cast<double>((2 * k) - 1) / V2;

    const auto seg_col = lp.add_col({
        .lowb = 0,
        .uppb = seg_width,
        .class_name = Line::class_name.full_name(),
        .variable_name = seg_label,
        .variable_uid = uid,
        .context =
            make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
    });

    linkrow[seg_col] = -1.0;
    if (std::abs(loss_k) < kLossCoeffTolerance) {
      // Skip the lossrow stamp — segment still participates in the
      // link row above (capacity preserved) but contributes zero
      // loss approximation.  No log here: the validation-time
      // `validate_line_reactance` warn covers the data-entry
      // outlier case at the line level.
      continue;
    }
    lossrow[seg_col] = -loss_k * loss_row_scale;
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
    .flow = LineLP::FlowpName,
    .seg = "flowp_seg",
    .loss = LineLP::LosspName,
    .link = "flowp_link",
    .loss_link = "lossp_link",
    .cap = LineLP::CapacitypName,
};

inline constexpr DirLabels negative_labels {
    .flow = LineLP::FlownName,
    .seg = "flown_seg",
    .loss = LineLP::LossnName,
    .link = "flown_link",
    .loss_link = "lossn_link",
    .cap = LineLP::CapacitynName,
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
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::FlowpName,
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
      .seg_p_cols = {},
      .seg_n_cols = {},
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
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlowpName,
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fp_col = fpc;
    apply_linear_allocation(
        brow_a, brow_b, fpc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capp_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitypName,
                                         uid,
                                         *capacity_col,
                                         fpc);
    }
  }

  // B→A direction: bus_b sends, bus_a receives.
  if (block_tmax_ba > 0.0) {
    const auto fnc = lp.add_col({
        .lowb = 0.0,
        .uppb = block_tmax_ba,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlownName,
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fn_col = fnc;
    apply_linear_allocation(
        brow_b, brow_a, fnc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capn_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitynName,
                                         uid,
                                         *capacity_col,
                                         fnc);
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
  assert(nseg > 0 && "line_losses: nseg must be positive");
  // linkrow non-zeros: fp + fn + K segs ≤ K + 2.
  // lossrow non-zeros: loss + K segs    ≤ K + 1 (tighter, separate var).
  const auto link_reserve_sz = static_cast<size_t>(nseg) + 2;
  const auto loss_reserve_sz = static_cast<size_t>(nseg) + 1;
  const double seg_width = fmax / nseg;

  // Hoisted once per (block, line): the AMPL/PAMPL block context is
  // identical for fp_col, fn_col, loss_col, linkrow and lossrow.
  // Pre-2026-05-14 each site rebuilt it from (scenario.uid(),
  // stage.uid(), block.uid()) — 5× per block, ≈ 5×K-of-segments calls
  // are similarly hoisted inside ``add_segments`` via the caller.
  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());

  // Directional flow variables (for bus balance + Kirchhoff).
  if (block_tmax_ab > 0.0) {
    const auto fpc = lp.add_col({
        .lowb = 0,
        .uppb = block_tmax_ab,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlowpName,
        .variable_uid = uid,
        .context = block_ctx,
    });
    result.fp_col = fpc;
    brow_a[fpc] = -1.0;
    brow_b[fpc] = +1.0;
  }

  if (block_tmax_ba > 0.0) {
    const auto fnc = lp.add_col({
        .lowb = 0,
        .uppb = block_tmax_ba,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlownName,
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
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::LosspName,
      .variable_uid = uid,
      .context = block_ctx,
  });
  result.lossp_col = loss_col;

  apply_loss_allocation(brow_a, brow_b, loss_col, config.allocation);

  // Linking: fp + fn − Σ seg_k = 0
  auto linkrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = flow_link_constraint_name,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  linkrow.reserve(link_reserve_sz);
  if (result.fp_col) {
    linkrow[*result.fp_col] = +1.0;
  }
  if (result.fn_col) {
    linkrow[*result.fn_col] = +1.0;
  }

  // Loss tracking: s · loss − Σ s · loss_k · seg_k = 0   (s = loss_row_scale)
  auto lossrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = loss_link_constraint_name,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  lossrow.reserve(loss_reserve_sz);
  lossrow[loss_col] = +config.loss_row_scale;

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
               lossrow,
               config.loss_row_scale);

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  // Capacity constraints (only for expansion lines).
  if (capacity_col) {
    if (result.fp_col) {
      result.capp_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitypName,
                                         uid,
                                         *capacity_col,
                                         *result.fp_col);
    }
    if (result.fn_col) {
      result.capn_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitynName,
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
  assert(nseg > 0 && "line_losses: nseg must be positive");
  const auto reserve_sz = static_cast<size_t>(nseg) + 1;
  const double seg_width = block_tmax / nseg;

  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());
  const auto flow_col = lp.add_col({
      .lowb = 0,
      .uppb = block_tmax,
      .cost = block_tcost,
      .class_name = Line::class_name.full_name(),
      .variable_name = labels.flow,
      .variable_uid = uid,
      .context = block_ctx,
  });

  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .class_name = Line::class_name.full_name(),
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
  // `block_ctx` already hoisted at top of function — reuse here
  // instead of rebuilding it twice more (linkrow + lossrow).
  auto linkrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = labels.link,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  linkrow.reserve(reserve_sz);
  linkrow[flow_col] = +1.0;

  // Loss tracking: s · loss − Σ s · loss_k · f_seg_k = 0   (s = loss_row_scale)
  auto lossrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = labels.loss_link,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  lossrow.reserve(reserve_sz);
  lossrow[loss_col] = +config.loss_row_scale;

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
               lossrow,
               config.loss_row_scale);

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
      .seg_p_cols = {},
      .seg_n_cols = {},
  };
}

/// PLP-direct per-direction helper.
///
/// For one direction (positive: a=sending, b=receiving; negative:
/// a=receiving, b=sending) builds K segment cols + 1 aggregation col +
/// 1 linking row.  Each segment is stamped directly into the bus rows
/// with its per-segment loss factor λ_k baked into the coefficients
/// (PLP `genpdlin.f:107-164`).
///
/// Returns the per-segment column list for one direction (PLP-faithful):
///   - K segment columns, each bounded `[0, w = tmax/K]`
///   - Per-segment bus-balance stamps with allocation-aware loss factor
///     `λ_k = w · (2k−1) · R / V²` (PLP `genpdlin.f:107-114`).
///
/// **No aggregator column, no link row.**  The Kirchhoff (KVL) row stamps
/// each segment with `±x_τ` so the algebraic identity `Σ seg_k = |f|` is
/// recovered without an explicit equality row.  This is what halves the
/// row count vs the older `piecewise` mode, and matches PLP exactly.
///
/// Per-segment transmission cost: split the line tcost across segments
/// (`tcost_k = block_tcost / K`) so `Σ tcost_k · seg_k = tcost · |f|`
/// after the segments saturate left-to-right.  At the optimum, segments
/// fill in order of increasing loss factor, so the cost stays consistent
/// with the legacy aggregator-based formulation.
[[nodiscard]] std::vector<ColIndex> add_direct_direction(
    const LossConfig& config,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    LinearProblem& lp,
    SparseRow& sending_brow,
    SparseRow& receiving_brow,
    double block_tmax,
    double block_tcost,
    Uid uid,
    const DirLabels& labels)
{
  if (block_tmax <= 0.0) {
    return {};
  }

  const int nseg = config.nseg;
  assert(nseg > 0 && "line_losses: nseg must be positive");
  const double seg_width = block_tmax / nseg;
  const double seg_tcost = block_tcost / nseg;

  std::vector<ColIndex> seg_cols;
  seg_cols.reserve(static_cast<size_t>(nseg));

  for (const auto k : iota_range(1, nseg + 1)) {
    const double lf_k = seg_width * config.resistance
        * static_cast<double>((2 * k) - 1) / config.V2;

    const auto seg_col = lp.add_col({
        .lowb = 0,
        .uppb = seg_width,
        .cost = seg_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = labels.seg,
        .variable_uid = uid,
        .context =
            make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
    });

    apply_linear_allocation(
        sending_brow, receiving_brow, seg_col, lf_k, config.allocation);
    seg_cols.push_back(seg_col);
  }

  return seg_cols;
}

/// PLP-direct piecewise-linear: no loss variables, no loss-tracking
/// rows.  Per-segment bus stamps encode the quadratic loss curve
/// directly.  Requires no capacity column.
///
/// Variables per block: fp_agg, fn_agg, 2·K segment cols.
/// Constraints per block: 2 linking rows (one per direction).
///
/// Ref: PLP `genpdlin.f` (GenPDLinA).
BlockResult add_piecewise_direct(const LossConfig& config,
                                 const ScenarioLP& scenario,
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
  auto seg_p_cols = add_direct_direction(config,
                                         scenario,
                                         stage,
                                         block,
                                         lp,
                                         brow_a,
                                         brow_b,
                                         block_tmax_ab,
                                         block_tcost,
                                         uid,
                                         positive_labels);

  auto seg_n_cols = add_direct_direction(config,
                                         scenario,
                                         stage,
                                         block,
                                         lp,
                                         brow_b,
                                         brow_a,
                                         block_tmax_ba,
                                         block_tcost,
                                         uid,
                                         negative_labels);

  return {
      .fp_col = {},
      .fn_col = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
      .seg_p_cols = std::move(seg_p_cols),
      .seg_n_cols = std::move(seg_n_cols),
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

    case LineLossesMode::piecewise_direct:
      // `resolve_mode` demotes direct + expansion to `piecewise`,
      // so capacity_col must be empty here.  This is a **hard**
      // contract — if a future code path bypasses `resolve_mode`
      // and reaches `piecewise_direct` with `capacity_col` set,
      // the segments would be created without any capacity row to
      // constrain them, silently producing a model where flow can
      // exceed line capacity.  The Release build compiles asserts
      // out, so the prior `assert(!capacity_col, ...)` would have
      // missed it.  Promote to a runtime check that survives
      // -DNDEBUG.
      if (capacity_col) [[unlikely]] {
        spdlog::critical(
            "line_losses: piecewise_direct dispatcher reached with "
            "capacity_col (line uid={}); resolve_mode demotion was "
            "bypassed.  Aborting to avoid silently producing an "
            "unconstrained-capacity LP.",
            uid);
        flush_default_logger_best_effort();
        std::abort();
      }
      return add_piecewise_direct(config,
                                  scenario,
                                  stage,
                                  block,
                                  lp,
                                  brow_a,
                                  brow_b,
                                  block_tmax_ab,
                                  block_tmax_ba,
                                  block_tcost,
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
