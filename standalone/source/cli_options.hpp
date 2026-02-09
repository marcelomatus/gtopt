/**
 * @file      cli_options.hpp
 * @brief     Lightweight command-line option parser using modern C++
 * @date      Sun Feb  9 07:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Replaces boost::program_options with a self-contained modern C++ parser.
 */

#pragma once

#include <algorithm>
#include <any>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cli
{

class parse_error : public std::runtime_error
{
  using std::runtime_error::runtime_error;
};

/// Stores parsed option values
class variables_map
{
public:
  [[nodiscard]] auto contains(const std::string& name) const -> bool
  {
    return std::ranges::any_of(
        entries_, [&](const auto& e) { return e.name == name; });
  }

  struct entry
  {
    std::string name;
    std::any value;

    template<typename T>
    [[nodiscard]] auto as() const -> T
    {
      return std::any_cast<T>(value);
    }
  };

  auto operator[](const std::string& name) const -> const entry&
  {
    auto it = std::ranges::find_if(
        entries_, [&](const auto& e) { return e.name == name; });
    if (it == entries_.end()) {
      throw std::out_of_range(std::format("option '{}' not found", name));
    }
    return *it;
  }

  void set(const std::string& name, std::any value)
  {
    auto it = std::ranges::find_if(
        entries_, [&](const auto& e) { return e.name == name; });
    if (it != entries_.end()) {
      it->value = std::move(value);
    } else {
      entries_.push_back({name, std::move(value)});
    }
  }

private:
  std::vector<entry> entries_;
};

/// Describes a single command-line option
struct option_desc
{
  std::string long_name;
  char short_name {};
  std::string description;

  // type: 0=flag, 1=string, 2=int, 3=double, 4=bool, 5=vector<string>
  int value_type {};
  bool has_implicit {};
  std::any implicit_value;
};

/// Builder for options descriptions
class options_description
{
public:
  explicit options_description(std::string title) : title_(std::move(title)) {}

  class option_builder
  {
  public:
    option_builder(options_description& parent, option_desc desc)
        : parent_(parent), desc_(std::move(desc))
    {
    }

    auto operator()(const std::string& name_spec,
                    const std::string& description) -> option_builder&
    {
      finish();
      desc_ = parse_name_spec(name_spec);
      desc_.description = description;
      desc_.value_type = 0;  // flag
      finished_ = false;
      return *this;
    }

    template<typename T>
    struct typed_value
    {
      bool has_implicit = false;
      T implicit_val {};

      auto implicit_value(T val) -> typed_value
      {
        has_implicit = true;
        implicit_val = std::move(val);
        return std::move(*this);
      }
    };

    template<typename T>
    auto operator()(const std::string& name_spec,
                    typed_value<T> tv,
                    const std::string& description) -> option_builder&
    {
      finish();
      desc_ = parse_name_spec(name_spec);
      desc_.description = description;

      if constexpr (std::is_same_v<T, std::string>) {
        desc_.value_type = 1;
      } else if constexpr (std::is_same_v<T, int>) {
        desc_.value_type = 2;
      } else if constexpr (std::is_same_v<T, double>) {
        desc_.value_type = 3;
      } else if constexpr (std::is_same_v<T, bool>) {
        desc_.value_type = 4;
      } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        desc_.value_type = 5;
      }

      if (tv.has_implicit) {
        desc_.has_implicit = true;
        desc_.implicit_value = tv.implicit_val;
      }
      finished_ = false;
      return *this;
    }

    ~option_builder() { finish(); }

    option_builder(const option_builder&) = delete;
    auto operator=(const option_builder&) -> option_builder& = delete;
    option_builder(option_builder&& other) noexcept
        : parent_(other.parent_)
        , desc_(std::move(other.desc_))
        , finished_(other.finished_)
    {
      other.finished_ = true;
    }
    auto operator=(option_builder&&) -> option_builder& = delete;

  private:
    void finish()
    {
      if (!finished_ && !desc_.long_name.empty()) {
        parent_.options_.push_back(desc_);
      }
      finished_ = true;
      desc_ = {};
    }

    static auto parse_name_spec(const std::string& spec) -> option_desc
    {
      option_desc d;
      auto comma = spec.find(',');
      if (comma != std::string::npos) {
        d.long_name = spec.substr(0, comma);
        if (comma + 1 < spec.size()) {
          d.short_name = spec[comma + 1];
        }
      } else {
        d.long_name = spec;
      }
      return d;
    }

    options_description& parent_;
    option_desc desc_;
    bool finished_ = false;
  };

  auto add_options() -> option_builder
  {
    return option_builder(*this, {});
  }

  template<typename T>
  static auto value() -> typename option_builder::typed_value<T>
  {
    return typename option_builder::typed_value<T>();
  }

  [[nodiscard]] auto options() const -> const std::vector<option_desc>&
  {
    return options_;
  }

  friend auto operator<<(std::ostream& os, const options_description& desc)
      -> std::ostream&
  {
    os << desc.title_ << ":\n";
    for (const auto& opt : desc.options_) {
      os << "  --" << opt.long_name;
      if (opt.short_name != 0) {
        os << ", -" << opt.short_name;
      }
      if (opt.value_type == 1) {
        os << " <string>";
      } else if (opt.value_type == 2) {
        os << " <int>";
      } else if (opt.value_type == 3) {
        os << " <double>";
      } else if (opt.value_type == 4) {
        os << " [bool]";
      } else if (opt.value_type == 5) {
        os << " <string>...";
      }
      os << "\n      " << opt.description << "\n";
    }
    return os;
  }

private:
  std::string title_;
  std::vector<option_desc> options_;
  friend class option_builder;
};

