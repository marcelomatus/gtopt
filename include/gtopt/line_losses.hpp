/**
 * @file      line_losses.hpp
 * @brief     Modular transmission line losses engine
 * @date      2026-04-03
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a pluggable loss-model dispatch for transmission lines.
 * Each LineLossesMode has its own implementation function that adds
 * the appropriate variables and constraints to the LP.
 *
 * ### Supported modes
 *
 * | Mode          | Extra rows/block | Extra cols/block       |
 * |---------------|-----------------|------------------------|
 * | `none`        | 0               | 1 (bidirectional flow) |
 * | `linear`      | 0               | 1–2 (flow per dir)     |
 * | `piecewise`   | 2               | K+3 (segs + loss + fp + fn) |
 * | `bidirectional` | 4             | 2(K+2) (per-dir segs)  |
 * | `adaptive`    | resolved at config time to piecewise or bidirectional |
 * | `dynamic`     | placeholder → piecewise                  |
 *
 * ### Mathematical background
 *
 * Quadratic loss: `P_loss = R · f² / V²`  [MW], with R [Ω], f [MW], V [kV].
 *
 * Piecewise-linear approximation with K segments over `[0, f_max]`:
 *   - Segment width: `w = f_max / K`
 *   - Segment k (1-based) loss coefficient: `loss_k = w · R · (2k−1) / V²`
 *   - Total: `loss = Σ_k loss_k · seg_k`, with `Σ_k seg_k = |f|`
 *
 * References:
 * - [1] Macedo, Vallejos, Fernández, "A Dynamic Piecewise Linear Model
 *       for DC Transmission Losses in Optimal Scheduling Problems",
 *       IEEE Trans. Power Syst., vol. 26, no. 1, pp. 508–516, 2011.
 * - [2] Wood & Wollenberg, "Power Generation, Operation and Control",
 *       3rd ed., Wiley, Ch. 13 (incremental transmission losses).
 * - [3] FERC Staff Paper, "Optimal Power Flow Paper 2: Linearization",
 *       December 2012.
 */

#pragma once

#include <optional>
#include <string_view>

#include <gtopt/line.hpp>
#include <gtopt/linear_problem.hpp>

namespace gtopt
{

class PlanningOptionsLP;
class SystemContext;
class ScenarioLP;
class StageLP;
class BlockLP;

namespace line_losses
{

// ─── Configuration ──────────────────────────────────────────────────

/**
 * @brief Resolved loss parameters for LP construction.
 *
 * Built once per (line, stage) and used for every block in that stage.
 */
struct LossConfig
{
  LineLossesMode mode {};  ///< Fully resolved (no `adaptive`)
  LossAllocationMode allocation {};
  double lossfactor {};  ///< Effective linear loss [p.u.]
  double resistance {};  ///< R [Ω]
  double V2 {};  ///< V² [kV²]
  int nseg {1};  ///< Segment count for PWL modes
};

// ─── Results ────────────────────────────────────────────────────────

/**
 * @brief LP indices produced by add_block() for one block.
 */
struct BlockResult
{
  std::optional<ColIndex> fp_col;  ///< A→B flow column
  std::optional<ColIndex> fn_col;  ///< B→A flow column
  std::optional<ColIndex> lossp_col;  ///< A→B loss column (PWL modes)
  std::optional<ColIndex> lossn_col;  ///< B→A loss column (PWL modes)
  std::optional<RowIndex> capp_row;  ///< A→B capacity constraint
  std::optional<RowIndex> capn_row;  ///< B→A capacity constraint
};

// ─── Mode resolution ────────────────────────────────────────────────

/**
 * @brief Resolve the effective LineLossesMode for a line.
 *
 * Fallback chain:
 *   1. Per-line `line_losses_mode` (string → enum)
 *   2. Per-line `use_line_losses` (deprecated bool: false → none)
 *   3. Global `line_losses_mode()` from PlanningOptionsLP
 *
 * If the resolved mode is `adaptive`, it is mapped to:
 *   - `bidirectional` if the line has expansion modules (`has_expansion`)
 *   - `piecewise` otherwise
 *
 * If the resolved mode is `dynamic`, it falls back to `piecewise`
 * (with a log warning on first call).
 *
 * @param line            The line data (per-element overrides)
 * @param options         Global planning options
 * @param has_expansion   Whether the line has capacity expansion (expcap)
 */
[[nodiscard]] LineLossesMode resolve_mode(const Line& line,
                                          const PlanningOptionsLP& options,
                                          bool has_expansion);

/**
 * @brief Build a LossConfig for the given line and stage parameters.
 *
 * Combines mode resolution with physical parameter extraction.
 * For `linear` mode, auto-computes lossfactor from R/V if needed:
 *   `λ = R · f_max / V²`  (linearization at rated flow [2]).
 *
 * For PWL modes, validates that R > 0 and V > 0 and nseg > 1;
 * falls back to `linear` or `none` if insufficient.
 */
[[nodiscard]] LossConfig make_config(LineLossesMode mode,
                                     const Line& line,
                                     LossAllocationMode allocation,
                                     double lossfactor,
                                     double resistance,
                                     double voltage,
                                     int loss_segments,
                                     double fmax);

// ─── LP construction ────────────────────────────────────────────────

/**
 * @brief Add loss model variables and constraints for one block.
 *
 * Dispatches to the appropriate mode implementation:
 *   - `none`:          single bidirectional flow, no loss
 *   - `linear`:        directional flows with loss coefficients
 *   - `piecewise`:     shared segments for |f| = fp + fn [1]
 *   - `bidirectional`: independent segments per direction [3]
 *
 * @return LP indices for the created variables and constraints.
 */
[[nodiscard]] BlockResult add_block(const LossConfig& config,
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
                                    Uid uid);

}  // namespace line_losses
}  // namespace gtopt
