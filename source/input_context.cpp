/**
 * @file      input_context.cpp
 * @brief     Header of
 * @date      Fri Apr 25 14:17:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

InputContext::InputContext(const SystemContext& system_context)
    : ElementContext(system_context.system())
    , m_system_context_(system_context)
{
}

}  // namespace gtopt
