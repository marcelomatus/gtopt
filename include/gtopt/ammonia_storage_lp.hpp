/**
 * @file      ammonia_storage_lp.hpp
 * @brief     LP wrapper for the ``AmmoniaStorage`` element
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Peer of ``HydrogenStorageLP`` on the ``StorageLP<>`` framework.
 * Same LP shape; the only differences are class-name labels and the
 * fact that the carrier-side reference resolves against
 * ``AmmoniaNode`` (NH₃, MWh_LHV) rather than ``HydrogenNode``.
 *
 * @see ammonia_storage.hpp     data struct
 * @see hydrogen_storage_lp.hpp upstream H₂ storage
 */

#pragma once

#include <gtopt/ammonia_storage.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using AmmoniaStorageLPId = ObjectId<class AmmoniaStorageLP>;
using AmmoniaStorageLPSId = ObjectSingleId<class AmmoniaStorageLP>;

class AmmoniaStorageLP : public StorageLP<CapacityObjectLP<AmmoniaStorage>>
{
public:
  static constexpr std::string_view FinpName {"finp"};
  static constexpr std::string_view FoutName {"fout"};
  static constexpr std::string_view ChargeName {"charge"};
  static constexpr std::string_view DischargeName {"discharge"};
  static constexpr std::string_view TypeKey {"type"};

  using CapacityBase = CapacityObjectLP<AmmoniaStorage>;
  using StorageBase = StorageLP<CapacityObjectLP<AmmoniaStorage>>;

  [[nodiscard]] constexpr auto&& ammonia_storage(this auto&& self) noexcept
  {
    return self.object();
  }

  explicit AmmoniaStorageLP(const AmmoniaStorage& pas, const InputContext& ic);

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

static_assert(AmmoniaStorageLP::Element::class_name
                  == LPClassName {"AmmoniaStorage"},
              "AmmoniaStorage::class_name must remain \"AmmoniaStorage\"");

}  // namespace gtopt
