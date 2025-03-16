#include <doctest/doctest.h>
#include <gtopt/gtopt.h>
#include <gtopt/version.h>

#include <string>

TEST_CASE("Gtopt") {
  using namespace gtopt;

  Gtopt gtopt("Tests");

  CHECK(gtopt.greet(LanguageCode::EN) == "Hello, Tests!");
  CHECK(gtopt.greet(LanguageCode::DE) == "Hallo Tests!");
  CHECK(gtopt.greet(LanguageCode::ES) == "¡Hola Tests!");
  CHECK(gtopt.greet(LanguageCode::FR) == "Bonjour Tests!");
}

TEST_CASE("Gtopt version") {
  static_assert(std::string_view(GTOPT_VERSION) == std::string_view("1.0"));
  CHECK(std::string(GTOPT_VERSION) == std::string("1.0"));
}
