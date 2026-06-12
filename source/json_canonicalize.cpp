/**
 * @file      json_canonicalize.cpp
 * @brief     Implementation of the JSON key-alias rewriter
 * @copyright BSD-3-Clause
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtopt/json_canonicalize.hpp>
#include <gtopt/names_registry.hpp>
#include <gtopt/unit_registry.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Skip over a JSON string literal whose opening quote is at
/// `pos`. Returns the index of the character after the closing quote.
/// Handles `\\`, `\"`, and other JSON escape sequences without
/// interpreting them.
[[nodiscard]] std::size_t skip_string(std::string_view s, std::size_t pos)
{
  // pos points at the opening '"'
  ++pos;  // step past opening quote
  while (pos < s.size()) {
    const char c = s[pos];
    if (c == '\\') {
      // Skip escape pair; `\u00XX` is also two chars after `\` which
      // we conservatively step over the same way (the first char `u`
      // is consumed, then the loop advances over the hex digits as
      // regular non-quote chars).
      pos += 2;
      continue;
    }
    if (c == '"') {
      return pos + 1;  // index past closing quote
    }
    ++pos;
  }
  return pos;
}

/// Read the unescaped content of a JSON string literal starting at
/// position `pos` (the opening quote).  Returns the content as a
/// `string_view` into `s` — valid only as long as `s` is alive.
/// For aliases (the lookup key in the registry) we currently expect
/// ASCII names with no escape sequences, so we slice the raw bytes
/// between the quotes and skip the lookup when an escape is present.
[[nodiscard]] std::string_view raw_string_content(std::string_view s,
                                                  std::size_t open_quote_pos,
                                                  std::size_t close_quote_pos)
{
  // [open_quote_pos+1 .. close_quote_pos-1)
  if (close_quote_pos <= open_quote_pos + 1) {
    return {};
  }
  return s.substr(open_quote_pos + 1, close_quote_pos - open_quote_pos - 2);
}

/// True if the string content has no JSON escape sequences (so the
/// raw bytes between the quotes equal the logical string value).
[[nodiscard]] bool is_simple_string(std::string_view content) noexcept
{
  return !content.contains('\\');
}

}  // namespace

std::string canonicalize_json_keys(std::string_view json_text,
                                   const NamesRegistry& registry,
                                   std::string_view enforce_dialect)
{
  // Quick reject: if the registry is empty, return a copy.
  if (registry.size() == 0) {
    return std::string {json_text};
  }

  // Set of aliases we have already warned about so the log stays
  // compact: one line per misfit alias regardless of how many times it
  // appears in the document.  Local to this call.
  std::unordered_set<std::string> warned_aliases;

  std::string out;
  out.reserve(json_text.size());

  // Track whether the next quoted string is a key (object-key
  // position) or a value.
  //
  // We maintain a small stack of container states.  Top of stack
  // tells us whether we're inside an object (then quoted strings
  // after `{` or `,` are keys until the next `:`).
  struct Frame
  {
    bool in_object;  // true if frame opened with '{'
    bool expecting_key;  // true if the next quoted string is a key
  };
  std::vector<Frame> stack;
  stack.reserve(16);

  std::size_t i = 0;
  while (i < json_text.size()) {
    const char c = json_text[i];

    switch (c) {
      case '{': {
        out.push_back(c);
        stack.push_back({.in_object = true, .expecting_key = true});
        ++i;
        continue;
      }
      case '[': {
        out.push_back(c);
        stack.push_back({.in_object = false, .expecting_key = false});
        ++i;
        continue;
      }
      case '}':
      case ']': {
        out.push_back(c);
        if (!stack.empty()) {
          stack.pop_back();
        }
        ++i;
        continue;
      }
      case ',': {
        out.push_back(c);
        if (!stack.empty() && stack.back().in_object) {
          stack.back().expecting_key = true;
        }
        ++i;
        continue;
      }
      case ':': {
        out.push_back(c);
        if (!stack.empty() && stack.back().in_object) {
          stack.back().expecting_key = false;
        }
        ++i;
        continue;
      }
      case '"': {
        const std::size_t end = skip_string(json_text, i);
        const bool is_key = !stack.empty() && stack.back().in_object
            && stack.back().expecting_key;

        if (is_key) {
          const auto content = raw_string_content(json_text, i, end);
          if (is_simple_string(content)) {
            if (const auto canonical = registry.canonical_for(content);
                canonical.has_value())
            {
              // Optional input warn: alias's source dialect differs
              // from the user-enforced dialect.  Hot path only emits
              // the lookup + once-per-alias warn when enforce_dialect
              // is non-empty.
              if (!enforce_dialect.empty()) {
                const auto alias_dialect = registry.dialect_for(content);
                if (alias_dialect.has_value()
                    && *alias_dialect != enforce_dialect)
                {
                  const std::string alias_key {content};
                  if (warned_aliases.insert(alias_key).second) {
                    // Escalate to error when the per-dialect unit
                    // registry confirms the two dialects disagree on
                    // the unit of this canonical — the numeric value
                    // in the JSON almost certainly needs a conversion
                    // the user did not perform.  Falls back to a
                    // plain warn when units are unknown or ambiguous.
                    const auto& units = UnitRegistry::instance();
                    const auto src_unit = units.class_agnostic_unit_for(
                        *canonical, *alias_dialect);
                    const auto tgt_unit = units.class_agnostic_unit_for(
                        *canonical, enforce_dialect);
                    if (src_unit.has_value() && tgt_unit.has_value()
                        && *src_unit != *tgt_unit)
                    {
                      spdlog::error(
                          "naming-dialect: UNIT MISMATCH on input alias "
                          "'{}' (dialect '{}', unit '{}') vs "
                          "--naming-dialect '{}' (unit '{}') for canonical "
                          "'{}' — the value will be parsed as-is; manual "
                          "conversion required",
                          content,
                          *alias_dialect,
                          *src_unit,
                          enforce_dialect,
                          *tgt_unit,
                          *canonical);
                    } else {
                      spdlog::warn(
                          "naming-dialect: input alias '{}' belongs to dialect "
                          "'{}' but --naming-dialect is '{}' (canonical: '{}')",
                          content,
                          *alias_dialect,
                          enforce_dialect,
                          *canonical);
                    }
                  }
                }
              }
              out.push_back('"');
              out.append(*canonical);
              out.push_back('"');
              i = end;
              continue;
            }
          }
        }
        // Default: copy verbatim.
        out.append(json_text.substr(i, end - i));
        i = end;
        continue;
      }
      default: {
        out.push_back(c);
        ++i;
        continue;
      }
    }
  }
  return out;
}

std::string canonicalize_json_keys(std::string_view json_text,
                                   const NamesRegistry& registry)
{
  return canonicalize_json_keys(json_text, registry, /*enforce_dialect=*/"");
}

