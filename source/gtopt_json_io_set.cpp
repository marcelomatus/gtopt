/**
 * @file      gtopt_json_io_set.cpp
 * @brief     --set key=value overlay for Planning (split from
 * gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains apply_set_options() and its helpers.  Instantiates
 * from_json<Planning> for JSON overlay merging.
 */

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtopt/enum_option.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ── --set key=value support ───────────────────────────────────────────

/// Auto-detect the JSON type of a value string.
/// Returns the value ready for embedding in a JSON document:
///  - "true"/"false"/"null" → bare keyword
///  - integer string        → bare number
///  - decimal / scientific  → bare number
///  - anything else         → JSON-quoted string
[[nodiscard]] std::string to_json_value(const std::string& v)
{
  if (v == "true" || v == "false" || v == "null") {
    return v;
  }
  // Try integer
  {
    char* end = nullptr;
    (void)std::strtoll(v.c_str(), &end, /*base=*/10);
    if (end != v.c_str() && *end == '\0') {
      return v;
    }
  }
  // Try floating-point
  {
    char* end = nullptr;
    (void)std::strtod(v.c_str(), &end);
    if (end != v.c_str() && *end == '\0') {
      return v;
    }
  }
  // String: escape backslashes and double-quotes
  std::string escaped;
  escaped.reserve(v.size() + 2);
  escaped += '"';
  for (const char c : v) {
    if (c == '"' || c == '\\') {
      escaped += '\\';
    }
    escaped += c;
  }
  escaped += '"';
  return escaped;
}

/// A path component consisting entirely of ASCII digits is interpreted
/// as an array index during overlay synthesis.  Used by
/// `build_set_option_json` to distinguish object keys from positional
/// array indices in dotted `--set` paths.
[[nodiscard]] constexpr bool is_array_index(std::string_view s) noexcept
{
  return !s.empty()
      && std::ranges::all_of(
          s, [](char c) noexcept { return c >= '0' && c <= '9'; });
}

/// Parse a decimal non-negative integer from @p s.  Callers must guard
/// the input with `is_array_index` first; behaviour on non-digit input
/// is left as the partial prefix parse from `std::from_chars`.
[[nodiscard]] std::size_t parse_array_index(std::string_view s) noexcept
{
  std::size_t idx = 0;
  std::from_chars(s.data(), s.data() + s.size(), idx);
  return idx;
}

/// Build a minimal Planning JSON overlay from a dotted key and a JSON
/// value.  The key is a dotted path relative to `planning.options`, e.g.
/// `"sddp_options.forward_solver_options.threads"`.  The result is a
/// valid JSON string like:
/// `{"options":{"sddp_options":{"forward_solver_options":{"threads":8}}}}`
///
/// Path components consisting entirely of digits are treated as array
/// indices.  `cascade_options.level_array.0.sddp_options.max_iterations`
/// produces
/// `{"options":{"cascade_options":{"level_array":[{"sddp_options":{"max_iterations":…}}]}}}`.
/// Indices > 0 emit leading empty-object placeholders so the target
/// lands at the requested position; the merge side leaves untouched
/// elements alone (see `CascadeOptions::merge`).
/// Legacy short keys that used to live directly on `PlanningOptions`
/// but were moved under `model_options` in §11 of
/// `docs/analysis/naming-conventions.md`.  Apply this rewrite before
/// path-splitting so existing CLI invocations like
/// `--set demand_fail_cost=5000` continue to resolve.  Also applies
/// the two §11.10 renames (reserve_fail_cost → reserve_shortage_cost,
/// hydro_fail_cost → hydro_spill_cost).
[[nodiscard]] std::string rewrite_legacy_model_options_key(
    std::string_view dotted_key)
{
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 13>
      legacy = {
          {
              {"demand_fail_cost", "model_options.demand_fail_cost"},
              {"reserve_fail_cost", "model_options.reserve_shortage_cost"},
              {"reserve_shortage_cost", "model_options.reserve_shortage_cost"},
              {"hydro_fail_cost", "model_options.hydro_spill_cost"},
              {"hydro_spill_cost", "model_options.hydro_spill_cost"},
              {"hydro_use_value", "model_options.hydro_use_value"},
              {"use_line_losses", "model_options.use_line_losses"},
              {"loss_segments", "model_options.loss_segments"},
              {"use_kirchhoff", "model_options.use_kirchhoff"},
              {"use_single_bus", "model_options.use_single_bus"},
              {"kirchhoff_threshold", "model_options.kirchhoff_threshold"},
              {"scale_objective", "model_options.scale_objective"},
              {"scale_theta", "model_options.scale_theta"},
          },
      };
  // Only rewrite if the *first* segment is one of the legacy short keys
  // (i.e., the user wrote `--set demand_fail_cost=…`, not
  // `--set model_options.demand_fail_cost=…`).
  const auto first_dot = dotted_key.find('.');
  const auto head = dotted_key.substr(0, first_dot);
  for (const auto& [alias, canonical] : legacy) {
    if (head == alias) {
      if (first_dot == std::string_view::npos) {
        return std::string {canonical};
      }
      std::string out {canonical};
      out += dotted_key.substr(first_dot);  // includes leading '.'
      return out;
    }
  }
  return std::string {dotted_key};
}

