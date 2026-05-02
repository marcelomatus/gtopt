/**
 * @file      variable_scale.hpp
 * @brief     VariableScale struct and VariableScaleMap for LP variable scaling
 * @date      Mon Mar 17 04:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Provides a JSON-configurable mechanism for defining LP variable scale
 * factors per element class, variable name, and optional element UID.
 *
 * The `VariableScale` struct defines a single scale entry:
 * ```json
 * {"class_name": "Reservoir", "variable": "energy", "scale": 1000.0}
 * ```
 *
 * An array of these entries lives in `PlanningOptions::variable_scales` and is
 * resolved at runtime by `VariableScaleMap::lookup()` with the following
 * priority:
 *
 *   1. Per-element override (matching class + variable + UID)
 *   2. Per-class default   (matching class + variable, no UID)
 *   3. Fallback default    (1.0 = no scaling)
 *
 * All LP variable scaling is configured exclusively via `variable_scales`;
 * there are no per-element `energy_scale` fields.
 *
 * Global options (`scale_theta`, `scale_alpha`) are auto-injected into the
 * `variable_scales` array by `PlanningOptionsLP`.  All scales follow the
 * same convention `physical = LP × scale`:
 *   - `Bus.theta`:  scale_theta = 1e-4  (theta ~0.01 rad → LP ~100)
 *   - `Sddp.alpha`: scale_alpha = 1e7   (alpha ~1e8 $ → LP ~10)
 *   - `Reservoir.energy`: energy_scale = 1000 (energy ~1e6 → LP ~1000)
 *
 * Users can override per-element (UID-specific) entries in `variable_scales`
 * for fine-grained control.
 */

#pragma once

#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @brief Scale definition for an LP variable.
 *
 * Convention: `physical_value = LP_value × scale`.
 *
 * When `uid` is set, the scale applies only to the element with that UID.
 * When `uid` is absent, the scale applies to all elements of the given class.
 */
struct VariableScale
{
  /// Element class (e.g. `"Bus"`, `"Reservoir"`).  Stored as a
  /// `std::string_view` so call sites can assign canonical constexpr
  /// constants (e.g. `system_class_name`, `BusLP::ClassName.full_name()`)
  /// without an intermediate string copy.  When populated by JSON
  /// deserialisation, the underlying buffer is the parser's
  /// daw::json owning string pool (kept alive by `Planning`'s
  /// `m_json_strings_` arena).  `VariableScaleMap` copies the view
  /// into an owning `std::string` key on construction.
  std::string_view class_name {};
  std::string_view variable {};  ///< Variable name (e.g. "theta", "energy")
  Uid uid {unknown_uid};  ///< Element UID (unknown_uid = all elements)
  Real scale {1.0};  ///< physical = LP × scale
  std::string_view name {};  ///< Element name (informational)
};

/**
 * @brief Lookup table for variable scales built from an array of
 * VariableScale.
 *
 * Resolution order for `lookup(class_name, variable, uid)`:
 *   1. Entry matching (class_name, variable, uid)   — per-element
 *   2. Entry matching (class_name, variable, -1)    — per-class
 *   3. Fallback: 1.0
 *
 * Optimized for millions of lookups with very few insertions.
 * Uses `std::unordered_map` with transparent hashing to avoid
 * temporary string allocations on every lookup.
 */
class VariableScaleMap
{
public:
  VariableScaleMap() = default;

  explicit VariableScaleMap(std::span<const VariableScale> scales)
  {
    // Partition into per-element and per-class entries using ranges
    auto element_entries = scales
        | std::views::filter([](const auto& vs)
                             { return vs.uid != unknown_uid; });
    auto class_entries = scales
        | std::views::filter([](const auto& vs)
                             { return vs.uid == unknown_uid; });

    // ElementKey / ClassKey hold `std::string_view` keys backed by the
    // source `VariableScale` objects (which in turn reference either
    // static-lifetime constexpr constants or the JSON parser's stable
    // string pool).  Caller must keep the source alive for the map's
    // lifetime — same contract as `std::string_view` keys in
    // `linear_problem.cpp:679`.
    for (const auto& vs : element_entries) {
      m_element_scales_.emplace(
          ElementKey {
              .class_name = vs.class_name,
              .variable = vs.variable,
              .uid = vs.uid,
          },
          vs.scale);
    }
    for (const auto& vs : class_entries) {
      m_class_scales_.emplace(
          ClassKey {
              .class_name = vs.class_name,
              .variable = vs.variable,
          },
          vs.scale);
    }
  }

  /// Look up the scale for a (class, variable, uid) triple.
  /// Returns the most specific match, or 1.0 if none found.
  /// No temporary string allocations — uses transparent hash lookup.
  [[nodiscard]] double lookup(std::string_view class_name,
                              std::string_view variable,
                              Uid uid = unknown_uid) const noexcept
  {
    // 1. Per-element match
    if (uid != unknown_uid) {
      const ElementKey kv {
          .class_name = class_name,
          .variable = variable,
          .uid = uid,
      };
      if (const auto it = m_element_scales_.find(kv);
          it != m_element_scales_.end())
      {
        return it->second;
      }
    }

    // 2. Per-class match
    const ClassKey kv {
        .class_name = class_name,
        .variable = variable,
    };
    if (const auto it = m_class_scales_.find(kv); it != m_class_scales_.end()) {
      return it->second;
    }

    // 3. Default
    return 1.0;
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return m_element_scales_.empty() && m_class_scales_.empty();
  }

private:
  // ── Hash helper: combine hashes ─────────────────────────────────────
  static constexpr size_t hash_combine(size_t h1, size_t h2) noexcept
  {
    // boost::hash_combine formula
    return h1 ^ (h2 + size_t {0x9e3779b9} + (h1 << 6U) + (h1 >> 2U));
  }

  // ── Per-element key: (class_name, variable, uid) ────────────────────

  /// Map key (non-owning).  Backed by the source `VariableScale`'s
  /// `string_view` fields — caller must keep the source alive for the
  /// map's lifetime.
  struct ElementKey
  {
    std::string_view class_name;
    std::string_view variable;
    Uid uid;

    bool operator==(const ElementKey&) const = default;
  };

  struct ElementHash
  {
    [[nodiscard]] size_t operator()(const ElementKey& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      h = hash_combine(h, std::hash<Uid> {}(k.uid));
      return h;
    }
  };

  struct ElementEqual
  {
    [[nodiscard]] bool operator()(const ElementKey& a,
                                  const ElementKey& b) const noexcept
    {
      return a == b;
    }
  };

  // ── Per-class key: (class_name, variable) ───────────────────────────

  /// Map key (non-owning).  Same lifetime contract as `ElementKey`.
  struct ClassKey
  {
    std::string_view class_name;
    std::string_view variable;

    bool operator==(const ClassKey&) const = default;
  };

  struct ClassHash
  {
    [[nodiscard]] size_t operator()(const ClassKey& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      return h;
    }
  };

  struct ClassEqual
  {
    [[nodiscard]] bool operator()(const ClassKey& a,
                                  const ClassKey& b) const noexcept
    {
      return a == b;
    }
  };

  std::unordered_map<ElementKey, double, ElementHash, ElementEqual>
      m_element_scales_;
  std::unordered_map<ClassKey, double, ClassHash, ClassEqual> m_class_scales_;
};

}  // namespace gtopt
