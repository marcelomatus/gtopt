/**
 * @file      integer_variable.hpp
 * @brief     Registry abstraction for every integer LP column in gtopt
 * @date      Sat May 31 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `IntegerVariable` is the runtime counterpart of `StateVariable`: a
 * single registry entry that names one integer LP column shared across
 * a group of blocks (or a stage / phase singleton).  Every producer of
 * an integer column in gtopt — `CommitmentLP` (`status`, `startup`,
 * `shutdown`, `hot_start`, `warm_start`, `cold_start`),
 * `SimpleCommitmentLP` (`status`), `ConverterLP` (`mode`, `dir`),
 * `CapacityObjectLP` (`expmod`), and future producers
 * (`Battery` on/off, `Pump` on/off, binary AGC reserves) — routes its
 * column through `SystemContext::add_integer_variable` (or
 * `add_integer_col`).
 *
 * The "no integer column without `IntegerVariable`" invariant is the
 * design contract from `docs/design/commitment-layout.md §5`.  This
 * header defines the registry types; the choke-point function lives on
 * `SystemContext`.
 *
 * Phase 0 of the commitment-layout rollout uses this header as a
 * re-plumbing layer only — producers pass through their existing scope
 * (Block when no `commitment_period`, Group when set), so the LP is
 * bit-identical to the pre-rollout master.  Phase 1 adds per-element
 * shorthand fields (`commit_period`, `commit_stride`, `commit_layout`);
 * Phase 2 adds the binding-stack resolver.  AMPL auto-registration is
 * deferred to Phase 1 — Phase 0 producers still call
 * `add_ampl_variable` explicitly.
 *
 * @see state_variable.hpp for the parallel registry pattern this file
 *      mirrors.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

/// Solver-level domain of an integer-registered LP column.
///
/// `Binary` and `Integer` are reified at LP build time as
/// `is_integer = true` on the underlying `SparseCol`.  `Relaxed` means
/// the user (or global option) asked for the LP relaxation: the column
/// stays continuous in its declared bounds.
///
/// The `Relaxed` form is what the SDDP backward pass already uses; making
/// the relaxation site explicit lets cut audits and integer-feasibility
/// checks distinguish "intentionally relaxed" from "should have been
/// integer but escaped the choke-point".
enum class IntegerDomain : std::uint8_t
{
  Binary = 0,  ///< {0, 1}
  Integer = 1,  ///< [lowb, uppb] integer
  Relaxed = 2,  ///< continuous over the declared bounds
};

/// Time-grid scope of an integer-registered LP column.
///
/// Set at the call site by the producer — the layout resolver may
/// refine WITHIN a scope (e.g. picking which named layout a `Group`
/// uses) but never crosses scopes.  A binding that targets a
/// `Phase`-scoped column with a chronological layout is rejected at
/// load time.
///
/// `Block`  — one column per (stage, block); the identity layout.
/// `Group`  — one column per (stage, group); group = contiguous run of blocks.
/// `Stage`  — one column per stage, layout-immune.
/// `Phase`  — one column per phase, layout-immune, scenario-invariant.
enum class IntegerScope : std::uint8_t
{
  Block = 0,
  Group = 1,
  Stage = 2,
  Phase = 3,
};

/// Opaque identifier for a commitment-layout group within a (stage,
/// layout) pair.  Until the binding stack lands (Phase 2) every
/// integer column is `Block` scope and `group_uid()` returns the
/// block uid; the type is reserved here so call sites and tests can
/// be authored against the long-term shape.
using GroupUid = Uid;

/// Sentinel meaning "no group" — used for `Stage` and `Phase` scopes.
constexpr GroupUid unknown_group {unknown_uid};

/// Runtime registry entry for one integer LP column.
///
/// Mirrors `StateVariable` (`include/gtopt/state_variable.hpp`):
/// inherits `LPVariable` (LPKey + ColIndex), carries a registry Key,
/// owns the (small) per-block-uid fan-out span by value.
///
/// `IntegerVariable` and `StateVariable` are orthogonal — an `expmod`
/// column may be registered as BOTH (integer AND cross-phase state),
/// at the same `LPKey` and the same `ColIndex`.  Producers that need
/// both call `add_integer_variable` and `add_state_variable`
/// independently.
class IntegerVariable : public LPVariable
{
public:
  using LPKey = gtopt::LPKey;

  /// Lookup key into the per-(scene, phase) integer-variable map.
  ///
  /// `class_name` is stored by value (`LPClassName` is a trivially
  /// copyable 72-byte aggregate carrying both PascalCase and
  /// snake_case forms — same convention as `StateVariable::Key`).
  ///
  /// `col_name` is a `std::string_view`; it must point to storage with
  /// program lifetime — in practice the `static constexpr` literals on
  /// each `*LP` class (e.g. `CommitmentLP::StatusName`).
  ///
  /// `group_uid` is `unknown_group` for `Stage` and `Phase` scopes.
  /// For `Block` scope it equals the block uid; for `Group` scope it
  /// is the group's opaque identifier from the layout table.
  struct Key
  {
    LPClassName class_name {};
    Uid element_uid {unknown_uid};
    std::string_view col_name {};
    ScenarioUid scenario_uid = unknown_uid_of<Scenario>();
    StageUid stage_uid = unknown_uid_of<Stage>();
    GroupUid group_uid {unknown_group};
    LPKey lp_key {};

    [[nodiscard]] constexpr auto operator<=>(const Key&) const noexcept =
        default;
  };

  /// Build a `Key` from concrete (scenario, stage) LP objects.
  /// Mirrors `StateVariable::key`.
  template<typename ScenarioLP, typename StageLP>
  [[nodiscard]] static auto key(const ScenarioLP& scenario,
                                const StageLP& stage,
                                LPClassName class_name,
                                Uid element_uid,
                                std::string_view col_name,
                                IntegerScope scope,
                                GroupUid group_uid = unknown_group) -> Key
  {
    if (element_uid == unknown_uid) {
      throw std::invalid_argument(
          "IntegerVariable::key: element_uid must not be unknown_uid");
    }
    // Scope-specific defaults: Stage/Phase scopes have no group; Block
    // scope's group_uid is the block uid (filled in by the caller, since
    // the block context is what knows it).
    if (scope == IntegerScope::Stage || scope == IntegerScope::Phase) {
      group_uid = unknown_group;
    }
    return Key {
        .class_name = class_name,
        .element_uid = element_uid,
        .col_name = col_name,
        .scenario_uid = scenario.uid(),
        .stage_uid = stage.uid(),
        .group_uid = group_uid,
        .lp_key = LPKey {.scene_index = scenario.scene_index(),
                         .phase_index = stage.phase_index()},
    };
  }

  /// Construct from key + col + domain/scope.  `blocks` is moved in;
  /// span accessors return views into the stored vector.
  constexpr IntegerVariable(LPKey lp_key,
                            ColIndex col,
                            IntegerDomain domain,
                            IntegerScope scope,
                            GroupUid group_uid,
                            std::vector<BlockUid> blocks) noexcept
      : LPVariable(lp_key, col)
      , m_domain_(domain)
      , m_scope_(scope)
      , m_group_uid_(group_uid)
      , m_blocks_(std::move(blocks))
  {
  }

  [[nodiscard]] constexpr auto domain() const noexcept { return m_domain_; }

  [[nodiscard]] constexpr auto scope() const noexcept { return m_scope_; }

  [[nodiscard]] constexpr auto group_uid() const noexcept
  {
    return m_group_uid_;
  }

  /// Per-block fan-out of this column.
  ///
  /// - `Block`  → single-element span containing the one block uid.
  /// - `Group`  → every block in the group.
  /// - `Stage`  → every block in the stage.
  /// - `Phase`  → **`std::nullopt`** — callers must check the optional
  ///              before iterating.  Returning an empty span instead of
  ///              `nullopt` was rejected at design time because it turns
  ///              a forgotten scope check into a silent no-op
  ///              (`docs/design/commitment-layout.md §5.2`).
  [[nodiscard]] constexpr auto blocks() const noexcept
      -> std::optional<std::span<const BlockUid>>
  {
    if (m_scope_ == IntegerScope::Phase) {
      return std::nullopt;
    }
    return std::span<const BlockUid>(m_blocks_);
  }

  /// Strict accessor: returns the per-block span unconditionally, or
  /// throws on `Phase` scope.  Use at sites that KNOW the scope is
  /// chronological — `CommitmentLP` after the layout resolver has
  /// refused Phase-scope bindings, for example.
  [[nodiscard]] auto blocks_or_throw() const -> std::span<const BlockUid>
  {
    if (m_scope_ == IntegerScope::Phase) {
      throw std::logic_error(
          "IntegerVariable::blocks_or_throw called on Phase-scope variable");
    }
    return {m_blocks_};
  }

private:
  IntegerDomain m_domain_ {IntegerDomain::Binary};
  IntegerScope m_scope_ {IntegerScope::Block};
  GroupUid m_group_uid_ {unknown_group};
  std::vector<BlockUid> m_blocks_ {};
};

}  // namespace gtopt