[[nodiscard]] std::string build_set_option_json(std::string_view dotted_key_in,
                                                std::string_view json_val)
{
  const auto rewritten = rewrite_legacy_model_options_key(dotted_key_in);
  const std::string_view dotted_key {rewritten};

  // Split the key on '.' and collect into a vector of string_views.
  // `std::ranges::to<std::vector>()` has a libstdc++-14 / clang-21
  // compatibility issue (forward_like before definition), so we use the
  // explicit loop form instead.
  std::vector<std::string_view> parts;
  for (auto r : dotted_key | std::views::split('.')) {
    parts.emplace_back(r.data(), r.size());
  }

  // Build the nested path body and track the closing brackets we owe.
  // `closers` is appended in order; we emit it reversed at the end.
  std::string body;
  std::string closers;

  for (const auto i : iota_range<std::size_t>(0, parts.size())) {
    const auto part = parts[i];
    const bool is_last = (i + 1 == parts.size());

    // Numeric parts are consumed by the preceding key when it opens an
    // array.  A stand-alone numeric trailing part (e.g. `foo.0`) produces
    // an empty-object element, which the merge side leaves untouched.
    if (is_array_index(part)) {
      continue;
    }

    std::format_to(std::back_inserter(body), R"("{}":)", part);

    if (is_last) {
      body += json_val;
      continue;
    }

    const auto next = parts[i + 1];
    if (!is_array_index(next)) {
      // Non-numeric next → nest another object.
      body += '{';
      closers += '}';
      continue;
    }

    // Numeric next → open an array with `idx` empty-object placeholders
    // preceding the target element.  Merging an array of empty overlays
    // against the base is a no-op per element.
    const auto idx = parse_array_index(next);
    body += '[';
    for ([[maybe_unused]] const auto _ : iota_range<std::size_t>(0, idx)) {
      body += "{},";
    }
    body += '{';
    // `closers` is emitted reversed — push the outer bracket first so it
    // ends up LAST (outermost) when reversed.  Array closer `]` precedes
    // the element-object closer `}` in pushing order.
    closers += "]}";
  }

  std::string json;
  json.reserve(body.size() + closers.size() + 14);
  json += R"({"options":{)";
  json += body;
  // std::string::append_range is C++26 and missing from GCC 14 (CI).
  // std::ranges::reverse_copy is C++20 and equivalent.
  std::ranges::reverse_copy(closers, std::back_inserter(json));
  json += "}}";
  return json;
}

/// Signature of a single SolverOptions field setter: takes the struct, the
/// field name (so `require_bool`/`require_enum` can build a good error
/// message without re-typing the literal) and the raw string value.  A
/// captureless lambda converts to this function pointer.
using SolverFieldSetter = void (*)(SolverOptions&,
                                   std::string_view,
                                   const std::string&);

