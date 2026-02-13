/**
 * @file      cli_options.hpp
 * @brief     Modern C++ command-line option parsing (replaces
 *            boost::program_options)
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a lightweight command-line parser using only the C++ standard
 * library.  The public surface intentionally mirrors the subset of
 * boost::program_options used by the project so that call-sites require
 * minimal changes.  Non-template implementation details live in
 * cli_options.cpp.
 */

#pragma once

#include <any>
#include <format>
#include <functional>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace gtopt::cli
{

// -------------------------------------------------------------------
//  parse_error – thrown on malformed or unknown options
// -------------------------------------------------------------------

class parse_error : public std::runtime_error
{
  using std::runtime_error::runtime_error;
};

// -------------------------------------------------------------------
//  option_value  – type-erased value holder for a single option
// -------------------------------------------------------------------

class option_value
{
public:
  /// Retrieve the stored value with type checking.
  template<typename T>
  [[nodiscard]] T as() const
  {
    return std::any_cast<T>(value_);
  }

  [[nodiscard]] bool empty() const noexcept { return !value_.has_value(); }

  // -- internal setters used by the parser --
  void set(std::any v) { value_ = std::move(v); }

private:
  std::any value_;
};

// -------------------------------------------------------------------
//  variables_map – stores parsed option values
// -------------------------------------------------------------------

class variables_map
{
public:
  [[nodiscard]] bool contains(const std::string& name) const
  {
    auto it = map_.find(name);
    return it != map_.end() && !it->second.empty();
  }

  option_value& operator[](const std::string& name) { return map_[name]; }

  [[nodiscard]] const option_value& operator[](const std::string& name) const
  {
    return map_.at(name);
  }

private:
  std::unordered_map<std::string, option_value> map_;
};

// -------------------------------------------------------------------
//  option_definition – describes a single CLI option
// -------------------------------------------------------------------

struct option_definition
{
  std::string long_name;  // e.g. "help"
  char short_name = '\0';  // e.g. 'h'
  std::string description;  // help text

  // Does this option accept a value argument?
  bool takes_value = false;

  // If the option can appear without '=<val>' and still be present
  // (e.g. --use-single-bus with implicit true).
  bool has_implicit = false;
  std::any implicit_value;

  // Can the option accept multiple values (collected in a vector)?
  bool multi_value = false;

  // Parser that converts a string token into std::any.
  // Null when the option is a pure flag (no value at all).
  std::function<std::any(const std::string&)> parser;
};

// -------------------------------------------------------------------
//  typed_value helpers – builder helpers for option values
// -------------------------------------------------------------------

/// Tag type for typed option building.
template<typename T>
struct typed_value
{
  bool has_implicit = false;
  T implicit_val {};
  bool multi = false;

  typed_value& implicit_value(T v)
  {
    has_implicit = true;
    implicit_val = std::move(v);
    return *this;
  }
};

/// Helper trait to detect std::vector types.
template<typename T>
inline constexpr bool is_vector_v = false;

template<typename T, typename A>
inline constexpr bool is_vector_v<std::vector<T, A>> = true;

/// Create a typed value descriptor for option definitions.
template<typename T>
[[nodiscard]] typed_value<T> value()
{
  if constexpr (is_vector_v<T>) {
    typed_value<T> tv;
    tv.multi = true;
    return tv;
  } else {
    return {};
  }
}

// -------------------------------------------------------------------
//  options_description – collection of option definitions
// -------------------------------------------------------------------

class options_description
{
public:
  explicit options_description(std::string caption = {})
      : caption_(std::move(caption))
  {
  }

  // ------------------------------------------------------------------
  //  Fluent "adder" returned by add_options()
  // ------------------------------------------------------------------
  class adder
  {
  public:
    explicit adder(options_description& parent)
        : parent_(parent)
    {
    }

    /// Pure flag option (no value, e.g. --help / -h).
    adder& operator()(const std::string& name_spec, const std::string& desc);

    /// Option with a typed value.
    template<typename T>
    adder& operator()(const std::string& name_spec,
                      const typed_value<T>& tv,
                      const std::string& desc)
    {
      option_definition def;
      parse_name_spec(name_spec, def);
      def.description = desc;
      def.takes_value = true;
      def.multi_value = tv.multi;

      if (tv.has_implicit) {
        def.has_implicit = true;
        def.implicit_value = std::any(tv.implicit_val);
      }

      // Build the string→any parser for the value type
      if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        def.multi_value = true;
        def.parser = [](const std::string& s) -> std::any
        {
          return s;  // collected later
        };
      } else {
        def.parser = [](const std::string& s) -> std::any
        {
          if constexpr (std::is_same_v<T, std::string>) {
            return s;
          } else if constexpr (std::is_same_v<T, bool>) {
            if (s == "1" || s == "true" || s == "yes") {
              return true;
            }
            if (s == "0" || s == "false" || s == "no") {
              return false;
            }
            throw parse_error(std::format("invalid boolean value: '{}'", s));
          } else if constexpr (std::is_same_v<T, int>) {
            return std::stoi(s);
          } else if constexpr (std::is_same_v<T, double>) {
            return std::stod(s);
          } else {
            static_assert(sizeof(T) == 0, "unsupported option type");
          }
        };
      }

      parent_.add(std::move(def));
      return *this;
    }

  private:
    static void parse_name_spec(const std::string& spec,
                                option_definition& def);

    options_description& parent_;  // NOLINT
  };

  [[nodiscard]] adder add_options() { return adder(*this); }

  void add(option_definition def);

  [[nodiscard]] const option_definition* find_long(
      const std::string& name) const;

  [[nodiscard]] const option_definition* find_short(char c) const;

  [[nodiscard]] const std::vector<option_definition>& options() const
  {
    return options_;
  }

  [[nodiscard]] const std::string& caption() const { return caption_; }

  /// Pretty-print the options (like boost's operator<<).
  friend std::ostream& operator<<(std::ostream& os,
                                  const options_description& desc);

private:
  std::string caption_;
  std::vector<option_definition> options_;
  std::unordered_map<std::string, std::size_t> long_index_;
  std::unordered_map<char, std::size_t> short_index_;
};