/// Parses command-line arguments given a description and positional mapping
inline void parse_args(int argc,
                       char** argv,
                       const options_description& desc,
                       const std::string& positional_name,
                       variables_map& vm,
                       bool allow_unregistered = false)
{
  const auto& opts = desc.options();

  auto find_by_long = [&](const std::string& name)
      -> std::optional<std::reference_wrapper<const option_desc>> {
    auto it = std::ranges::find_if(
        opts, [&](const auto& o) { return o.long_name == name; });
    if (it != opts.end()) return std::cref(*it);
    return std::nullopt;
  };

  auto find_by_short = [&](char c)
      -> std::optional<std::reference_wrapper<const option_desc>> {
    auto it = std::ranges::find_if(
        opts, [&](const auto& o) { return o.short_name == c; });
    if (it != opts.end()) return std::cref(*it);
    return std::nullopt;
  };

  auto store_value = [&](const option_desc& opt, const std::string& val) {
    switch (opt.value_type) {
      case 1:  // string
        vm.set(opt.long_name, val);
        break;
      case 2:  // int
        vm.set(opt.long_name, std::stoi(val));
        break;
      case 3:  // double
        vm.set(opt.long_name, std::stod(val));
        break;
      case 4:  // bool
      {
        bool bval = (val == "true" || val == "1" || val == "yes");
        vm.set(opt.long_name, bval);
      } break;
      case 5:  // vector<string>
      {
        std::vector<std::string> vec;
        if (vm.contains(opt.long_name)) {
          vec = vm[opt.long_name].as<std::vector<std::string>>();
        }
        vec.push_back(val);
        vm.set(opt.long_name, vec);
      } break;
      default:
        break;
    }
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg.starts_with("--")) {
      // Long option
      auto eq_pos = arg.find('=');
      std::string name = (eq_pos != std::string::npos)
          ? arg.substr(2, eq_pos - 2)
          : arg.substr(2);
      auto opt_ref = find_by_long(name);

      if (!opt_ref) {
        if (allow_unregistered) continue;
        throw parse_error(std::format("unknown option: --{}", name));
      }

      const auto& opt = opt_ref->get();

      if (opt.value_type == 0) {
        // Flag
        vm.set(opt.long_name, true);
      } else if (eq_pos != std::string::npos) {
        // --name=value
        store_value(opt, arg.substr(eq_pos + 1));
      } else if (opt.has_implicit) {
        // Option with implicit value: use implicit when no =value given
        vm.set(opt.long_name, opt.implicit_value);
      } else if (i + 1 < argc) {
        // --name value
        store_value(opt, argv[++i]);
      } else {
        throw parse_error(std::format("option --{} requires a value", name));
      }
    } else if (arg.starts_with("-") && arg.size() == 2) {
      // Short option
      char c = arg[1];
      auto opt_ref = find_by_short(c);

      if (!opt_ref) {
        if (allow_unregistered) continue;
        throw parse_error(std::format("unknown option: -{}", c));
      }

      const auto& opt = opt_ref->get();

      if (opt.value_type == 0) {
        vm.set(opt.long_name, true);
      } else if (opt.has_implicit) {
        vm.set(opt.long_name, opt.implicit_value);
      } else if (i + 1 < argc) {
        store_value(opt, argv[++i]);
      } else {
        throw parse_error(
            std::format("option -{} requires a value", c));
      }
    } else {
      // Positional argument
      if (!positional_name.empty()) {
        auto opt_ref = find_by_long(positional_name);
        if (opt_ref) {
          store_value(opt_ref->get(), arg);
        }
      } else if (!allow_unregistered) {
        throw parse_error(std::format("unexpected argument: {}", arg));
      }
    }
  }
}

}  // namespace cli
