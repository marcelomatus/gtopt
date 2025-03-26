#include <utility>

#include <fmt/format.h>
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
      return fmt::format("Hello, {}!", name);
    case LanguageCode::DE:
      return fmt::format("Hallo {}!", name);
    case LanguageCode::ES:
      return fmt::format("¡Hola {}!", name);
    case LanguageCode::FR:
      return fmt::format("Bonjour {}!", name);
  }
}