/// Registry of `--set solver_options.<field>=value` setters, keyed by JSON
/// field name — the single source of truth for the settable SolverOptions
/// surface.  Adding a field is one entry here (the "every settable field
/// round-trips" test guards the set).  Replaced an ~18-branch
/// `if (field == ...)` chain where forgetting a branch silently dropped the
/// field — the defect behind the `crossover` regression.
[[nodiscard]] const std::unordered_map<std::string_view, SolverFieldSetter>&
solver_field_setters()
{
  static const std::unordered_map<std::string_view, SolverFieldSetter>
      registry {
          {"threads",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.threads = std::stoi(v); }},
          {"log_level",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.log_level = std::stoi(v); }},
          {"max_fallbacks",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.max_fallbacks = std::stoi(v); }},
          {"presolve",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.presolve = require_bool(n, v); }},
          {"advanced_basis",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.advanced_basis = require_bool(n, v); }},
          {"crossover",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.crossover = require_enum<CrossoverMode>(n, v); }},
          {"force_barrier_crossover",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.force_barrier_crossover = require_bool(n, v); }},
          {"memory_emphasis",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.memory_emphasis = require_bool(n, v); }},
          {"optimal_eps",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.optimal_eps = std::stod(v); }},
          {"feasible_eps",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.feasible_eps = std::stod(v); }},
          {"barrier_eps",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.barrier_eps = std::stod(v); }},
          {"time_limit",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.time_limit = std::stod(v); }},
          {"mip_gap",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.mip_gap = std::stod(v); }},
          {"mip_gap_abs",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.mip_gap_abs = std::stod(v); }},
          {"param_file",
           [](SolverOptions& o, std::string_view, const std::string& v)
           { o.param_file = v; }},
          {"log_mode",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.log_mode = require_enum<SolverLogMode>(n, v); }},
          {"scaling",
           [](SolverOptions& o, std::string_view n, const std::string& v)
           { o.scaling = require_enum<SolverScaling>(n, v); }},
          // `algorithm` is the one irregular parse: a name (barrier) OR a
          // numeric index (0..3), so its setter is a small inline lambda
          // rather than a uniform `require_*` call.
          {"algorithm",
           [](SolverOptions& o, std::string_view, const std::string& v)
           {
             if (const auto algo = enum_from_name<LPAlgo>(v)) {
               o.algorithm = *algo;
               return;
             }
             const auto iv = std::stoi(v);
             if (iv < 0
                 || std::cmp_greater_equal(
                     iv, std::to_underlying(LPAlgo::last_algo)))
             {
               throw std::invalid_argument(
                   std::format("algorithm value {} out of range (0–{})",
                               iv,
                               std::to_underlying(LPAlgo::last_algo) - 1));
             }
             o.algorithm = static_cast<LPAlgo>(iv);
           }},
      };
  return registry;
}

/// Set a single field on a SolverOptions struct via the registry.
/// @return true if the field was recognised and set.
[[nodiscard]] bool try_set_solver_field(SolverOptions& so,
                                        std::string_view field,
                                        const std::string& value)
{
  const auto& registry = solver_field_setters();
  const auto it = registry.find(field);
  if (it == registry.end()) {
    return false;
  }
  it->second(so, field, value);
  return true;
}

