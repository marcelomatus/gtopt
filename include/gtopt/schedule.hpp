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
#include <gtopt/input_context.hpp>
#include <gtopt/input_traits.hpp>

namespace gtopt
{

template<typename Type, typename... Uid>
class Schedule : public InputTraits
{
public:
  using value_type = Type;
  using vector_type = typename InputTraits::idx_vector_t<Type, Uid...>;
  using FSched = FieldSched<Type, vector_type>;

  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

private:
  FSched m_sched_;
  array_vector_uid_idx_v m_arrow_array_uid_;

public:
  Schedule() = default;

  explicit Schedule(FSched psched)
      : m_sched_(std::move(psched))
  {
  }

  template<typename InputContext>
  explicit Schedule(const InputContext& ic,
                    const std::string_view& cname,
                    const Id& id,
                    FSched psched)
      : m_sched_(std::move(psched))
      , m_arrow_array_uid_(ic.template get_array_index<Type, FSched, Uid...>(
            m_sched_, cname, id))
  {
  }

  template<typename InputContext, typename FS>
  explicit Schedule(const InputContext& ic, FS&& sched)
      : m_sched_(std::forward<FS>(sched))
  {
    (void)ic;
  }

  constexpr Type at(Uid... uids) const
  {
    return at_sched<Type>(m_sched_, m_arrow_array_uid_, uids...);
  }
};

template<typename Type, typename... Uid>
class OptSchedule : public InputTraits
{
public:
  using value_type = Type;
  using vector_type = typename InputTraits::idx_vector_t<Type, Uid...>;
  using FSched = FieldSched<Type, vector_type>;
  using OptFSched = std::optional<FSched>;

  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

private:
  OptFSched m_sched_;
  array_vector_uid_idx_v m_arrow_array_uids_;

public:
  OptSchedule() = default;

  explicit OptSchedule(OptFSched sched)
      : m_sched_(std::move(sched))
  {
  }

  explicit OptSchedule(const InputContext& ic,
                       const std::string_view& cname,
                       const Id& id,
                       OptFSched psched)
      : m_sched_(std::move(psched))
      , m_arrow_array_uids_(ic.template get_array_index<Type, FSched, Uid...>(
            m_sched_.value_or(FSched {}), cname, id))
  {
  }

  [[nodiscard]] constexpr bool has_value() const
  {
    return m_sched_.has_value();
  }

  constexpr explicit operator bool() const { return has_value(); }

  constexpr std::optional<Type> at(Uid... uids) const
  {
    if (m_sched_) {
      return at_sched<Type>(*m_sched_, m_arrow_array_uids_, uids...);
    }
    return {};
  }

  constexpr std::optional<Type> optval(Uid... uids) const
  {
    if (m_sched_) {
      return optval_sched<Type>(*m_sched_, m_arrow_array_uids_, uids...);
    }
    return {};
  }
};

using ActiveSched = Schedule<IntBool, StageUid>;

using TRealSched = Schedule<Real, StageUid>;
using STRealSched = Schedule<Real, ScenarioUid, StageUid>;
using TBRealSched = Schedule<Real, StageUid, BlockUid>;
using STBRealSched = Schedule<Real, ScenarioUid, StageUid, BlockUid>;

using OptTRealSched = OptSchedule<Real, StageUid>;
using OptSTRealSched = OptSchedule<Real, ScenarioUid, StageUid>;
using OptTBRealSched = OptSchedule<Real, StageUid, BlockUid>;
using OptSTBRealSched = OptSchedule<Real, ScenarioUid, StageUid, BlockUid>;

}  // namespace gtopt
