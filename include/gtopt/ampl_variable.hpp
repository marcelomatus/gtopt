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
 * Class name storage: `class_name` is kept as a `std::string` so that
 * registration sites can materialize the lowercase form of the
 * element's class (e.g. `as_label(lowercase(LineLP::ClassName.full_name()))`
 * → `"line"`) without worrying about the lifetime of the lowercased
 * view.  `attribute` stays as `std::string_view` because attribute
 * names come from `constexpr` class-level literals (e.g.
 * `GeneratorLP::GenerationName`).
 *
 * `block_cols` is a non-owning pointer into the element's own
 * `STBIndexHolder`; elements and the registry share the same
 * `SimulationLP` scope, so the pointer stays valid for the full solve.
 */

#pragma once

#include <cstdint>
#include <optional>
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
struct AmplVariableKey
{
  std::string class_name;  ///< e.g. "generator", "line", "battery"
  Uid element_uid {unknown_uid};
  std::string_view attribute;  ///< e.g. "generation", "flowp", "theta"
  ScenarioUid scenario_uid = unknown_uid_of<Scenario>();
  StageUid stage_uid = unknown_uid_of<Stage>();

  [[nodiscard]] friend auto operator<=>(
      const AmplVariableKey&, const AmplVariableKey&) noexcept = default;
};

/// Registry value: either a per-block column map copy (the common case)
/// or a single stage-level column used for every block.
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
  /// entry represents a stage-level variable (see `stage_col`).
  BIndexHolder<ColIndex> block_cols;

  /// Stage-level column: same value for every block in the stage.
  /// Only meaningful when `block_cols` is empty.
  ColIndex stage_col {unknown_index};

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
};

/// Map from (class_name, element_uid) to its registered element name,
/// used so that PAMPL expressions like `generator("G1")` can resolve the
/// string "G1" to its Uid.  Key is (class_name, name).
///
/// `class_name` uses `std::string` for the same reason as `AmplVariableKey`
/// (materialized lowercase view).  `element_name` is a `std::string`
/// because it comes from the element's `Id::name` field, which is
/// already owned storage.
using AmplElementNameKey = std::pair<std::string, std::string>;

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
  std::string class_name;
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
  std::string class_name;  ///< "options", "system", ...
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
  std::string class_name;
  Uid element_uid {unknown_uid};

  [[nodiscard]] friend auto operator<=>(
      const AmplMetadataKey&, const AmplMetadataKey&) noexcept = default;
};

/// Metadata registry: (class, uid) -> attribute bundle.  Populated by
/// each element's `add_to_lp` so that `collect_sum_cols` can evaluate
/// multi-predicate filters without re-reading the element's fields.
using AmplElementMetadataMap = flat_map<AmplMetadataKey, AmplElementMetadata>;

}  // namespace gtopt
