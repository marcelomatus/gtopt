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
 * {"class_name": "Reservoir", "variable": "volume", "scale": 1000.0}
 * ```
 *
 * An array of these entries lives in `Options::variable_scales` and is
 * resolved at runtime by `VariableScaleMap::lookup()` with the following
 * priority:
 *
 *   1. Per-element override (matching class + variable + UID)
 *   2. Per-class default   (matching class + variable, no UID)
 *   3. Fallback default    (1.0 = no scaling)
 *
 * Per-element fields (`Battery::energy_scale`, `Reservoir::vol_scale`) and
 * global options (`Options::scale_theta`) take precedence over entries in
 * `variable_scales` — the array provides a uniform, extensible mechanism
 * for cases not covered by dedicated fields.
 */

#pragma once

#include <ranges>
#include <span>
#include <string_view>

#include <gtopt/fmap.hpp>

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
};

/**
 * @brief Lookup table for variable scales built from an array of
 * VariableScale.
 *
 * Resolution order for `lookup(class_name, variable, uid)`:
 *   1. Entry matching (class_name, variable, uid)   — per-element
 *   2. Entry matching (class_name, variable, -1)    — per-class
 *   3. Fallback: 1.0
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
      m_element_scales_[Key {.class_name = vs.class_name,
                             .variable = vs.variable,
                             .uid = vs.uid,}] = vs.scale;
    }
    for (const auto& vs : class_entries) {
      m_class_scales_[ClassKey {.class_name = vs.class_name,
                                 .variable = vs.variable,}] = vs.scale;
    }
  }

  /// Look up the scale for a (class, variable, uid) triple.
  /// Returns the most specific match, or 1.0 if none found.
  [[nodiscard]] double lookup(std::string_view class_name,
                              std::string_view variable,
                              Uid uid = unknown_uid) const noexcept
  {
    // 1. Per-element match
    if (uid != unknown_uid) {
      if (const auto it =
              m_element_scales_.find(Key {.class_name = Name {class_name},
                                          .variable = Name {variable},
                                          .uid = uid,});
          it != m_element_scales_.end())
      {
        return it->second;
      }
    }

    // 2. Per-class match
    if (const auto it = m_class_scales_.find(ClassKey {
            .class_name = Name {class_name}, .variable = Name {variable},});
        it != m_class_scales_.end())
    {
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
  /// Key for per-element scale: (class_name, variable, uid)
  struct Key
  {
    Name class_name;
    Name variable;
    Uid uid;

    auto operator<=>(const Key&) const = default;
  };

  /// Key for per-class scale: (class_name, variable)
  struct ClassKey
  {
    Name class_name;
    Name variable;

    auto operator<=>(const ClassKey&) const = default;
  };

  flat_map<Key, double> m_element_scales_;
  flat_map<ClassKey, double> m_class_scales_;
};

}  // namespace gtopt