std::string canonicalize_json_keys(std::string_view json_text)
{
  return canonicalize_json_keys(json_text, NamesRegistry::instance());
}

std::string canonicalize_json_keys(std::string_view json_text,
                                   std::string_view enforce_dialect)
{
  return canonicalize_json_keys(
      json_text, NamesRegistry::instance(), enforce_dialect);
}

// ─── Inverse pass: canonical → dialect alias (output rename) ───────────────

std::string decanonicalize_json_keys(std::string_view json_text,
                                     const NamesRegistry& registry,
                                     std::string_view dialect)
{
  if (dialect.empty() || registry.size() == 0) {
    return std::string {json_text};
  }

  std::string out;
  out.reserve(json_text.size());

  struct Frame
  {
    bool in_object;
    bool expecting_key;
  };
  std::vector<Frame> stack;
  stack.reserve(16);

  std::size_t i = 0;
  while (i < json_text.size()) {
    const char c = json_text[i];
    switch (c) {
      case '{':
        out.push_back(c);
        stack.push_back({.in_object = true, .expecting_key = true});
        ++i;
        continue;
      case '[':
        out.push_back(c);
        stack.push_back({.in_object = false, .expecting_key = false});
        ++i;
        continue;
      case '}':
      case ']':
        out.push_back(c);
        if (!stack.empty()) {
          stack.pop_back();
        }
        ++i;
        continue;
      case ',':
        out.push_back(c);
        if (!stack.empty() && stack.back().in_object) {
          stack.back().expecting_key = true;
        }
        ++i;
        continue;
      case ':':
        out.push_back(c);
        if (!stack.empty() && stack.back().in_object) {
          stack.back().expecting_key = false;
        }
        ++i;
        continue;
      case '"': {
        const std::size_t end = skip_string(json_text, i);
        const bool is_key = !stack.empty() && stack.back().in_object
            && stack.back().expecting_key;
        if (is_key) {
          const auto content = raw_string_content(json_text, i, end);
          if (is_simple_string(content)) {
            if (const auto alias = registry.alias_for(content, dialect);
                alias.has_value())
            {
              out.push_back('"');
              out.append(*alias);
              out.push_back('"');
              i = end;
              continue;
            }
          }
        }
        out.append(json_text.substr(i, end - i));
        i = end;
        continue;
      }
      default:
        out.push_back(c);
        ++i;
        continue;
    }
  }
  return out;
}

std::string decanonicalize_json_keys(std::string_view json_text,
                                     std::string_view dialect)
{
  return decanonicalize_json_keys(
      json_text, NamesRegistry::instance(), dialect);
}

}  // namespace gtopt
