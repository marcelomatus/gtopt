/**
 * @file      carrier_converter_lp.hpp
 * @brief     LP wrapper for ``CarrierConverter``
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Creates one ``input`` column per (scenario, stage, block) with
 * cost ``ocost`` and upper bound ``capacity``.  Stamps
 *   * ``−1``          into the from-node balance row
 *   * ``+efficiency`` into the to-node balance row
 *
 * Dispatch on the runtime ``Carrier`` enum picks the correct node-LP
 * type to look up.  This is the only place where carrier-string-like
 * dispatch happens; everywhere else the C++ type system carries the
 * carrier identity at compile time.
 */

#pragma once

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/carrier_converter.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{

using CarrierConverterLPId = ObjectId<class CarrierConverterLP>;
using CarrierConverterLPSId = ObjectSingleId<class CarrierConverterLP>;

class CarrierConverterLP : public CapacityObjectLP<CarrierConverter>
{
public:
  static constexpr std::string_view InputName {"input"};
  static constexpr std::string_view TypeKey {"type"};

  using CapacityBase = CapacityObjectLP<CarrierConverter>;

  [[nodiscard]] constexpr auto&& carrier_converter(this auto&& self) noexcept
  {
    return self.object();
  }

  explicit CarrierConverterLP(const CarrierConverter& pcc,
                              const InputContext& ic);

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  [[nodiscard]] constexpr auto&& input_cols_at(const ScenarioLP& scenario,
                                               const StageLP& stage) const
  {
    return input_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for PAMPL user-constraint resolution
  /// @{
  [[nodiscard]] auto param_efficiency(StageUid s, BlockUid b) const
  {
    return efficiency.at(s, b);
  }
  [[nodiscard]] auto param_ocost(StageUid s, BlockUid b) const
  {
    return ocost.at(s, b);
  }
  /// @}

private:
  OptTBRealSched efficiency;
  OptTBRealSched ocost;

  STBIndexHolder<ColIndex> input_cols;
};

static_assert(CarrierConverterLP::Element::class_name
                  == LPClassName {"CarrierConverter"},
              "CarrierConverter::class_name must remain \"CarrierConverter\"");

}  // namespace gtopt
