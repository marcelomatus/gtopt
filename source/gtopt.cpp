#include <fmt/format.h>
#include <gtopt/gtopt.h>

using namespace gtopt;

Gtopt::Gtopt(std::string _name) : name(std::move(_name)) {}

std::string Gtopt::greet(LanguageCode lang) const {
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
