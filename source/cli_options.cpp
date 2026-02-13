/**
 * @file      cli_options.cpp
 * @brief     Implementation of the modern C++ command-line option parser
 * @date      Wed Feb 12 22:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains non-template implementation details for the CLI options parser.
 * See cli_options.hpp for the public interface and template definitions.
 */

#include <iostream>
#include <ranges>
#include <span>

#include <gtopt/cli_options.hpp>

namespace gtopt::cli
{

// -------------------------------------------------------------------
//  options_description::adder
// -------------------------------------------------------------------

void options_description::adder::parse_name_spec(const std::string& spec,
                                                 option_definition& def)
{
  auto comma = spec.find(',');
  if (comma != std::string::npos) {
    def.long_name = spec.substr(0, comma);
    if (comma + 1 < spec.size()) {
      def.short_name = spec[comma + 1];
    }
  } else {
    def.long_name = spec;
  }
}

options_description::adder& options_description::adder::operator()(
    const std::string& name_spec, const std::string& desc)
{
  option_definition def;
  parse_name_spec(name_spec, def);
  def.description = desc;
  parent_.add(std::move(def));
  return *this;
}

// -------------------------------------------------------------------
//  options_description
// -------------------------------------------------------------------

void options_description::add(option_definition def)
{
  if (!def.long_name.empty()) {
    long_index_[def.long_name] = options_.size();
  }
  if (def.short_name != '\0') {
    short_index_[def.short_name] = options_.size();
  }
  options_.push_back(std::move(def));
}

const option_definition* options_description::find_long(
    const std::string& name) const
{
  auto it = long_index_.find(name);
  return it != long_index_.end() ? &options_[it->second] : nullptr;
}

const option_definition* options_description::find_short(char c) const
{
  auto it = short_index_.find(c);
  return it != short_index_.end() ? &options_[it->second] : nullptr;
}

std::ostream& operator<<(std::ostream& os, const options_description& desc)
{
  os << std::format("{}:\n", desc.caption_);
  for (const auto& opt : desc.options_) {
    const auto name_part = (opt.short_name != '\0')
        ? std::format("-{} [ --{} ]", opt.short_name, opt.long_name)
        : std::format("     --{}", opt.long_name);

    const auto value_part = (opt.takes_value && !opt.has_implicit) ? " arg"
        : (opt.takes_value && opt.has_implicit)                    ? " [=arg]"
                                                                   : "";

    os << std::format("  {}{}  {}\n", name_part, value_part, opt.description);
  }
  return os;
}

// -------------------------------------------------------------------
//  positional_options_description
// -------------------------------------------------------------------

void positional_options_description::add(const std::string& name, int count)
{
  name_ = name;
  count_ = count;
}

// -------------------------------------------------------------------
//  command_line_parser
// -------------------------------------------------------------------

command_line_parser::command_line_parser(int argc, char** argv)
{
  const std::span args {argv, static_cast<std::size_t>(argc)};

  if (argc > 1) {
    auto arg_view = args.subspan(1);
    tokens_ = arg_view
        | std::views::transform(
              [](const char* arg) { return std::string(arg); })
        | std::ranges::to<std::vector>();
  }
}

command_line_parser& command_line_parser::options(
    const options_description& desc)
{
  desc_ = &desc;
  return *this;
}

command_line_parser& command_line_parser::positional(
    const positional_options_description& pos)
{
  pos_ = &pos;
  return *this;
}

command_line_parser& command_line_parser::allow_unregistered()
{
  allow_unregistered_ = true;
  return *this;
}

void command_line_parser::parse_into(variables_map& vm) const
{
  if (desc_ == nullptr) {
    throw parse_error("no options_description set");
  }

  // Collect multi-value tokens per option name
  std::unordered_map<std::string, std::vector<std::string>> multi_tokens;

  for (std::size_t i = 0; i < tokens_.size(); ++i) {
    const auto& tok = tokens_[i];

    if (tok.starts_with("--")) {
      // Long option
      auto eq = tok.find('=', 2);
      std::string name = tok.substr(2, eq == std::string::npos ? eq : eq - 2);
      const auto* def = desc_->find_long(name);

      if (def == nullptr) {
        if (allow_unregistered_) {
          continue;
        }
        throw parse_error(std::format("unrecognised option '--{}'", name));
      }

      if (!def->takes_value) {
        // Pure flag
        vm[def->long_name].set(std::any {true});
      } else if (eq != std::string::npos) {
        // --name=value
        auto raw = tok.substr(eq + 1);
        store_value(vm, *def, raw, multi_tokens);
      } else if (def->has_implicit) {
        // Implicit value – check if next token looks like a value
        if (i + 1 < tokens_.size() && !tokens_[i + 1].starts_with("-")) {
          ++i;
          store_value(vm, *def, tokens_[i], multi_tokens);
        } else {
          vm[def->long_name].set(def->implicit_value);
        }
      } else {
        // Requires next token as value
        if (i + 1 >= tokens_.size()) {
          throw parse_error(
              std::format("option '--{}' requires a value", name));
        }
        ++i;
        store_value(vm, *def, tokens_[i], multi_tokens);
      }
    } else if (tok.starts_with("-") && tok.size() == 2) {
      // Short option
      const char sc = tok[1];
      const auto* def = desc_->find_short(sc);
      if (def == nullptr) {
        if (allow_unregistered_) {
          continue;
        }
        throw parse_error(
            std::format("unrecognised option '-{}'", std::string(1, sc)));
      }

      if (!def->takes_value) {
        vm[def->long_name].set(std::any {true});
      } else if (def->has_implicit) {
        if (i + 1 < tokens_.size() && !tokens_[i + 1].starts_with("-")) {
          ++i;
          store_value(vm, *def, tokens_[i], multi_tokens);
        } else {
          vm[def->long_name].set(def->implicit_value);
        }
      } else {
        if (i + 1 >= tokens_.size()) {
          throw parse_error(std::format("option '-{}' requires a value",
                                        std::string(1, sc)));
        }
        ++i;
        store_value(vm, *def, tokens_[i], multi_tokens);
      }
    } else {
      // Positional argument
      if (pos_ != nullptr && !pos_->name().empty()) {
        multi_tokens[pos_->name()].push_back(tok);
      } else if (!allow_unregistered_) {
        throw parse_error(
            std::format("unexpected positional argument: '{}'", tok));
      }
    }
  }

  // Finalise multi-value options (collect into vector<string>)
  for (auto& [name, vals] : multi_tokens) {
    const auto* def = desc_->find_long(name);
    if (def != nullptr && def->multi_value) {
      vm[name].set(std::any(std::move(vals)));
    }
  }
}

void command_line_parser::store_value(
    variables_map& vm,
    const option_definition& def,
    const std::string& raw,
    std::unordered_map<std::string, std::vector<std::string>>& multi)
{
  if (def.multi_value) {
    multi[def.long_name].push_back(raw);
  } else {
    vm[def.long_name].set(def.parser(raw));
  }
}

// -------------------------------------------------------------------
//  Convenience: store / notify
// -------------------------------------------------------------------

void store(const command_line_parser& parser, variables_map& vm)
{
  parser.parse_into(vm);
}

void notify(const variables_map& /*vm*/)
{
  // Nothing to do – kept for API compatibility.
}

}  // namespace gtopt::cli