// -------------------------------------------------------------------
//  positional_options_description
// -------------------------------------------------------------------

class positional_options_description
{
public:
  /// Register an option name for positional arguments.
  /// @param name  Option name to map positional args to.
  /// @param count Maximum positional slots (-1 = unlimited).
  void add(const std::string& name, int count);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] int count() const noexcept { return count_; }

private:
  std::string name_;
  int count_ = 0;
};

// -------------------------------------------------------------------
//  command_line_parser – the actual parser
// -------------------------------------------------------------------

class command_line_parser
{
public:
  /// Construct from argc/argv (skips argv[0]).
  command_line_parser(int argc, char** argv);

  /// Construct from a vector of strings.
  explicit command_line_parser(std::vector<std::string> args)
      : tokens_(std::move(args))
  {
  }

  command_line_parser& options(const options_description& desc);

  command_line_parser& positional(const positional_options_description& pos);

  command_line_parser& allow_unregistered();

  /// Parse and fill the supplied variables_map.
  void parse_into(variables_map& vm) const;

private:
  static void store_value(
      variables_map& vm,
      const option_definition& def,
      const std::string& raw,
      std::unordered_map<std::string, std::vector<std::string>>& multi);

  std::vector<std::string> tokens_;
  const options_description* desc_ = nullptr;
  const positional_options_description* pos_ = nullptr;
  bool allow_unregistered_ = false;
};

// -------------------------------------------------------------------
//  Convenience: store / notify  (no-ops for API compatibility)
// -------------------------------------------------------------------

void store(const command_line_parser& parser, variables_map& vm);

void notify(const variables_map& /*vm*/);

}  // namespace gtopt::cli
