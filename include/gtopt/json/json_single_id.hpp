/**
 * @file      json_single_id.hpp
 * @brief     JSON serialization for SingleId variant type
 * @date      Fri Mar 28 17:28:01 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the daw::json variant type list for SingleId,
 * enabling JSON serialization of Uid-or-Name identifier references.
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/single_id.hpp>

namespace daw::json
{

using SingleId = gtopt::SingleId;
using OptSingleId = gtopt::OptSingleId;
using jvtl_SingleId = json_variant_type_list<Uid, Name>;

}  // namespace daw::json