/// Try to handle a --set key=value as a direct SolverOptions field set.
/// Returns true if the key matched a solver_options path and was applied.
[[nodiscard]] bool try_set_solver_options_path(Planning& planning,
                                               std::string_view key,
                                               const std::string& value)
{
  // solver_options.<field>
  constexpr std::string_view pfx_so = "solver_options.";
  if (key.starts_with(pfx_so)) {
    return try_set_solver_field(
        planning.options.solver_options, key.substr(pfx_so.size()), value);
  }

  // sddp_options.forward_solver_options.<field>
  constexpr std::string_view pfx_fwd = "sddp_options.forward_solver_options.";
  if (key.starts_with(pfx_fwd)) {
    auto& fso = planning.options.sddp_options.forward_solver_options;
    if (!fso) {
      fso = SolverOptions {};
    }
    return try_set_solver_field(*fso, key.substr(pfx_fwd.size()), value);
  }

  // sddp_options.backward_solver_options.<field>
  constexpr std::string_view pfx_bwd = "sddp_options.backward_solver_options.";
  if (key.starts_with(pfx_bwd)) {
    auto& bso = planning.options.sddp_options.backward_solver_options;
    if (!bso) {
      bso = SolverOptions {};
    }
    return try_set_solver_field(*bso, key.substr(pfx_bwd.size()), value);
  }

  // monolithic_options.solver_options.<field>
  constexpr std::string_view pfx_mono = "monolithic_options.solver_options.";
  if (key.starts_with(pfx_mono)) {
    auto& mso = planning.options.monolithic_options.solver_options;
    if (!mso) {
      mso = SolverOptions {};
    }
    return try_set_solver_field(*mso, key.substr(pfx_mono.size()), value);
  }

  return false;
}

// ── end --set support ─────────────────────────────────────────────────

}  // namespace

bool apply_set_options(Planning& planning,
                       const std::vector<std::string>& set_options)
{
  for (const auto& opt : set_options) {
    const auto eq_pos = opt.find('=');
    if (eq_pos == std::string::npos || eq_pos == 0) {
      spdlog::error("--set: invalid format '{}' (expected key=value)", opt);
      return false;
    }
    const auto key = opt.substr(0, eq_pos);
    const auto value = opt.substr(eq_pos + 1);

    // Direct setter for SolverOptions paths (non-optional struct fields)
    try {
      if (try_set_solver_options_path(planning, key, value)) {
        spdlog::info("--set {}={} applied", key, value);
        continue;
      }
    } catch (const std::exception& ex) {
      spdlog::error("--set {}={}: {}", key, value, ex.what());
      return false;
    }

    // JSON overlay approach for all other paths
    const auto json_val = to_json_value(value);
    auto json = build_set_option_json(key, json_val);

    // Pad the overlay's cascade level_array to match the base size before
    // merging.  `build_set_option_json` only constructs (N+1) elements when
    // targeting `cascade_options.level_array.N...` — so when N+1 < base
    // size, `CascadeOptions::merge` would otherwise fall into its
    // "size-mismatch → wholesale replace" branch and silently drop every
    // base level past N.  Padding the overlay with empty CascadeLevel
    // placeholders forces element-wise merge across the full base array.
    const auto pad_level_array_overlay = [&](Planning& overlay)
    {
      auto& ov_levels = overlay.options.cascade_options.level_array;
      const auto& base_levels = planning.options.cascade_options.level_array;
      if (!ov_levels.empty() && ov_levels.size() < base_levels.size()) {
        ov_levels.resize(base_levels.size());
      }
    };

    try {
      auto overlay = daw::json::from_json<Planning>(json, StrictParsePolicy);
      pad_level_array_overlay(overlay);
      planning.merge(std::move(overlay));
      spdlog::info("--set {}={} applied", key, value);
    } catch (const daw::json::json_exception& ex) {
      // If auto-typed value failed (e.g. number where string expected),
      // retry with the value as a quoted string
      if (json_val.front() != '"') {
        auto str_json = build_set_option_json(key, "\"" + value + "\"");
        try {
          auto overlay =
              daw::json::from_json<Planning>(str_json, StrictParsePolicy);
          pad_level_array_overlay(overlay);
          planning.merge(std::move(overlay));
          spdlog::info("--set {}={} applied (as string)", key, value);
          continue;
        } catch (...) {
        }
      }
      spdlog::error("--set {}={}: unknown option or invalid value", key, value);
      return false;
    }
  }
  return true;
}

}  // namespace gtopt
