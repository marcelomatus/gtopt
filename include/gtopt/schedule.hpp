/**
 * @file      schedule.hpp
 * @brief     Header of
 * @date      Sun Jun  1 15:35:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/input_traits.hpp>

namespace gtopt
{

template<typename Type, typename... Index>
class Schedule : InputTraits
{
public:
  using traits = mvector_traits<Type, Index...>;
  using value_type = Type;
  using vector_type = typename traits::vector_type;
  using FSched = FieldSched<Type, vector_type>;

private:
  FSched sched;
  array_index_t<Index...> array_index;

public:
  Schedule() = default;

  template<typename FSched>
    requires(!std::same_as<std::remove_cvref_t<FSched>, Schedule>)
  explicit Schedule(FSched&& psched)
      : sched(std::forward<FSched>(psched))
  {
  }

  template<typename InputContext>
  explicit Schedule(const InputContext& ic,
                    const std::string_view& cname,
                    const Id& id,
                    FSched&& psched)
      : sched(std::move(psched))
      , array_index(
            ic.template get_array_index<FSched, Index...>(sched, cname, id))
  {
  }

  template<typename InputContext>
  explicit Schedule(const InputContext& ic, FSched&& psched)
      : sched(std::move(psched))
  {
    (void)ic;
  }

  constexpr auto at(Index... index) const
  {
    return at_sched<Type>(sched, array_index, index...);
  }
};

template<typename Type, typename... Index>
class OptSchedule : InputTraits
{
public:
  using traits = mvector_traits<Type, Index...>;
  using value_type = Type;
  using vector_type = typename traits::vector_type;
  using FSched = FieldSched<Type, vector_type>;
  using OptFSched = std::optional<FSched>;

private:
  OptFSched sched;
  array_index_t<Index...> array_index;

public:
  OptSchedule() = default;

  explicit OptSchedule(const OptFSched& psched)
      : sched(psched)
  {
  }

  explicit OptSchedule(OptFSched&& psched) noexcept
      : sched(std::move(psched))
  {
  }

  template<typename InputContext, typename OptFSched>
  explicit OptSchedule(const InputContext& ic,
                       const std::string_view& cname,
                       const Id& id,
                       OptFSched&& psched)
      : sched(std::forward<OptFSched>(psched))
      , array_index(
            ic.template get_array_index<std::decay_t<OptFSched>, Index...>(
                sched, cname, id))
  {
  }

  [[nodiscard]] constexpr bool has_value() const { return sched.has_value(); }

  constexpr explicit operator bool() const { return has_value(); }

  constexpr std::optional<Type> at(Index... index) const
  {
    if (sched) {
      return at_sched<Type>(sched.value(), array_index, index...);
    }
    return {};
  }

  constexpr std::optional<Type> optval(Index... index) const
  {
    if (sched) {
      return optval_sched<Type>(sched.value(), array_index, index...);
    }
    return {};
  }
};

using ActiveSched = Schedule<IntBool, StageIndex>;

using TRealSched = Schedule<Real, StageIndex>;
using STRealSched = Schedule<Real, ScenarioIndex, StageIndex>;
using TBRealSched = Schedule<Real, StageIndex, BlockIndex>;
using STBRealSched = Schedule<Real, ScenarioIndex, StageIndex, BlockIndex>;

using OptTRealSched = OptSchedule<Real, StageIndex>;
using OptSTRealSched = OptSchedule<Real, ScenarioIndex, StageIndex>;
using OptTBRealSched = OptSchedule<Real, StageIndex, BlockIndex>;
using OptSTBRealSched =
    OptSchedule<Real, ScenarioIndex, StageIndex, BlockIndex>;

}  // namespace gtopt
