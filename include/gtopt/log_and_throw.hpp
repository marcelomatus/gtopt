/**
 * @file      log_and_throw.hpp
 * @brief     Log-then-throw helper codifying the gtopt error convention.
 * @date      2026-06-27
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * The gtopt convention for an error that is both worth logging and fatal to
 * the current operation is to format the message ONCE, log it, and throw the
 * same text (see e.g. array_index_traits.cpp).  This helper packages that so
 * call sites stay a single expression:
 *
 *     log_and_throw(std::format("cuopt: {} failed with status {}", what, rc));
 *
 * It is the natural home for the solver-plugin backends in particular: a
 * plugin throw propagates to gtopt_main's top-level handler, which converts it
 * to an `Error` WITHOUT logging — so without this the failure never reaches the
 * log file.  (Plugins can call spdlog only now that it is built shared; see the
 * spdlog CPM block in CMakeLists.txt.)
 */

#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace gtopt
{

/// Log @p msg at error level, then throw it as a `std::runtime_error`.
/// The message is passed as a `{}` argument (not the format string) so a stray
/// brace in already-formatted text is never reinterpreted as a placeholder.
[[noreturn]] inline void log_and_throw(std::string msg)
{
  spdlog::error("{}", msg);
  throw std::runtime_error(std::move(msg));
}

}  // namespace gtopt
