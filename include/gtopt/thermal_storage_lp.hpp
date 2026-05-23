/**
 * @file      thermal_storage_lp.hpp
 * @brief     LP wrapper for the ``ThermalStorage`` element
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Peer of ``BatteryLP`` and ``ReservoirLP`` on the ``StorageLP<>``
 * framework.  Identical LP shape to ``BatteryLP``: one ``finp``
 * column per (scenario, stage, block) for charging, one ``fout``
 * column for discharging, one ``energy`` column for SoC, and the
 * standard energy-balance row.  All differences are semantic:
 *   * Energy is MWh_th, not MWh_e.
 *   * ``input_efficiency`` / ``output_efficiency`` are the HTF↔salt
 *     heat-exchanger losses on either side of the TES.
 *   * ``emin`` is the molten-salt freezing heel.
 *   * The carrier-side reference (``ThermalStorage.thermal_node``)
 *     resolves against ``ThermalNode``, not ``Bus`` — but
 *     ``ThermalStorageLP`` does NOT wire itself into a thermal-node
 *     balance row here.  That coupling lives on the ``ThermalNodeLP``
 *     (when written) or on a future converter / generator that
 *     references the thermal node directly.
 *
 * @see thermal_storage.hpp   data struct
 * @see battery_lp.hpp        peer electric storage
 */

#pragma once

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/storage_lp.hpp>
#include <gtopt/thermal_storage.hpp>

namespace gtopt
{
using ThermalStorageLPId = ObjectId<class ThermalStorageLP>;
using ThermalStorageLPSId = ObjectSingleId<class ThermalStorageLP>;

/**
 * @class ThermalStorageLP
 * @brief LP representation of a thermal energy storage element.
 *
 * Mirror of ``BatteryLP`` for the thermal carrier.  All numerical
 * accounting (SoC, charge / discharge bounds, capacity row,
 * cross-phase coupling, soft-emin slack, ``efin`` penalty) is
 * inherited from ``StorageLP<>`` unchanged.
 */
class ThermalStorageLP : public StorageLP<CapacityObjectLP<ThermalStorage>>
{
public:
  /// LP column name for the **charging** flow (HTF→salt; MW_th).
  static constexpr std::string_view FinpName {"finp"};
  /// LP column name for the **discharging** flow (salt→HTF; MW_th).
  static constexpr std::string_view FoutName {"fout"};
  /// PAMPL alias resolving to ``finp_cols``.
  static constexpr std::string_view ChargeName {"charge"};
  /// PAMPL alias resolving to ``fout_cols``.
  static constexpr std::string_view DischargeName {"discharge"};
  /// Filter metadata key published by ``add_to_lp`` for ``sum(...)``
  /// predicate matching (mirrors ``BatteryLP::TypeKey``).
  static constexpr std::string_view TypeKey {"type"};

  using CapacityBase = CapacityObjectLP<ThermalStorage>;
  using StorageBase = StorageLP<CapacityObjectLP<ThermalStorage>>;

  /// Access the underlying ``ThermalStorage`` object.
  [[nodiscard]] constexpr auto&& thermal_storage(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Constructs from a ``ThermalStorage`` data struct + input context.
  explicit ThermalStorageLP(const ThermalStorage& pts, const InputContext& ic);

  /// Adds thermal-storage variables and balance rows to the LP.
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  /// Emits primal / dual / cost streams to the output context.
  bool add_to_output(OutputContext& out) const;

  /// Charge-flow columns for (scenario, stage), keyed by block uid.
  [[nodiscard]] constexpr auto&& finp_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return finp_cols.at({scenario.uid(), stage.uid()});
  }

  /// Discharge-flow columns for (scenario, stage), keyed by block uid.
  [[nodiscard]] constexpr auto&& fout_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return fout_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_input_efficiency(StageUid s, BlockUid b) const
  {
    return input_efficiency.at(s, b);
  }
  [[nodiscard]] auto param_output_efficiency(StageUid s, BlockUid b) const
  {
    return output_efficiency.at(s, b);
  }
  /// @}

private:
  OptTBRealSched input_efficiency;
  OptTBRealSched output_efficiency;

  STBIndexHolder<ColIndex> finp_cols;
  STBIndexHolder<ColIndex> fout_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `ThermalStorage::class_name` literal fails the build (LP row labels
// and CSV outputs depend on the exact string `"ThermalStorage"`).
static_assert(ThermalStorageLP::Element::class_name
                  == LPClassName {"ThermalStorage"},
              "ThermalStorage::class_name must remain \"ThermalStorage\"");

}  // namespace gtopt
