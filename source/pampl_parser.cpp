/**
 * @file      pampl_parser.cpp
 * @brief     Implementation of the pseudo-AMPL (.pampl) constraint file parser
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtopt/pampl_parser.hpp>

namespace gtopt
{

namespace
{

// ── Character helpers ────────────────────────────────────────────────────────

bool is_ident_start(char c)
{
  return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_cont(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// ── Low-level scanner ────────────────────────────────────────────────────────

/**
 * @brief Minimal scanner for PAMPL header tokens only.
 *
 * The scanner is only responsible for tokenizing the optional header
 * (`[inactive] constraint NAME ["desc"] :`).  The constraint expression
 * itself is extracted as a raw substring (everything up to the next `;`) and
 * passed verbatim to ConstraintParser.
 */
class Scanner
{
public:
  explicit Scanner(std::string_view src) noexcept
      : m_src_(src)
  {
  }

  [[nodiscard]] std::size_t pos() const noexcept { return m_pos_; }
  [[nodiscard]] bool at_end() const noexcept { return m_pos_ >= m_src_.size(); }

  /// Skip whitespace and `#` / `//` line comments.
  void skip_ws_comments()
  {
    while (!at_end()) {
      const char c = m_src_[m_pos_];
      if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        ++m_pos_;
        continue;
      }
      // Line comment: # ...
      if (c == '#') {
        while (!at_end() && m_src_[m_pos_] != '\n') {
          ++m_pos_;
        }
        continue;
      }
      // Line comment: // ...
      if (c == '/' && m_pos_ + 1 < m_src_.size() && m_src_[m_pos_ + 1] == '/') {
        while (!at_end() && m_src_[m_pos_] != '\n') {
          ++m_pos_;
        }
        continue;
      }
      break;
    }
  }

  /// Peek at the next non-ws character without advancing.
  [[nodiscard]] char peek_char()
  {
    skip_ws_comments();
    return at_end() ? '\0' : m_src_[m_pos_];
  }

  /// Read an identifier; returns empty if current position is not ident_start.
  [[nodiscard]] std::string read_ident()
  {
    skip_ws_comments();
    if (at_end() || !is_ident_start(m_src_[m_pos_])) {
      return {};
    }
    const std::size_t start = m_pos_;
    while (!at_end() && is_ident_cont(m_src_[m_pos_])) {
      ++m_pos_;
    }
    return std::string {m_src_.substr(start, m_pos_ - start)};
  }

  /// Read a quoted string literal (`"..."` or `'...'`).  Returns the
  /// unquoted content, or empty if the next token is not a string.
  [[nodiscard]] std::string read_string()
  {
    skip_ws_comments();
    if (at_end()) {
      return {};
    }
    const char quote = m_src_[m_pos_];
    if (quote != '"' && quote != '\'') {
      return {};
    }
    ++m_pos_;  // skip opening quote
    std::string result;
    while (!at_end() && m_src_[m_pos_] != quote) {
      if (m_src_[m_pos_] == '\\' && m_pos_ + 1 < m_src_.size()) {
        ++m_pos_;  // skip backslash
      }
      result += m_src_[m_pos_++];
    }
    if (at_end()) {
      throw std::invalid_argument("PAMPL: unterminated string literal");
    }
    ++m_pos_;  // skip closing quote
    return result;
  }

  /// Consume a specific character; return true if it matched.
  bool consume(char expected)
  {
    skip_ws_comments();
    if (!at_end() && m_src_[m_pos_] == expected) {
      ++m_pos_;
      return true;
    }
    return false;
  }

  /// Return the rest of the source from current position up to (not
  /// including) the next `;`, advancing past the `;`.  Throws if no `;`
  /// is found before end of input.
  [[nodiscard]] std::string read_until_semicolon()
  {
    const std::size_t start = m_pos_;
    while (!at_end() && m_src_[m_pos_] != ';') {
      ++m_pos_;
    }
    if (at_end()) {
      throw std::invalid_argument(
          "PAMPL: missing ';' at end of constraint expression");
    }
    std::string expr {m_src_.substr(start, m_pos_ - start)};
    ++m_pos_;  // consume ';'
    return expr;
  }

