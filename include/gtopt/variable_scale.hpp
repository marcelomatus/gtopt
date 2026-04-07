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
 * Per-element fields (`Battery::energy_scale`, `Reservoir::energy_scale`) take
 * precedence over entries in `variable_scales`.
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
  Name class_name {};  ///< Element class (e.g. "Bus", "Reservoir", "Battery")
  Name variable {};  ///< Variable name (e.g. "theta", "volume", "energy")
  Uid uid {unknown_uid};  ///< Element UID (unknown_uid = all elements)
  Real scale {1.0};  ///< physical = LP × scale
  Name name {};  ///< Element name (informational, not used for lookup)
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
      const ElementKeyView kv {
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
    const ClassKeyView kv {
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

  /// Owning key stored in the map (holds std::string).
  struct ElementKey
  {
    std::string class_name;
    std::string variable;
    Uid uid;

    bool operator==(const ElementKey&) const = default;
  };

  /// Non-owning view key used for lookup (holds string_view).
  struct ElementKeyView
  {
    std::string_view class_name;
    std::string_view variable;
    Uid uid;
  };

  struct ElementHash
  {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(const ElementKey& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      h = hash_combine(h, std::hash<Uid> {}(k.uid));
      return h;
    }

    [[nodiscard]] size_t operator()(const ElementKeyView& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      h = hash_combine(h, std::hash<Uid> {}(k.uid));
      return h;
    }
  };

  struct ElementEqual
  {
    using is_transparent = void;

    [[nodiscard]] bool operator()(const ElementKey& a,
                                  const ElementKey& b) const noexcept
    {
      return a == b;
    }

    [[nodiscard]] bool operator()(const ElementKeyView& a,
                                  const ElementKey& b) const noexcept
    {
      return a.class_name == b.class_name && a.variable == b.variable
          && a.uid == b.uid;
    }

    [[nodiscard]] bool operator()(const ElementKey& a,
                                  const ElementKeyView& b) const noexcept
    {
      return a.class_name == b.class_name && a.variable == b.variable
          && a.uid == b.uid;
    }
  };

  // ── Per-class key: (class_name, variable) ───────────────────────────

  /// Owning key stored in the map.
  struct ClassKey
  {
    std::string class_name;
    std::string variable;

    bool operator==(const ClassKey&) const = default;
  };

  /// Non-owning view key used for lookup.
  struct ClassKeyView
  {
    std::string_view class_name;
    std::string_view variable;
  };

  struct ClassHash
  {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(const ClassKey& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      return h;
    }

    [[nodiscard]] size_t operator()(const ClassKeyView& k) const noexcept
    {
      auto h = std::hash<std::string_view> {}(k.class_name);
      h = hash_combine(h, std::hash<std::string_view> {}(k.variable));
      return h;
    }
  };

  struct ClassEqual
  {
    using is_transparent = void;

    [[nodiscard]] bool operator()(const ClassKey& a,
                                  const ClassKey& b) const noexcept
    {
      return a == b;
    }

    [[nodiscard]] bool operator()(const ClassKeyView& a,
                                  const ClassKey& b) const noexcept
    {
      return a.class_name == b.class_name && a.variable == b.variable;
    }

    [[nodiscard]] bool operator()(const ClassKey& a,
                                  const ClassKeyView& b) const noexcept
    {
      return a.class_name == b.class_name && a.variable == b.variable;
    }
  };

  std::unordered_map<ElementKey, double, ElementHash, ElementEqual>
      m_element_scales_;
  std::unordered_map<ClassKey, double, ClassHash, ClassEqual> m_class_scales_;
};

}  // namespace gtopt
