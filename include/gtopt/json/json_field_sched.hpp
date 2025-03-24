/**
 * @file      json_schedule.hpp
 * @brief     Header of
 * @date      Sat Mar 22 06:53:09 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{

using FileSched = gtopt::FileSched;

template<typename Type>
using Array = gtopt::Array<Type>;

using RealFieldSched = gtopt::RealFieldSched;
using OptRealFieldSched = gtopt::OptRealFieldSched;
using jvtl_RealFieldSched =
    json_variant_type_list<Real,
                           json_link_no_name<std::vector<Real>>,
                           FileSched>;

using IntFieldSched = gtopt::IntFieldSched;
using OptIntFieldSched = gtopt::OptIntFieldSched;
using jvtl_IntFieldSched =
    json_variant_type_list<Int, json_link_no_name<std::vector<Int>>, FileSched>;

using IntBoolFieldSched = gtopt::IntBoolFieldSched;
using OptIntBoolFieldSched = gtopt::OptIntBoolFieldSched;
using jvtl_IntBoolFieldSched =
    json_variant_type_list<IntBool,
                           json_link_no_name<std::vector<IntBool>>,
                           FileSched>;

using ActiveFieldSched = gtopt::ActiveFieldSched;
using OptActiveFieldSched = gtopt::OptActiveFieldSched;
using jvtl_ActiveFieldSched = jvtl_IntBoolFieldSched;

}  // namespace daw::json
