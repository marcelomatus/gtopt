/**
 * @file      json_basic_types.hpp
 * @brief     Header of json basic types
 * @date      Tue Mar 18 14:13:53 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Json basic types
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/basic_types.hpp>

namespace daw::json
{

using Uid = gtopt::Uid;
using OptUid = gtopt::OptUid;

using Name = gtopt::Name;

using OptName = gtopt::OptName;
using Name = gtopt::Name;

using Real = gtopt::Real;
using OptReal = gtopt::OptReal;

using Int = gtopt::Int;
using OptInt = gtopt::OptInt;

using Bool = gtopt::Bool;
using OptBool = gtopt::OptBool;

using IntBool = gtopt::IntBool;
using OptIntBool = gtopt::OptIntBool;

}  // namespace daw::json
