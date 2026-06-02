/**
 * @file      single_id_sched.hpp
 * @brief     TB-schedulable single-id wrapper (Phase 1 of Issue #510).
 * @date      Mon Jun  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Generator.fuel (and any future similarly-shaped FK that may need to
 * change with time) is promoted from a bare `OptSingleId` to a
 * variant that supports either:
 *   * a single SingleId  — scalar broadcast, byte-for-byte identical
 *                          to the legacy static-fuel behaviour.
 *   * a 2-D inline matrix `[[stage0_block0, stage0_block1, ...], ...]`
 *                          of `Uid` — explicit per-(stage, block)
 *                          schedule.
 *   * a FileSched (filename string) — Parquet/CSV schedule indexed
 *                          by (stage, block).  Defers materialisation
 *                          to the InputContext loader (same path as
 *                          `OptTBRealFieldSched`).
 *
 * The wrapper exposes `is_constant()` / `constant()` for the static
 * fast path (so LP wiring can short-circuit identically to today) and
 * an `at(stage_uid, block_uid)` resolver for the time-varying case.
 *
 * Phase 2 (endogenous switching, where the LP chooses fuel) is NOT
 * in scope here — this type only carries exogenous schedules.
 */

#pragma once

#include <optional>
#include <variant>
#include <vector>

#include <gtopt/field_sched.hpp>  // FileSched
#include <gtopt/single_id.hpp>

namespace gtopt
{

/// Inline 2-D matrix of Uid indexed by `[stage_index][block_index]`.
using UidMatrix = std::vector<std::vector<Uid>>;

/// TB-schedulable SingleId — flat variant of
/// `(Uid | Name | UidMatrix)`.
///
/// The first two alternatives are the legacy scalar form (mirrors the
/// `SingleId` variant); a JSON `42` pins `Uid`, a JSON `"gas"` pins
/// `Name`.  The third carries the per-(stage, block) inline matrix.
///
/// **Phase 1 scope note**: the issue text lists a fourth `FileSched`
/// alternative for Parquet/CSV file-backed schedules.  That path is
/// DEFERRED — daw::json's variant dispatcher cannot disambiguate two
/// string-valued alternatives by token type, so combining `Name` and
/// `FileSched` requires a tagged-string wrapper that we have not yet
/// landed.  Neither converter (plp2gtopt / plexos2gtopt) currently
/// emits a fuel-schedule file path, so the limitation is invisible to
/// current callers.  See Phase 2 / follow-up issue for the wiring.
using TBSingleIdSched = std::variant<Uid, Name, UidMatrix>;

/// Optional `TBSingleIdSched` — JSON-null-aware.
using OptTBSingleIdSched = std::optional<TBSingleIdSched>;

/// Variant alternative indices (kept in lock-step with
/// `TBSingleIdSched` and `jvtl_TBSingleIdSched`).
namespace tb_single_id_sched
{
inline constexpr std::size_t kUid = 0;
inline constexpr std::size_t kName = 1;
inline constexpr std::size_t kMatrix = 2;
}  // namespace tb_single_id_sched

/// Returns true iff `sched` is in the scalar `Uid` or `Name` form.
/// When true, the LP fast path takes the legacy static-fuel branch
/// and emits byte-identical rows to a Phase-0 / single-fuel generator.
[[nodiscard]] constexpr bool is_constant_single_id(
    const TBSingleIdSched& sched) noexcept
{
  return sched.index() == tb_single_id_sched::kUid
      || sched.index() == tb_single_id_sched::kName;
}

/// Returns true iff the optional schedule is empty OR holds a scalar
/// SingleId.  Used by callers (e.g. emission expansion) to keep the
/// single-fuel path byte-identical.
[[nodiscard]] constexpr bool is_constant_single_id(
    const OptTBSingleIdSched& sched) noexcept
{
  return !sched.has_value() || is_constant_single_id(*sched);
}

/// Extracts the scalar SingleId iff `sched` is in the constant form.
/// Returns `std::nullopt` for the matrix / file forms.
[[nodiscard]] inline std::optional<SingleId> constant_single_id(
    const TBSingleIdSched& sched) noexcept
{
  if (const auto* u = std::get_if<tb_single_id_sched::kUid>(&sched)) {
    return SingleId {*u};
  }
  if (const auto* n = std::get_if<tb_single_id_sched::kName>(&sched)) {
    return SingleId {*n};
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<SingleId> constant_single_id(
    const OptTBSingleIdSched& sched) noexcept
{
  if (!sched.has_value()) {
    return std::nullopt;
  }
  return constant_single_id(*sched);
}

}  // namespace gtopt
