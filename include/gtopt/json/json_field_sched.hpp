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

using gtopt::OptRealFieldSched;
using gtopt::RealFieldSched;
using jvtl_RealFieldSched =
    json_variant_type_list<Real,
                           json_link_no_name<std::vector<Real>>,
                           FileSched>;

using gtopt::IntFieldSched;
using gtopt::OptIntFieldSched;
using jvtl_IntFieldSched =
    json_variant_type_list<Int, json_link_no_name<std::vector<Int>>, FileSched>;

using gtopt::IntBoolFieldSched;
using gtopt::OptIntBoolFieldSched;
using jvtl_IntBoolFieldSched =
    json_variant_type_list<IntBool,
                           json_link_no_name<std::vector<IntBool>>,
                           FileSched>;

using gtopt::Active;
using gtopt::OptActive;
using jvtl_Active = jvtl_IntBoolFieldSched;

using gtopt::OptSTBRealFieldSched;
using gtopt::OptTBRealFieldSched;
using gtopt::OptTRealFieldSched;
using gtopt::STBRealFieldSched;
using gtopt::TBRealFieldSched;
using gtopt::TRealFieldSched;

using jvtl_RealFieldSched1 = jvtl_RealFieldSched;

using jvtl_RealFieldSched2 =
    json_variant_type_list<Real,
                           json_link_no_name<std::vector<std::vector<Real>>>,
                           FileSched>;

using jvtl_RealFieldSched3 = json_variant_type_list<
    Real,
    json_link_no_name<std::vector<std::vector<std::vector<Real>>>>,
    FileSched>;

using jvtl_TBRealFieldSched = jvtl_RealFieldSched2;
using jvtl_STBRealFieldSched = jvtl_RealFieldSched3;
using jvtl_TRealFieldSched = jvtl_RealFieldSched1;

}  // namespace daw::json
