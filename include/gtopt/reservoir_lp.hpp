/**
 * @file      reservoir_lp.hpp
 * @brief     Header of
 * @date      Wed Jul 30 23:15:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using ReservoirLPId = ObjectId<class ReservoirLP>;
using ReservoirLPSId = ObjectSingleId<class ReservoirLP>;

class ReservoirLP : public StorageLP<ObjectLP<Reservoir>>
{
public:
  constexpr static std::string_view ClassName = "Reservoir";

  using StorageBase = StorageLP<ObjectLP<Reservoir>>;

  explicit ReservoirLP(Reservoir preservoir, const InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] auto junction() { return JunctionLPSId {reservoir().junction}; }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched capacity;
  STBIndexHolder<ColIndex> extraction_cols;
};

}  // namespace gtopt
