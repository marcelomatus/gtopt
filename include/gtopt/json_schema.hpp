/**
 * @file      json_schema.hpp
 * @brief     JSON Schema dump for the gtopt input data model (options)
 * @date      2026-06-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a single diagnostic entry point that renders a JSON Schema
 * (draft 2020-12) for the gtopt input data model directly from the
 * daw_json_link ``json_data_contract`` mappings.  No input file is
 * needed — the schema is generated purely from the compile-time type
 * descriptions, mirroring the ``--list-dialects`` "dump-and-exit"
 * diagnostic.
 *
 * CLI integration
 * ---------------
 * Invoked by the ``--json-schema`` flag of the gtopt binary:
 *   - ``gtopt --json-schema``         dumps the default (``options``)
 *     schema.
 *   - ``gtopt --json-schema options`` selects a known top-level type by
 *     name.
 *
 * Only top-level types that carry a ``json_data_contract`` *and* render
 * cleanly through ``daw::json::to_json_schema`` are exposed; see
 * ``known_schema_names()`` for the dispatch table.  The full ``Planning``
 * / ``System`` schemas are intentionally absent: their element-level
 * field schedules map to variants whose alternatives include bare scalar
 * types, which the bundled daw_json_link cannot render (it would fail to
 * compile).  ``PlanningOptions`` — the configuration contract operators
 * hand-edit most — is the renderable default.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gtopt
{

/**
 * @brief Render the JSON Schema of a known top-level gtopt input type.
 *
 * @param type_name  Selects which top-level type to render.  An empty or
 *                   absent name yields the default schema (the
 *                   ``PlanningOptions`` configuration contract).  A
 *                   non-empty name selects a known top-level type
 *                   (case-insensitive), e.g. ``"options"``.
 * @return The JSON Schema as a string.  For an UNKNOWN @p type_name, an
 *         error listing the known names is printed to ``stderr`` and an
 *         empty string is returned (mirrors the ``--list-dialects``
 *         unknown-filter behaviour).
 */
[[nodiscard]] std::string dump_json_schema(std::string_view type_name = {});

/**
 * @brief The set of known top-level schema names accepted by
 *        ``dump_json_schema`` (the first entry is the default).
 *        Exposed for diagnostics and tests.
 */
[[nodiscard]] const std::vector<std::string_view>& known_schema_names();

}  // namespace gtopt
