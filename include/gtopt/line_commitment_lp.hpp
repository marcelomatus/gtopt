/**
 * @file      line_commitment_lp.hpp
 * @brief     LP formulation for Optimal Transmission Switching (issue #509)
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines ``LineCommitmentLP`` — the LP-build companion to the
 * ``LineCommitment`` JSON element.  For every block of every active
 * (scenario, stage) the class:
 *
 *   1. Creates a binary ``u_l ∈ {0, 1}`` status column (relaxable to
 *      [0, 1] via ``LineCommitment.relax``).
 *   2. Stamps capacity gating rows so ``u_l = 0`` forces ``f_l = 0``:
 *
 *        + direction (A → B): f - F^max_ab · u_l ≤ 0
 *        − direction (B → A): F^max_ba · u_l + f ≥ 0
 *
 *      (rows scale to the per-block ``tmax_ab`` / ``tmax_ba`` schedule;
 *      transparent to whether the underlying flow column is signed —
 *      ``tangent_signed_flow`` — or split into ``fp`` / ``fn``).
 *
 * **v0.5 scope** (issue #509 §"Implementation roadmap"):
 *   - **Capacity gating** — always emitted (transport + Kirchhoff).
 *   - **KVL big-M disjunction** — emitted only in
 *     ``kirchhoff_mode = node_angle``.  The per-line KVL equality
 *     row stamped by ``LineLP`` is rewritten in place as an upper-
 *     side ``≤`` inequality and a new lower-side ``≥`` row is added,
 *     so ``u_l = 0`` decouples ``θ_a`` from ``θ_b`` exactly like an
 *     opened breaker.
 *
 * **Mode gates** (enforced by ``validate_planning``):
 *   - ``method ∈ {sddp, cascade}`` is rejected — Benders cuts on a
 *     mixed-integer subproblem are unsound (Zou-Ahmed-Sun 2019).
 *   - ``kirchhoff_mode = cycle_basis`` (the gtopt default) combined
 *     with active LineCommitment + ``use_kirchhoff = true`` is
 *     rejected — the v0.5 KVL big-M rewrite is node_angle-only.
 *     Users must pick ``kirchhoff_mode = node_angle`` (for DC-OPF
 *     OTS) or ``use_kirchhoff = false`` (transport-mode OTS, which
 *     is bit-for-bit correct under capacity gating alone).  A
 *     follow-up will land the cycle-form disjunctive rewrite.
 */

#pragma once

#include <gtopt/line_commitment.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class LineCommitmentLP : public ObjectLP<LineCommitment>
{
public:
  static constexpr std::string_view StatusName {"status"};
  static constexpr std::string_view CapacityPName {"capacity_p"};
  static constexpr std::string_view CapacityNName {"capacity_n"};
  /// Lower-side KVL big-M row label (Kirchhoff node_angle mode only).
  /// The upper-side ``≤`` half is the same row label the existing
  /// LineLP KVL emission uses (``LineLP::ThetaName``) since we
  /// MUTATE that row in place to become the upper half.
  static constexpr std::string_view KvlMinusName {"kvl_minus"};

  using Base = ObjectLP<LineCommitment>;

  explicit LineCommitmentLP(const LineCommitment& lc, const InputContext& ic);

  [[nodiscard]] constexpr auto&& line_commitment(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto line_sid() const noexcept
  {
    return LineLPSId {line_commitment().line};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Look up the status column for (scenario, stage, block).
  [[nodiscard]] std::optional<ColIndex> lookup_status_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    return lookup_inner(status_cols_, scenario, stage, buid);
  }

private:
  ElementIndex<LineLP> line_index_;
  OptTBRealSched fixed_status_;

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<RowIndex> capacity_p_rows_;
  STBIndexHolder<RowIndex> capacity_n_rows_;
};

// Pin the data-struct constant value so an accidental rename of the
// `LineCommitment::class_name` literal fails the build.
static_assert(LineCommitmentLP::Element::class_name
                  == LPClassName {"LineCommitment"},
              "LineCommitment::class_name must remain \"LineCommitment\"");

}  // namespace gtopt
