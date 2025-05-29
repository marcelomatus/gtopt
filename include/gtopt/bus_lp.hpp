/**
 * @file      bus_lp.hpp
 * @brief     Linear Programming representation of a Bus for optimization problems
 * @date      Mon Mar 24 09:40:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The BusLP class provides a linear programming (LP) compatible representation
 * of a Bus, which is a fundamental component for power system optimization.
 * It maintains the bus's electrical properties and provides methods for LP
 * formulation while using modern C++23 features for better ergonomics.
 *
 * @note Uses C++23 features including deducing this and structured bindings
 */

#pragma once

#include <gtopt/bus.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

using BusLPId = ObjectId<class BusLP>;
using BusLPSId = ObjectSingleId<class BusLP>;

class BusLP : public ObjectLP<Bus>
{
public:
  constexpr static std::string_view ClassName = "Bus";

  explicit BusLP(const InputContext& /*ic*/, Bus pbus) noexcept
      : ObjectLP<Bus>(std::move(pbus))
  {}

  // Structured binding support
  template<std::size_t I>
  [[nodiscard]] constexpr auto get() const noexcept {
    if constexpr (I == 0) {
      return id();
    } else if constexpr (I == 1) {
      return static_cast<const BusLP*>(this)->ObjectLP<Bus>::object();
    } else if constexpr (I == 2) {
      return static_cast<const BusLP*>(this)->ObjectLP<Bus>::object().voltage.value_or(1.0);
    }
  }

  [[nodiscard]] constexpr auto bus() const noexcept -> const Bus& {
    return ObjectLP<Bus>::object();
  }

  [[nodiscard]] constexpr auto reference_theta() const noexcept {
    return bus().reference_theta;
  }

  [[nodiscard]] constexpr auto voltage() const noexcept -> double {
    return bus().voltage.value_or(1.0);
  }

  [[nodiscard]] constexpr auto use_kirchhoff() const noexcept -> bool {
    return bus().use_kirchhoff.value_or(true);
  }

  [[nodiscard]] auto needs_kirchhoff(const SystemContext& sc) const noexcept -> bool;

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] const auto& balance_rows_at(const ScenarioIndex scenario_index,
                                          const StageIndex stage_index) const noexcept {
    return balance_rows.at({scenario_index, stage_index});
  }

  [[nodiscard]] auto theta_cols_at(const SystemContext& sc,
                                  const ScenarioLP& scenario,
                                  const StageLP& stage,
                                  LinearProblem& lp,
                                  const std::vector<BlockLP>& blocks) const
      -> const BIndexHolder&
  {
    const auto key = std::make_pair(scenario.index(), stage.index());
    if (auto it = theta_cols.find(key); it != theta_cols.end()) {
      return it->second;
    }
    return lazy_add_theta(sc, scenario, stage, lp, blocks);
  }

private:
  auto lazy_add_theta(const SystemContext& sc,
                     const ScenarioLP& scenario,
                     const StageLP& stage,
                     LinearProblem& lp,
                     const std::vector<BlockLP>& blocks) const
      -> const BIndexHolder&;

  STBIndexHolder balance_rows;
  mutable STBIndexHolder theta_cols;
};

}  // namespace gtopt

// Structured binding support
namespace std
{
template<>
struct tuple_size<gtopt::BusLP> : integral_constant<size_t, 3> {};

template<size_t I>
struct tuple_element<I, gtopt::BusLP> {
  using type = decltype(declval<gtopt::BusLP>().get<I>());
};
} // namespace std
