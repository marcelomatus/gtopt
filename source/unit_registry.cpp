/**
 * @file      unit_registry.cpp
 * @brief     Implementation of UnitRegistry — see header for the design.
 */

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <daw/daw_read_file.h>
#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/unit_registry.hpp>
#include <gtopt/units_builtin.hpp>
#include <spdlog/spdlog.h>

namespace gtopt::unit_registry_detail
{

/// One JSON entry as it appears in `unit_dialects.json::units[]`.
struct UnitEntry
{
  std::string klass;
  std::string canonical;
  std::string dialect;
  std::string unit;
};

/// Top-level document shape.
struct UnitsFile
{
  int version = 0;
  std::string doc;
  std::vector<UnitEntry> units;
};

}  // namespace gtopt::unit_registry_detail

namespace daw::json
{

template<>
struct json_data_contract<gtopt::unit_registry_detail::UnitEntry>
{
  using type = json_member_list<json_string<"class", std::string>,
                                json_string<"canonical", std::string>,
                                json_string<"dialect", std::string>,
                                json_string<"unit", std::string>>;

  constexpr static auto to_json_data(
      gtopt::unit_registry_detail::UnitEntry const& e)
  {
    return std::forward_as_tuple(e.klass, e.canonical, e.dialect, e.unit);
  }
};

template<>
struct json_data_contract<gtopt::unit_registry_detail::UnitsFile>
{
  using type = json_member_list<
      json_number<"version", int>,
      json_string_null<"doc", std::string>,
      json_array<"units",
                 gtopt::unit_registry_detail::UnitEntry,
                 std::vector<gtopt::unit_registry_detail::UnitEntry>>>;

  constexpr static auto to_json_data(
      gtopt::unit_registry_detail::UnitsFile const& f)
  {
    return std::forward_as_tuple(f.version, f.doc, f.units);
  }
};

}  // namespace daw::json

namespace gtopt
{

void UnitRegistry::build_from_json(std::string_view json_content)
{
  using unit_registry_detail::UnitsFile;
  UnitsFile file;
  try {
    file = daw::json::from_json<UnitsFile>(json_content, StrictParsePolicy);
  } catch (const std::exception& ex) {
    throw std::runtime_error(
        std::format("unit_registry: parse error: {}", ex.what()));
  }

  if (file.version != 1) {
    throw std::runtime_error(std::format(
        "unit_registry: unsupported version {} (expected 1)", file.version));
  }

  std::size_t skipped_dupes = 0;
  for (auto& entry : file.units) {
    // Skip entries with empty class / canonical / dialect — those are
    // structurally meaningless and would collide on the empty-string
    // key.  Empty `unit` is fine (= "dimensionless / unitless").
    if (entry.klass.empty() || entry.canonical.empty() || entry.dialect.empty())
    {
      continue;
    }
    auto [it, inserted] = m_units_.emplace(Key {std::move(entry.klass),
                                                std::move(entry.canonical),
                                                std::move(entry.dialect)},
                                           std::move(entry.unit));
    if (!inserted) {
      ++skipped_dupes;
    }
  }

  spdlog::debug(
      "unit_registry: loaded {} (class, canonical, dialect) entries "
      "(skipped {} dupes)",
      m_units_.size(),
      skipped_dupes);
}

UnitRegistry::UnitRegistry(std::string_view json_content)
{
  build_from_json(json_content);
}

UnitRegistry::UnitRegistry(const std::filesystem::path& json_path)
{
  const auto result = daw::read_file(json_path.string());
  if (!result) {
    throw std::runtime_error(std::format(
        "unit_registry: cannot read units file '{}'", json_path.string()));
  }
  build_from_json(result.value());
  m_source_path_ = json_path;
}

std::optional<std::string_view> UnitRegistry::unit_for(
    std::string_view class_name,
    std::string_view canonical,
    std::string_view dialect) const noexcept
{
  const auto it = m_units_.find(Key {std::string {class_name},
                                     std::string {canonical},
                                     std::string {dialect}});
  if (it == m_units_.end()) {
    return std::nullopt;
  }
  return std::string_view {it->second};
}

std::optional<std::string_view> UnitRegistry::class_agnostic_unit_for(
    std::string_view canonical, std::string_view dialect) const noexcept
{
  // Walk every entry (small dictionary, O(N) is fine).  Two passes
  // would also work but a single sweep with an early-divergence break
  // is simpler.  Treat the empty string as a real value: a canonical
  // that is dimensionless in one dialect but kV in another is a
  // mismatch, not "compatible".
  std::optional<std::string_view> found;
  for (const auto& [key, unit] : m_units_) {
    const auto& [k_class, k_canonical, k_dialect] = key;
    if (k_canonical != canonical || k_dialect != dialect) {
      continue;
    }
    const std::string_view u {unit};
    if (!found.has_value()) {
      found = u;
    } else if (*found != u) {
      // Distinct classes disagree on the unit for this canonical name —
      // caller cannot resolve without class context.
      return std::nullopt;
    }
  }
  return found;
}

std::optional<std::filesystem::path> find_units_file()
{
  namespace fs = std::filesystem;

  // 1. Environment override.
  if (const char* env = std::getenv("GTOPT_UNIT_DIALECTS_FILE"); env != nullptr)
  {
    fs::path p {env};
    if (fs::exists(p)) {
      return p;
    }
    spdlog::warn(
        "unit_registry: $GTOPT_UNIT_DIALECTS_FILE='{}' set but path does "
        "not exist; falling back to default search",
        env);
  }

  // 2. Absolute install path baked at configure time.
  if (!kInstalledUnitsFilePath.empty()) {
    fs::path p {std::string {kInstalledUnitsFilePath}};
    if (fs::exists(p)) {
      return p;
    }
  }

  // 3. In-tree source path (development / out-of-source builds).
  if (!kSourceUnitsFilePath.empty()) {
    fs::path p {std::string {kSourceUnitsFilePath}};
    if (fs::exists(p)) {
      return p;
    }
  }

  // 4. Caller should fall back to compiled-in JSON.
  return std::nullopt;
}

const UnitRegistry& UnitRegistry::instance()
{
  static const UnitRegistry registry = []() -> UnitRegistry
  {
    if (const auto path = find_units_file(); path.has_value()) {
      try {
        UnitRegistry r {*path};
        spdlog::debug("unit_registry: loaded from '{}'", path->string());
        return r;
      } catch (const std::exception& ex) {
        spdlog::warn(
            "unit_registry: failed to load '{}': {} — using "
            "compiled-in fallback",
            path->string(),
            ex.what());
      }
    }
    try {
      return UnitRegistry {kBuiltinUnitsJson};
    } catch (const std::exception& ex) {
      spdlog::error(
          "unit_registry: compiled-in fallback also failed to parse: {} "
          "— disabling unit-aware diagnostics",
          ex.what());
      // Empty registry: every unit_for() returns nullopt and no
      // unit-driven behaviour fires (graceful degradation to the
      // pre-feature behaviour).
      return UnitRegistry {std::string_view {R"({"version": 1, "units": []})"}};
    }
  }();
  return registry;
}

}  // namespace gtopt
