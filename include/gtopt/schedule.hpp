/**
 * @file      schedule.hpp
 * @brief     Typed schedule wrapper for indexed field data
 * @date      Sun Jun  1 15:35:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Schedule template class, which wraps a FieldSched
 * and resolves its values into Arrow-indexed arrays for LP construction.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/input_traits.hpp>
#include <gtopt/single_id_sched.hpp>

namespace gtopt
{

template<typename Type, typename... Uid>
class Schedule : public InputTraits
{
public:
  using value_type = Type;
  using vector_type = InputTraits::idx_vector_t<Type, Uid...>;
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
                    std::string_view cname,
                    const Id& id,
                    FSched psched)
      : m_sched_(std::move(psched))
      , m_arrow_array_uid_(ic.template get_array_index<Type, FSched, Uid...>(
            m_sched_, cname, id))
  {
    SPDLOG_DEBUG("Schedule: cname '{}' id '{} {}'", cname, id.first, id.second);
  }

  [[nodiscard]] constexpr Type at(Uid... uids) const
  {
    return at_sched<Type>(m_sched_, m_arrow_array_uid_, uids...);
  }
};

template<typename Type, typename... Uid>
class OptSchedule : public InputTraits
{
public:
  using value_type = Type;
  using vector_type = InputTraits::idx_vector_t<Type, Uid...>;
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
                       std::string_view cname,
                       const Id& id,
                       OptFSched psched)
      : m_sched_(std::move(psched))
  {
    SPDLOG_DEBUG(
        "OptSchedule: cname '{}' id '{} {}'", cname, id.first, id.second);
    if (m_sched_) {
      m_arrow_array_uids_ = ic.template get_array_index<Type, FSched, Uid...>(
          m_sched_.value(), cname, id);
    }
  }

  [[nodiscard]] constexpr bool has_value() const
  {
    return m_sched_.has_value();
  }

  constexpr explicit operator bool() const { return has_value(); }

  [[nodiscard]] constexpr std::optional<Type> at(Uid... uids) const
  {
    if (m_sched_) {
      return at_sched<Type>(*m_sched_, m_arrow_array_uids_, uids...);
    }
    return {};
  }

  [[nodiscard]] constexpr std::optional<Type> optval(Uid... uids) const
  {
    if (m_sched_) {
      return optval_sched<Type>(*m_sched_, m_arrow_array_uids_, uids...);
    }
    return {};
  }
};

using ActiveSched = Schedule<IntBool, StageUid>;
using OptActiveSched = OptSchedule<IntBool, StageUid>;

// Per-(stage, block) IntBool schedule — the block-resolved analog of
// `ActiveSched`.  Backs `Line.in_service` (line open/closed per block).
using TBIntBoolSched = Schedule<IntBool, StageUid, BlockUid>;
using OptTBIntBoolSched = OptSchedule<IntBool, StageUid, BlockUid>;

using TRealSched = Schedule<Real, StageUid>;
using STRealSched = Schedule<Real, ScenarioUid, StageUid>;
using TBRealSched = Schedule<Real, StageUid, BlockUid>;
using STBRealSched = Schedule<Real, ScenarioUid, StageUid, BlockUid>;

using OptTRealSched = OptSchedule<Real, StageUid>;
using OptSTRealSched = OptSchedule<Real, ScenarioUid, StageUid>;
using OptTBRealSched = OptSchedule<Real, StageUid, BlockUid>;
using OptSTBRealSched = OptSchedule<Real, ScenarioUid, StageUid, BlockUid>;

/// LP-side resolver for an `OptTBSingleIdSched` (Phase 1 of Issue #510).
/// Wraps the JSON variant (`SingleId | UidMatrix | FileSched`) and
/// resolves a per-(stage, block) Uid lookup.  Keeps the scalar fast
/// path byte-for-byte identical to legacy single-fuel behaviour: when
/// `is_constant()` returns true, callers should hit the legacy branch
/// (single `sc.element<FuelLP>(...)` lookup).
///
/// Mapping of input forms to the resolver's state:
///   * `SingleId`   → scalar; `is_constant() == true`; `constant()`
///                    returns the SingleId.  `at(s, b)` resolves the
///                    constant to a Uid (requires SystemContext-side
///                    indexing if the constant is a Name).
///   * `UidMatrix`  → inline `[stage_index][block_index]` matrix; we
///                    look up `(stage_uid → stage_index)` and
///                    `(block_uid → block_index)` via the
///                    `SimulationLP` block layout.  Out-of-range
///                    indices fall back to the (0, 0) cell — matches
///                    the broadcast semantics of the Real schedule
///                    machinery.
///   * `FileSched`  → DEFERRED in Phase 1: the per-block fuel parquet
///                    loader path is documented but not wired here.
///                    Setting a file schedule today raises a runtime
///                    error from the resolver (no silent fallback).
class OptTBSingleIdSchedResolver
{
public:
  OptTBSingleIdSchedResolver() = default;

  explicit OptTBSingleIdSchedResolver(OptTBSingleIdSched sched) noexcept
      : m_sched_(std::move(sched))
  {
  }

  [[nodiscard]] constexpr bool has_value() const noexcept
  {
    return m_sched_.has_value();
  }

  constexpr explicit operator bool() const noexcept { return has_value(); }

  /// True iff the schedule is unset OR scalar — the legacy fast path.
  [[nodiscard]] constexpr bool is_constant() const noexcept
  {
    return is_constant_single_id(m_sched_);
  }

  /// Scalar SingleId form (legacy fast path).  Returns nullopt if the
  /// schedule is unset OR not in scalar form.
  [[nodiscard]] constexpr std::optional<SingleId> constant() const noexcept
  {
    return constant_single_id(m_sched_);
  }

  /// Underlying optional variant (read-only).  Exposed so the
  /// LP-construction layer can dispatch on the active alternative
  /// directly (e.g. when wiring the `at(stage_index, block_index)`
  /// matrix lookup against the `SimulationLP` block layout).
  [[nodiscard]] constexpr const OptTBSingleIdSched& sched() const noexcept
  {
    return m_sched_;
  }

private:
  OptTBSingleIdSched m_sched_;
};

}  // namespace gtopt
