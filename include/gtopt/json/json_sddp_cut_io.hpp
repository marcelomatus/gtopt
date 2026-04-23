/**
 * @file      json_sddp_cut_io.hpp
 * @brief     daw::json data contracts for SDDP cut/state JSON I/O
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines lightweight data structures and their daw::json mappings for
 * serializing Benders cuts and state variable columns in compact JSON.
 * Used by save_cuts_json / load_cuts_json as an alternative to CSV.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <daw/json/daw_json_link.h>

namespace gtopt
{

// ─── Cut coefficient entry ──────────────────────────────────────────────────

/// A single coefficient in a serialized Benders cut.
/// The key encodes the variable identity as "class:var:uid"
/// (e.g. "Reservoir:efin:1" or "Sddp:alpha:0").
struct CutCoeffEntry
{
  std::string key {};  ///< Structured key "class:var:uid"
  double coeff {};  ///< Coefficient in physical space
};

// ─── Stored cut entry ───────────────────────────────────────────────────────

/// A serialized Benders cut with structured coefficient keys.
struct CutEntry
{
  std::string type {};  ///< "o" (optimality) or "f" (feasibility)
  int phase_uid {};  ///< Phase UID this cut belongs to
  int scene_uid {};  ///< Scene UID that generated this cut
  std::string name {};  ///< Cut name (for iteration tracking)
  double rhs {};  ///< Right-hand side in physical objective units
  std::optional<double> dual {};  ///< Row dual (nullopt = unknown)
  std::vector<CutCoeffEntry> coefficients {};  ///< Variable coefficients
};

// ─── Cut file wrapper ───────────────────────────────────────────────────────

/// Top-level wrapper for the JSON cut file.
struct CutFileData
{
  int version {2};  ///< Format version
  double scale_objective {};  ///< Scale objective used when saving
  std::vector<CutEntry> cuts {};  ///< All cuts
};

// ─── State column entry ─────────────────────────────────────────────────────

/// A single state variable column solution.
struct StateColEntry
{
  std::string key {};  ///< Structured key: "class:var:uid"
  int phase_uid {};  ///< Phase UID
  int scene_uid {};  ///< Scene UID
  double value {};  ///< Column value (physical units)
  double rcost {};  ///< Reduced cost
};

/// Top-level wrapper for the JSON state file.
struct StateFileData
{
  int version {2};  ///< Format version
  int iteration {};  ///< Iteration index
  std::vector<StateColEntry> columns {};  ///< State variable columns
};

}  // namespace gtopt

// ─── daw::json data contracts ───────────────────────────────────────────────

namespace daw::json
{

template<>
struct json_data_contract<gtopt::CutCoeffEntry>
{
  using type = json_member_list<json_string<"key", std::string>,
                                json_number<"coeff", double>>;

  [[nodiscard]] static auto to_json_data(gtopt::CutCoeffEntry const& v)
  {
    return std::forward_as_tuple(v.key, v.coeff);
  }
};

template<>
struct json_data_contract<gtopt::CutEntry>
{
  using type =
      json_member_list<json_string<"type", std::string>,
                       json_number<"phase", int>,
                       json_number<"scene", int>,
                       json_string<"name", std::string>,
                       json_number<"rhs", double>,
                       json_number_null<"dual", std::optional<double>>,
                       json_array<"coefficients", gtopt::CutCoeffEntry>>;

  [[nodiscard]] static auto to_json_data(gtopt::CutEntry const& v)
  {
    return std::forward_as_tuple(v.type,
                                 v.phase_uid,
                                 v.scene_uid,
                                 v.name,
                                 v.rhs,
                                 v.dual,
                                 v.coefficients);
  }
};

template<>
struct json_data_contract<gtopt::CutFileData>
{
  using type = json_member_list<json_number<"version", int>,
                                json_number<"scale_objective", double>,
                                json_array<"cuts", gtopt::CutEntry>>;

  [[nodiscard]] static auto to_json_data(gtopt::CutFileData const& v)
  {
    return std::forward_as_tuple(v.version, v.scale_objective, v.cuts);
  }
};

template<>
struct json_data_contract<gtopt::StateColEntry>
{
  using type = json_member_list<json_string<"key", std::string>,
                                json_number<"phase", int>,
                                json_number<"scene", int>,
                                json_number<"value", double>,
                                json_number<"rcost", double>>;

  [[nodiscard]] static auto to_json_data(gtopt::StateColEntry const& v)
  {
    return std::forward_as_tuple(
        v.key, v.phase_uid, v.scene_uid, v.value, v.rcost);
  }
};

template<>
struct json_data_contract<gtopt::StateFileData>
{
  using type = json_member_list<json_number<"version", int>,
                                json_number<"iteration", int>,
                                json_array<"columns", gtopt::StateColEntry>>;

  [[nodiscard]] static auto to_json_data(gtopt::StateFileData const& v)
  {
    return std::forward_as_tuple(v.version, v.iteration, v.columns);
  }
};

}  // namespace daw::json
