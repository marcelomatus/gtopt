/**
 * @file      hydrogen_storage_lp.hpp
 * @brief     LP wrapper for the ``HydrogenStorage`` element
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Peer of ``BatteryLP`` / ``ThermalStorageLP`` / ``ReservoirLP`` on
 * the ``StorageLP<>`` framework.  Identical LP shape: one ``finp``
 * column per (scenario, stage, block) for charging, one ``fout``
 * column for discharging, one ``energy`` column for SoC, and the
 * standard energy-balance row.  All differences are semantic:
 *   * Energy is MWh_LHV, not MWh_e.
 *   * The carrier-side reference (``HydrogenStorage.hydrogen_node``)
 *     resolves against ``HydrogenNode``, not ``Bus``.
 *
 * @see hydrogen_storage.hpp   data struct
 * @see battery_lp.hpp         peer electric storage
 * @see ammonia_storage_lp.hpp downstream NH₃ storage
 */

#pragma once

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/hydrogen_storage.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using HydrogenStorageLPId = ObjectId<class HydrogenStorageLP>;
using HydrogenStorageLPSId = ObjectSingleId<class HydrogenStorageLP>;

class HydrogenStorageLP : public StorageLP<CapacityObjectLP<HydrogenStorage>>
{
public:
  static constexpr std::string_view FinpName {"finp"};
  static constexpr std::string_view FoutName {"fout"};
  static constexpr std::string_view ChargeName {"charge"};
  static constexpr std::string_view DischargeName {"discharge"};
  static constexpr std::string_view TypeKey {"type"};

  using CapacityBase = CapacityObjectLP<HydrogenStorage>;
  using StorageBase = StorageLP<CapacityObjectLP<HydrogenStorage>>;

  [[nodiscard]] constexpr auto&& hydrogen_storage(this auto&& self) noexcept
  {
    return self.object();
  }

  explicit HydrogenStorageLP(const HydrogenStorage& phs,
                             const InputContext& ic);

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  [[nodiscard]] constexpr auto&& finp_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return finp_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr auto&& fout_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return fout_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] auto param_input_efficiency(StageUid s, BlockUid b) const
  {
    return input_efficiency.at(s, b);
  }
  [[nodiscard]] auto param_output_efficiency(StageUid s, BlockUid b) const
  {
    return output_efficiency.at(s, b);
  }

private:
  OptTBRealSched input_efficiency;
  OptTBRealSched output_efficiency;

  STBIndexHolder<ColIndex> finp_cols;
  STBIndexHolder<ColIndex> fout_cols;
};

static_assert(HydrogenStorageLP::Element::class_name
                  == LPClassName {"HydrogenStorage"},
              "HydrogenStorage::class_name must remain \"HydrogenStorage\"");

}  // namespace gtopt
