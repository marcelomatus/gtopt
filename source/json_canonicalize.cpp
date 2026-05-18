/**
 * @file      json_canonicalize.cpp
 * @brief     Implementation of the JSON key-alias rewriter
 * @copyright BSD-3-Clause
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/json_canonicalize.hpp>
#include <gtopt/names_registry.hpp>

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
                                   const NamesRegistry& registry)
{
  // Quick reject: if the registry is empty, return a copy.
  if (registry.size() == 0) {
    return std::string {json_text};
  }

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

std::string canonicalize_json_keys(std::string_view json_text)
{
  return canonicalize_json_keys(json_text, NamesRegistry::instance());
}

}  // namespace gtopt
