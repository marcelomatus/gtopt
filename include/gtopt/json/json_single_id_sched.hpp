/**
 * @file      json_single_id_sched.hpp
 * @brief     JSON serialization for the TB-schedulable SingleId variant.
 * @date      Mon Jun  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the daw::json variant type list for `OptTBSingleIdSched`.
 * The variant matches the type defined in `single_id_sched.hpp` —
 *
 *   `Uid`        — JSON number → scalar fuel uid (legacy).
 *   `Name`       — JSON string → scalar fuel name (legacy).
 *   `UidMatrix`  — JSON 2-D array of integers → per-(stage, block).
 *
 * Phase 1 of Issue #510 (Generator multi-fuel) introduces this type to
 * promote `Generator.fuel` from a static `OptSingleId` to a TB schedule.
 * The file-backed-schedule alternative documented in the issue text is
 * DEFERRED — see `single_id_sched.hpp` for the rationale.
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/single_id_sched.hpp>

namespace daw::json
{

using gtopt::OptTBSingleIdSched;
using gtopt::TBSingleIdSched;
using gtopt::UidMatrix;

// Variant element ordering (kept in lock-step with `TBSingleIdSched`):
//   0. Uid        — JSON number → scalar fuel uid (legacy).
//   1. Name       — JSON string → scalar fuel name (legacy).
//   2. UidMatrix  — JSON array of arrays → 2-D `[stage][block]` matrix.
//
// daw::json's variant_type_list dispatches by JSON token type — number
// pins `Uid`, array pins `UidMatrix`, string pins `Name`.  A FileSched
// (file-backed schedule) alternative is DEFERRED for Phase 1; daw::json
// cannot disambiguate two string-valued alternatives, so file-backed
// fuel scheduling needs a tagged-string wrapper landed first.
using jvtl_TBSingleIdSched =
    json_variant_type_list<Uid, Name, json_link_no_name<UidMatrix>>;

}  // namespace daw::json