private:
  std::string_view m_src_;
  std::size_t m_pos_ {0};
};

// ── PAMPL parse logic ────────────────────────────────────────────────────────

std::vector<UserConstraint> do_parse(std::string_view source, Uid start_uid)
{
  Scanner sc(source);
  std::vector<UserConstraint> result;

  Uid next_uid = start_uid;

  while (true) {
    sc.skip_ws_comments();
    if (sc.at_end()) {
      break;
    }

    // ── Optional header: [inactive] constraint NAME ["desc"] : ──────────────
    bool active = true;
    std::string name;
    std::string description;
    bool has_header = false;

    // Save position so we can rewind if "inactive"/"constraint" are not found
    const std::size_t saved_pos = sc.pos();

    // Try to read a header keyword
    const std::string first_word = sc.read_ident();

    if (first_word == "inactive") {
      // Must be followed by "constraint"
      const std::string second_word = sc.read_ident();
      if (second_word != "constraint") {
        throw std::invalid_argument(
            std::format("PAMPL: expected 'constraint' after 'inactive', "
                        "got '{}'",
                        second_word));
      }
      active = false;
      has_header = true;
    } else if (first_word == "constraint") {
      has_header = true;
    } else {
      // Not a header – rewind and treat as bare expression
      // (The Scanner has no rewind method, but we can reconstruct the
      // expression by re-reading from saved_pos.)
      // Re-create scanner from saved position:
      sc = Scanner {source.substr(saved_pos)};
      // (next_uid and result are still valid)
    }

    if (has_header) {
      // Read constraint name (mandatory after 'constraint')
      name = sc.read_ident();
      if (name.empty()) {
        throw std::invalid_argument(
            "PAMPL: expected identifier (constraint name) after 'constraint'");
      }

      // Optional description string
      sc.skip_ws_comments();
      if (!sc.at_end() && (sc.peek_char() == '"' || sc.peek_char() == '\'')) {
        description = sc.read_string();
      }

      // Mandatory colon
      if (!sc.consume(':')) {
        throw std::invalid_argument(std::format(
            "PAMPL: expected ':' after constraint name '{}'", name));
      }
    }

    // ── Expression up to ';' ─────────────────────────────────────────────────
    std::string expr = sc.read_until_semicolon();

    // Trim leading/trailing whitespace from expression
    const auto expr_start = expr.find_first_not_of(" \t\r\n");
    const auto expr_end = expr.find_last_not_of(" \t\r\n");
    if (expr_start == std::string::npos) {
      // Empty expression (e.g., a bare ';') — skip silently
      continue;
    }
    expr = expr.substr(expr_start, expr_end - expr_start + 1);

    // Auto-generate name if none was provided
    if (name.empty()) {
      name = std::format("uc_{}", static_cast<int>(next_uid));
    }

    UserConstraint uc;
    uc.uid = next_uid++;
    uc.name = std::move(name);
    uc.expression = std::move(expr);
    if (!active) {
      uc.active = false;
    }
    if (!description.empty()) {
      uc.description = std::move(description);
    }

    result.push_back(std::move(uc));
  }

  return result;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::vector<UserConstraint> PamplParser::parse_file(std::string_view filepath,
                                                    Uid start_uid)
{
  const std::ifstream file {std::string {filepath}};
  if (!file) {
    throw std::runtime_error(
        std::format("PAMPL: cannot open file '{}'", filepath));
  }
  std::ostringstream buf;
  buf << file.rdbuf();
  return parse(buf.str(), start_uid);
}

std::vector<UserConstraint> PamplParser::parse(std::string_view source,
                                               Uid start_uid)
{
  return do_parse(source, start_uid);
}

}  // namespace gtopt
