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

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/sparse_col.hpp>

namespace gtopt
{

/// Key identifying a PAMPL-visible variable at a specific
/// (element, attribute, scenario, stage) granularity.
struct AmplVariableKey
{
  std::string class_name;  ///< e.g. "generator", "line", "battery"
  Uid element_uid {unknown_uid};
  std::string_view attribute;  ///< e.g. "generation", "flowp", "theta"
  ScenarioUid scenario_uid {unknown_uid};
  StageUid stage_uid {unknown_uid};

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

}  // namespace gtopt
