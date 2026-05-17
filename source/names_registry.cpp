/**
 * @file      names_registry.cpp
 * @brief     Implementation of the runtime-loadable naming dictionary
 * @copyright BSD-3-Clause
 */

#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <daw/daw_read_file.h>
#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/names_builtin.hpp>
#include <gtopt/names_registry.hpp>
#include <spdlog/spdlog.h>

namespace gtopt::names_registry_detail
{

/// One row of `share/gtopt/names.json` -> `aliases[]`.
///
/// Lives at namespace scope (not anonymous) so the
/// `daw::json::json_data_contract` specializations below can bind to
/// it across translation units.
struct NameAliasEntry
{
  std::string klass;  // "class" field (renamed to avoid C++ keyword)
  std::string canonical;
  std::string alt;
  std::string dialect;
};

/// The whole file. We only consume `aliases`; the `version` and `doc`
/// fields are accepted (so strict parsing doesn't reject them) but
/// otherwise ignored at runtime.
struct NamesFile
{
  int version = 0;
  std::string doc;
  std::vector<NameAliasEntry> aliases;
};

}  // namespace gtopt::names_registry_detail

namespace daw::json
{

template<>
struct json_data_contract<gtopt::names_registry_detail::NameAliasEntry>
{
  using type = json_member_list<json_string<"class", std::string>,
                                json_string<"canonical", std::string>,
                                json_string<"alt", std::string>,
                                json_string_null<"dialect", std::string>>;

  constexpr static auto to_json_data(
      gtopt::names_registry_detail::NameAliasEntry const& e)
  {
    return std::forward_as_tuple(e.klass, e.canonical, e.alt, e.dialect);
  }
};

template<>
struct json_data_contract<gtopt::names_registry_detail::NamesFile>
{
  using type = json_member_list<
      json_number<"version", int>,
      json_string_null<"doc", std::string>,
      json_array<"aliases",
                 gtopt::names_registry_detail::NameAliasEntry,
                 std::vector<gtopt::names_registry_detail::NameAliasEntry>>>;

  constexpr static auto to_json_data(
      gtopt::names_registry_detail::NamesFile const& f)
  {
    return std::forward_as_tuple(f.version, f.doc, f.aliases);
  }
};

}  // namespace daw::json

namespace gtopt
{

void NamesRegistry::check_alias_uniqueness(std::string_view alias,
                                           std::string_view canonical) const
{
  if (const auto it = m_alias_to_canonical_.find(std::string {alias});
      it != m_alias_to_canonical_.end())
  {
    if (it->second != canonical) {
      throw std::runtime_error(
          std::format("names.json: alias '{}' is registered for two "
                      "different canonical names: '{}' and '{}'",
                      alias,
                      it->second,
                      canonical));
    }
  }
}

void NamesRegistry::build_from_json(std::string_view json_content)
{
  // Parse the flat-list dictionary.  Use the standard StrictParsePolicy
  // so unknown fields surface as an error during loader development.
  using names_registry_detail::NamesFile;
  NamesFile file;
  try {
    file = daw::json::from_json<NamesFile>(json_content, StrictParsePolicy);
  } catch (const std::exception& ex) {
    throw std::runtime_error(
        std::format("names.json: parse error: {}", ex.what()));
  }

  if (file.version != 1) {
    throw std::runtime_error(std::format(
        "names.json: unsupported version {} (expected 1)", file.version));
  }

  std::size_t skipped_dupes = 0;
  for (const auto& entry : file.aliases) {
    if (entry.canonical.empty() || entry.alt.empty()) {
      continue;
    }
    if (entry.alt == entry.canonical) {
      continue;
    }
    check_alias_uniqueness(entry.alt, entry.canonical);
    auto [it, inserted] =
        m_alias_to_canonical_.emplace(entry.alt, entry.canonical);
    if (!inserted) {
      ++skipped_dupes;
      continue;
    }
    m_canonical_to_aliases_[entry.canonical].emplace_back(entry.alt);
  }

  spdlog::debug("names_registry: loaded {} aliases (skipped {} duplicate rows)",
                m_alias_to_canonical_.size(),
                skipped_dupes);
}

NamesRegistry::NamesRegistry(std::string_view json_content)
{
  build_from_json(json_content);
}

NamesRegistry::NamesRegistry(const std::filesystem::path& json_path)
{
  const auto result = daw::read_file(json_path.string());
  if (!result) {
    throw std::runtime_error(std::format(
        "names_registry: cannot read names file '{}'", json_path.string()));
  }
  build_from_json(result.value());
  m_source_path_ = json_path;
}

std::optional<std::string_view> NamesRegistry::canonical_for(
    std::string_view alias) const noexcept
{
  const auto it = m_alias_to_canonical_.find(std::string {alias});
  if (it == m_alias_to_canonical_.end()) {
    return std::nullopt;
  }
  return std::string_view {it->second};
}

std::optional<std::filesystem::path> find_names_file()
{
  namespace fs = std::filesystem;

  // 1. Environment override.
  if (const char* env = std::getenv("GTOPT_NAMES_FILE"); env != nullptr) {
    fs::path p {env};
    if (fs::exists(p)) {
      return p;
    }
    spdlog::warn(
        "names_registry: $GTOPT_NAMES_FILE='{}' set but path does not "
        "exist; falling back to default search",
        env);
  }

  // 2. Absolute install path baked at configure time.
  if (!kInstalledNamesFilePath.empty()) {
    fs::path p {std::string {kInstalledNamesFilePath}};
    if (fs::exists(p)) {
      return p;
    }
  }

  // 3. In-tree source path (development / out-of-source builds).
  if (!kSourceNamesFilePath.empty()) {
    fs::path p {std::string {kSourceNamesFilePath}};
    if (fs::exists(p)) {
      return p;
    }
  }

  // 4. Caller should fall back to compiled-in JSON.
  return std::nullopt;
}

const NamesRegistry& NamesRegistry::instance()
{
  static const NamesRegistry registry = []() -> NamesRegistry
  {
    if (const auto path = find_names_file(); path.has_value()) {
      try {
        NamesRegistry r {*path};
        spdlog::debug("names_registry: loaded from '{}'", path->string());
        return r;
      } catch (const std::exception& ex) {
        spdlog::warn(
            "names_registry: failed to load '{}': {} — using "
            "compiled-in fallback",
            path->string(),
            ex.what());
      }
    }
    try {
      return NamesRegistry {kBuiltinNamesJson};
    } catch (const std::exception& ex) {
      spdlog::error(
          "names_registry: compiled-in fallback also failed to parse: {}"
          " — disabling JSON key canonicalisation",
          ex.what());
      // Empty registry: canonicalize_json_keys becomes a verbatim copy.
      // This makes the system degrade gracefully to the pre-feature
      // behaviour (no alias support) rather than aborting startup.
      return NamesRegistry {
          std::string_view {R"({"version": 1, "aliases": []})"}};
    }
  }();
  return registry;
}

}  // namespace gtopt
