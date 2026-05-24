/**
 * @file      allowance_pool_lp.hpp
 * @brief     LP wrapper for ``AllowancePool`` (Phase 2 — banking only)
 * @date      Sun May 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Peer of ``LngTerminalLP`` on the ``StorageLP<>`` framework, with
 * a single difference: the inflow is **free allowance allocation**
 * (the regulator's grandfathering / benchmarking entitlement) and
 * there is no per-generator coupling — emissions consumption is
 * wired in Phase 3 by injecting ``EmissionZone.production`` cols
 * into the pool's energy-balance row.
 *
 * Phase 2 (this file) builds only:
 *   * one ``free_allocation`` column per (scenario, stage, block)
 *     fixed at ``delivery / stage_duration`` (m³ → tCO₂/hour rate),
 *   * the ``StorageLP`` SoC carry rows (banked allowances across
 *     stages), with ``emin`` / ``emax`` / ``efin`` / ``efin_cost``
 *     handled by the base.
 *
 * Phase 3 will add: ``EmissionZone.production`` outflow stamping
 * into the per-block energy rows, replacing the standalone
 * ``EmissionZone.cap`` row with a pool-mediated cap.
 *
 * Phase 4 will add: optional auction-purchase column priced at
 * ``auction_price`` and capped by ``auction_cap``.
 */

#pragma once

#include <gtopt/allowance_pool.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{

using AllowancePoolLPId = ObjectId<class AllowancePoolLP>;
using AllowancePoolLPSId = ObjectSingleId<class AllowancePoolLP>;

class AllowancePoolLP : public StorageLP<ObjectLP<AllowancePool>>
{
public:
  /// LP column name for the free-allocation inflow (per block,
  /// fixed-rate = delivery / stage_duration).
  static constexpr std::string_view FreeAllocationName {"free_allocation"};

  using StorageBase = StorageLP<ObjectLP<AllowancePool>>;

  explicit AllowancePoolLP(const AllowancePool& pool, const InputContext& ic);

  [[nodiscard]] constexpr auto&& allowance_pool(this auto&& self) noexcept
  {
    return self.object();
  }

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  /// Free-allocation column indices, keyed by (scenario, stage, block).
  /// Exposed for Phase 3 unit tests + the upcoming
  /// EmissionZone-coupling code path.
  [[nodiscard]] constexpr auto&& free_allocation_cols_at(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    return free_allocation_cols.at({scenario.uid(), stage.uid()});
  }

private:
  OptTRealSched delivery;
  STBIndexHolder<ColIndex> free_allocation_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `AllowancePool::class_name` literal fails the build (LP row labels
// and CSV outputs depend on the exact string `"AllowancePool"`).
static_assert(AllowancePoolLP::Element::class_name
                  == LPClassName {"AllowancePool"},
              "AllowancePool::class_name must remain \"AllowancePool\"");

}  // namespace gtopt
