/**
 * @file      gtopt.cpp
 * @brief     Header of
 * @date      Wed Mar 26 14:38:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <format>

#include <gtopt/basic_types.hpp>
#include <gtopt/bus.hpp>
#include <gtopt/gtopt.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/system.hpp>

using namespace gtopt;

Gtopt::Gtopt(std::string name)
    : name(std::move(name))
{
}

std::string Gtopt::greet(LanguageCode lang) const
{
  switch (lang) {
    default:
    case LanguageCode::EN:
      return std::format("Hello, {}!", name);
    case LanguageCode::DE:
      return std::format("Hallo {}!", name);
    case LanguageCode::ES:
      return std::format("¡Hola {}!", name);
    case LanguageCode::FR:
      return std::format("Bonjour {}!", name);
  }
}
