/**
 * @file      ampl_variable.hpp
 * @brief     Registry types for user-constraint (AMPL) variable resolution
 * @date      Wed Apr  9 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each LP element registers its PAMPL-visible columns via
 * `SystemContext::add_ampl_variable` at `add_to_lp` time.  The
 * user-constraint resolver then looks them up generically through
 * `find_ampl_col`, replacing the former element-type dispatch chain in
 * element_column_resolver.cpp.
 *
 * Design mirrors `StateVariable` / `SimulationLP::add_state_variable`.
 *
 * Class name storage: `class_name` is `std::string_view` pointing
 * into each `LPClassName::snake_case()` constexpr buffer — program-
 * lifetime storage, so no allocation on map lookups.  `attribute` is
 * likewise `std::string_view` because attribute names come from
 * `constexpr` class-level literals (e.g.
 * `GeneratorLP::GenerationName`).
 *
 * `block_cols` is a non-owning pointer into the element's own
 * `STBIndexHolder`; elements and the registry share the same
 * `SimulationLP` scope, so the pointer stays valid for the full solve.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/sparse_col.hpp>

namespace gtopt
{

/// Tag on each registered AMPL variable distinguishing ordinary LP
/// columns from state-backed columns that bridge phases via the
/// state-variable subsystem.  The user-constraint DSL will use this in
/// Phase 2 to validate `state(...)` wrappers: the parser asserts
/// authorial intent by requiring `state(elem.efin)` and the resolver
/// checks that the referenced entry's `kind == StateBacked`.
///
/// Default is `Regular`: elements that want the state-backed tag call
/// `SystemContext::add_ampl_state_variable` (same shape as
/// `add_ampl_variable`) once the Phase 2 API lands.
enum class AmplVariableKind : std::uint8_t
{
  Regular,  ///< Ordinary LP column scoped to this (scene, phase)
  StateBacked,  ///< Also a StateVariable — cross-phase bridge
};

/// Key identifying a PAMPL-visible variable at a specific
/// (element, attribute, scenario, stage) granularity.
///
/// `class_name` is a `std::string_view` into the constexpr
/// `LPClassName::snake_case()` buffer — program-lifetime storage,
/// so no allocation is needed for map lookups.
struct AmplVariableKey
{
  std::string_view class_name;  ///< e.g. "generator", "line", "battery"
  Uid element_uid {unknown_uid};
  std::string_view attribute;  ///< e.g. "generation", "flowp", "theta"
  ScenarioUid scenario_uid = unknown_uid_of<Scenario>();
  StageUid stage_uid = unknown_uid_of<Stage>();

  [[nodiscard]] friend auto operator<=>(
      const AmplVariableKey&, const AmplVariableKey&) noexcept = default;
};

/// Registry value: one of three shapes for the registered attribute —
///  - **Per-block single col** (the common case): `block_cols`.
///  - **Stage-level single col**: `stage_col`, broadcast to every block.
///  - **Per-block sum of cols**: `block_cols_sum`.  The attribute
///    resolves to `Σ cols` per block — used by the line PWL-direct mode
///    where `flowp` / `flown` are the sum of per-segment LP cols rather
///    than an aggregator column.  See `LineLP::add_to_lp` for the
///    piecewise_direct registration site, and
///    `element_column_resolver.cpp::resolve_col_to_row` for the
///    stamping path that expands a sum-leg into one `row[col] +=
///    base · coef` per element.
///
/// Exactly one of `block_cols`, `stage_col`, `block_cols_sum` is
/// populated per registration (asserted by construction in the
/// `add_ampl_variable` overloads).
///
/// The per-block map is stored **by value** because elements hold their
/// `STBIndexHolder`s (flat_map of BIndexHolder) and adding new entries
/// invalidates any pointers into older values.  In SDDP, `add_to_lp` is
/// called once per (scenario, stage) pair, so later insertions grow the
/// outer flat_map and reallocate its storage — any raw pointer cached
/// here from an earlier call would dangle.  Copying the `BIndexHolder`
/// by value into `AmplVariable` sidesteps the lifetime problem entirely;
/// the indices inside are small POD so copies are cheap.
struct AmplVariable
{
  /// Per-block column map for this (scenario, stage).  Empty when this
  /// entry represents a stage-level variable (see `stage_col`) or a
  /// per-block sum-of-cols (see `block_cols_sum`).
  BIndexHolder<ColIndex> block_cols;

  /// Stage-level column: same value for every block in the stage.
  /// Only meaningful when `block_cols` and `block_cols_sum` are empty.
  ColIndex stage_col {unknown_index};

  /// Per-block sum-of-cols.  When this map has an entry for a block,
  /// the attribute resolves to the **sum** of those columns (each
  /// stamped with the leg coefficient by the resolver).  Used for
  /// virtual aggregators like `line.flowp = Σ flowp_seg_k` in
  /// `piecewise_direct` line-loss mode — no LP col is created for the
  /// aggregator, the existing segment cols are summed at resolution
  /// time.  Empty in the common single-col case.
  BIndexHolder<std::vector<ColIndex>> block_cols_sum;

  /// Optional per-block additive offset (Option C demand substitution
  /// and similar shifted-variable encodings).  When set, the AMPL
  /// resolver reports `physical_value = LP_value × scale + offset(b)`
  /// — and the user-constraint row builder folds the offset into the
  /// row's RHS via the existing `param_shift` accumulator.  Empty when
  /// no shift is registered (the common case).
  BIndexHolder<double> block_offsets;

  /// Regular LP column or state-backed column.  Default is Regular;
  /// state-backed entries will be set by the Phase 2
  /// `add_ampl_state_variable` API.
  AmplVariableKind kind {AmplVariableKind::Regular};

  [[nodiscard]] std::optional<ColIndex> col_at(
      BlockUid block_uid) const noexcept
  {
    if (!block_cols.empty()) {
      if (const auto it = block_cols.find(block_uid); it != block_cols.end()) {
        return it->second;
      }
      return std::nullopt;
    }
    if (stage_col != ColIndex {unknown_index}) {
      return stage_col;
    }
    return std::nullopt;
  }

  /// Look up the per-block additive offset.  Returns `0.0` when this
  /// registration has no offset table or no entry for this block — the
  /// expected case for every element except those using a shifted-
  /// variable encoding (Option C demand-fail).
  [[nodiscard]] double offset_at(BlockUid block_uid) const noexcept
  {
    if (block_offsets.empty()) {
      return 0.0;
    }
    if (const auto it = block_offsets.find(block_uid);
        it != block_offsets.end())
    {
      return it->second;
    }
    return 0.0;
  }

  /// Look up the per-block sum-of-cols list for this block.  Returns an
  /// empty span when this registration is not a sum-of-cols entry, or
  /// when the block has no list entry.  Callers prefer the single-col
  /// `col_at` path; this is the fall-through for virtual aggregators.
  [[nodiscard]] std::span<const ColIndex> cols_at(
      BlockUid block_uid) const noexcept
  {
    if (block_cols_sum.empty()) {
      return {};
    }
    const auto it = block_cols_sum.find(block_uid);
    if (it == block_cols_sum.end()) {
      return {};
    }
    return std::span<const ColIndex> {it->second};
  }
};

/// Map from (class_name, element_uid) to its registered element name,
/// used so that PAMPL expressions like `generator("G1")` can resolve the
/// string "G1" to its Uid.  Key is (class_name, name).
///
/// Both views are non-owning: `class_name` points into constexpr
/// `LPClassName::snake_case()` (program lifetime); `element_name`
/// points into the element's `Id::name` (owned by the `System`,
/// which outlives the registry).
using AmplElementNameKey = std::pair<std::string_view, std::string_view>;

/// Variable registry: (class_name, uid, attribute, scenario, stage) -> cols.
using AmplVariableMap = flat_map<AmplVariableKey, AmplVariable>;

/// Name registry: (class_name, name) -> element uid.
using AmplElementNameMap = flat_map<AmplElementNameKey, Uid>;

// ── Compound attributes ─────────────────────────────────────────────────────

/// One leg of a compound PAMPL attribute.  The compound is the sum
/// `Σ coefficient * <source_attribute>` where each `source_attribute` is
/// itself a registered `AmplVariable` of the same element.
///
/// Example: `line.flow = (+1, "flowp"), (-1, "flown")`.
struct AmplCompoundLeg
{
  double coefficient {1.0};
  std::string_view source_attribute;  ///< must be a registered AMPL attribute
};

/// Key identifying a class-level compound attribute.  Compounds are
/// purely syntactic sugar over existing AMPL columns, so they are
/// indexed by class + compound name only (not per-element).
struct AmplCompoundKey
{
  std::string_view class_name;
  std::string_view compound_name;

  [[nodiscard]] friend auto operator<=>(
      const AmplCompoundKey&, const AmplCompoundKey&) noexcept = default;
};

/// Compound-attribute registry: (class, compound_name) -> legs.
using AmplCompoundMap = flat_map<AmplCompoundKey, std::vector<AmplCompoundLeg>>;

// ── Scalar parameter registry (Phase 1d) ────────────────────────────────────

/// Key identifying a class-level scalar parameter.  "Singleton classes"
/// like `options` and `system` expose globally-scoped read-only constants
/// that PAMPL constraints may reference as `options.scale_objective`,
/// `system.n_stages`, etc.  No element_uid, no (scenario, stage) tuple
/// — the value is the same for every row.
struct AmplScalarKey
{
  std::string_view class_name;  ///< "options", "system", ...
  std::string_view attribute;  ///< "annual_discount_rate", "scale_objective"

  [[nodiscard]] friend auto operator<=>(
      const AmplScalarKey&, const AmplScalarKey&) noexcept = default;
};

/// Scalar registry: (class, attribute) -> resolved double value.
///
/// Values are cached at registration time (once per `SimulationLP` under
/// `std::call_once`) so the resolver path is a pure read.  Options are
/// immutable for the lifetime of a `SimulationLP`, which is what makes
/// the cache-by-value approach safe.
using AmplScalarMap = flat_map<AmplScalarKey, double>;

// ── Element metadata registry (F9) ──────────────────────────────────────────

/// Value stored against a metadata key.  Strings for categorical
/// attributes (`type`, `bus`, `name`, `class_name`, …), doubles for
/// numeric data (`uid`, `cap`, …).
using AmplMetadataValue = std::variant<std::string, double>;

/// Metadata bundle for one element: an ordered `(key, value)` list.
/// Small enough that linear search beats a map.
using AmplElementMetadata =
    std::vector<std::pair<std::string_view, AmplMetadataValue>>;

/// Lookup key for the per-element metadata registry.
struct AmplMetadataKey
{
  std::string_view class_name;
  Uid element_uid {unknown_uid};

  [[nodiscard]] friend auto operator<=>(
      const AmplMetadataKey&, const AmplMetadataKey&) noexcept = default;
};

/// Metadata registry: (class, uid) -> attribute bundle.  Populated by
/// each element's `add_to_lp` so that `collect_sum_cols` can evaluate
/// multi-predicate filters without re-reading the element's fields.
using AmplElementMetadataMap = flat_map<AmplMetadataKey, AmplElementMetadata>;

}  // namespace gtopt
