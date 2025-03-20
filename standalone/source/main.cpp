#include <iostream>
#include <string>
#include <unordered_map>

#include <cxxopts.hpp>
#include <gtopt/gtopt.hpp>
#include <gtopt/version.h>

using std::cout;

auto main(int argc, char** argv) -> int
{
  const std::unordered_map<std::string, gtopt::LanguageCode> languages {
      {"en", gtopt::LanguageCode::EN},
      {"de", gtopt::LanguageCode::DE},
      {"es", gtopt::LanguageCode::ES},
      {"fr", gtopt::LanguageCode::FR},
  };

  cxxopts::Options options(*argv, "A program to welcome the world!");

  std::string language;
  std::string name;

  // clang-format off
  options.add_options()
    ("h,help", "Show help")
    ("v,version", "Print the current version number")
    ("n,name", "Name to greet", cxxopts::value(name)->default_value("World"))
    ("l,lang", "Language code to use", cxxopts::value(language)->default_value("en"))
  ;
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result["help"].as<bool>()) {
    cout << options.help() << '\n';
    return 0;
  }

  if (result["version"].as<bool>()) {
    cout << "Gtopt, version " << GTOPT_VERSION << '\n';
    return 0;
  }

  auto langIt = languages.find(language);
  if (langIt == languages.end()) {
    std::cerr << "unknown language code: " << language << '\n';
    return 1;
  }

  gtopt::Gtopt gtopt(name);
  std::cout << gtopt.greet(langIt->second) << '\n';

  return 0;
}
