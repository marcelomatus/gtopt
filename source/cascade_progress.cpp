/**
 * @file      cascade_progress.cpp
 * @brief     Read/write the cascade resume checkpoint files.
 *
 * Hand-rolled JSON I/O (no `daw_json_link`) keeps this translation unit
 * self-contained.  The on-disk format is small and stable; a manual
 * parser is cheaper to maintain than custom `json_data_contract`
 * specializations.
 */

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtopt/cascade_progress.hpp>

namespace gtopt
{

namespace
{

[[nodiscard]] constexpr auto status_to_string(CascadeLevelStatus s) noexcept
    -> std::string_view
{
  switch (s) {
    case CascadeLevelStatus::pending:
      return "pending";
    case CascadeLevelStatus::in_progress:
      return "in_progress";
    case CascadeLevelStatus::done:
      return "done";
  }
  return "pending";
}

[[nodiscard]] auto string_to_status(std::string_view s) noexcept
    -> CascadeLevelStatus
{
  if (s == "done") {
    return CascadeLevelStatus::done;
  }
  if (s == "in_progress") {
    return CascadeLevelStatus::in_progress;
  }
  return CascadeLevelStatus::pending;
}

// ─── JSON escape ───────────────────────────────────────────────────────────

[[nodiscard]] auto json_escape(std::string_view s) -> std::string
{
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

// ─── Minimal JSON parser ────────────────────────────────────────────────────
//
// Supports objects, arrays, strings (with \"  \\  \n  \r  \t  \uXXXX escapes
// for the chars we actually emit), numbers (integers + IEEE floats), bools,
// null.  No Unicode normalisation beyond what `json_escape` produces.

class JsonParser
{
public:
  explicit JsonParser(std::string_view text) noexcept
      : m_text_(text)
  {
  }

  [[nodiscard]] auto error() const noexcept -> const std::string&
  {
    return m_error_;
  }

  [[nodiscard]] auto failed() const noexcept -> bool
  {
    return !m_error_.empty();
  }

  void skip_ws() noexcept
  {
    while (m_pos_ < m_text_.size()
           && (std::isspace(static_cast<unsigned char>(m_text_[m_pos_])) != 0))
    {
      ++m_pos_;
    }
  }

  [[nodiscard]] auto peek() noexcept -> char
  {
    skip_ws();
    return m_pos_ < m_text_.size() ? m_text_[m_pos_] : '\0';
  }

  auto consume(char expected) -> bool
  {
    skip_ws();
    if (m_pos_ < m_text_.size() && m_text_[m_pos_] == expected) {
      ++m_pos_;
      return true;
    }
    set_error(std::format("expected '{}' at pos {}", expected, m_pos_));
    return false;
  }

  auto parse_string() -> std::string
  {
    skip_ws();
    if (m_pos_ >= m_text_.size() || m_text_[m_pos_] != '"') {
      set_error(std::format("expected string at pos {}", m_pos_));
      return {};
    }
    ++m_pos_;
    std::string out;
    while (m_pos_ < m_text_.size() && m_text_[m_pos_] != '"') {
      const char c = m_text_[m_pos_];
      if (c == '\\') {
        ++m_pos_;
        if (m_pos_ >= m_text_.size()) {
          set_error("unterminated escape");
          return {};
        }
        const char esc = m_text_[m_pos_];
        switch (esc) {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case '/':
            out += '/';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
            // Skip 4-digit hex; we only emit control chars this way.
            if (m_pos_ + 4 >= m_text_.size()) {
              set_error("truncated \\u escape");
              return {};
            }
            {
              unsigned code = 0;
              const auto hex = m_text_.substr(m_pos_ + 1, 4);
              auto [p, ec] = std::from_chars(
                  hex.data(),
                  hex.data()
                      + hex.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                  code,
                  16);
              if (ec != std::errc {}
                  || p
                      != hex.data()
                          + hex.size())  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
              {
                set_error("invalid \\u escape");
                return {};
              }
              if (code < 0x80U) {
                out += static_cast<char>(code);
              } else {
                // Lossless storage isn't required for our payloads.
                out += '?';
              }
              m_pos_ += 4;
            }
            break;
          default:
            set_error(std::format("unknown escape \\{}", esc));
            return {};
        }
        ++m_pos_;
      } else {
        out += c;
        ++m_pos_;
      }
    }
    if (m_pos_ >= m_text_.size()) {
      set_error("unterminated string");
      return {};
    }
    ++m_pos_;  // closing quote
    return out;
  }

  auto parse_int64() -> std::int64_t
  {
    skip_ws();
    const auto start = m_pos_;
    if (m_pos_ < m_text_.size()
        && (m_text_[m_pos_] == '-' || m_text_[m_pos_] == '+'))
    {
      ++m_pos_;
    }
    while (m_pos_ < m_text_.size()
           && (std::isdigit(static_cast<unsigned char>(m_text_[m_pos_])) != 0))
    {
      ++m_pos_;
    }
    if (m_pos_ == start) {
      set_error(std::format("expected integer at pos {}", m_pos_));
      return 0;
    }
    std::int64_t value = 0;
    const auto span = m_text_.substr(start, m_pos_ - start);
    auto [ptr, ec] = std::from_chars(
        span.data(),
        span.data()
            + span.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        value);
    (void)ptr;
    if (ec != std::errc {}) {
      set_error("integer parse failed");
      return 0;
    }
    return value;
  }

  auto parse_double() -> double
  {
    skip_ws();
    const auto start = m_pos_;
    if (m_pos_ < m_text_.size()
        && (m_text_[m_pos_] == '-' || m_text_[m_pos_] == '+'))
    {
      ++m_pos_;
    }
    while (m_pos_ < m_text_.size()) {
      const char c = m_text_[m_pos_];
      if ((std::isdigit(static_cast<unsigned char>(c)) != 0) || c == '.'
          || c == 'e' || c == 'E' || c == '+' || c == '-')
      {
        ++m_pos_;
        continue;
      }
      break;
    }
    if (m_pos_ == start) {
      set_error(std::format("expected number at pos {}", m_pos_));
      return 0.0;
    }
    double value = 0.0;
    const auto span = m_text_.substr(start, m_pos_ - start);
    auto [ptr, ec] = std::from_chars(
        span.data(),
        span.data()
            + span.size(),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        value);
    (void)ptr;
    if (ec != std::errc {}) {
      set_error("number parse failed");
      return 0.0;
    }
    return value;
  }

  auto parse_bool() -> bool
  {
    skip_ws();
    if (m_pos_ + 4 <= m_text_.size() && m_text_.substr(m_pos_, 4) == "true") {
      m_pos_ += 4;
      return true;
    }
    if (m_pos_ + 5 <= m_text_.size() && m_text_.substr(m_pos_, 5) == "false") {
      m_pos_ += 5;
      return false;
    }
    set_error("expected bool");
    return false;
  }

  /// Skip an arbitrary JSON value (objects, arrays, primitives) so the
  /// caller can ignore unknown keys.  Recursion is bounded by the input
  /// nesting depth (a few levels for our payloads); not protected against
  /// pathological inputs because the file is only ever written by us.
  // NOLINTNEXTLINE(misc-no-recursion)
  void skip_value()
  {
    const char c = peek();
    if (c == '{') {
      ++m_pos_;
      while (true) {
        if (peek() == '}') {
          ++m_pos_;
          return;
        }
        (void)parse_string();
        if (!consume(':')) {
          return;
        }
        skip_value();
        if (peek() == ',') {
          ++m_pos_;
        }
      }
    } else if (c == '[') {
      ++m_pos_;
      while (true) {
        if (peek() == ']') {
          ++m_pos_;
          return;
        }
        skip_value();
        if (peek() == ',') {
          ++m_pos_;
        }
      }
    } else if (c == '"') {
      (void)parse_string();
    } else if (c == 't' || c == 'f') {
      (void)parse_bool();
    } else if (c == 'n') {
      if (m_pos_ + 4 <= m_text_.size() && m_text_.substr(m_pos_, 4) == "null") {
        m_pos_ += 4;
      } else {
        set_error("expected null");
      }
    } else {
      (void)parse_double();
    }
  }

  void set_error(std::string msg)
  {
    if (m_error_.empty()) {
      m_error_ = std::move(msg);
    }
  }

private:
  std::string_view m_text_;
  std::size_t m_pos_ {0};
  std::string m_error_;
};

// ─── Atomic write helper ───────────────────────────────────────────────────

[[nodiscard]] auto write_atomic(const std::filesystem::path& path,
                                const std::string& payload)
    -> std::expected<void, Error>
{
  namespace fs = std::filesystem;
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    // create_directories returns false if the dir already exists; only
    // an `ec` set to a real error indicates failure.
    if (ec) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("create_directories('{}') failed: {}",
                                 path.parent_path().string(),
                                 ec.message()),
      });
    }
  }
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("open '{}' for write failed", tmp),
      });
    }
    out << payload;
    if (!out) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("write to '{}' failed", tmp),
      });
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    // Fall back to copy+remove (e.g. cross-device rename on tmpfs).
    std::error_code ec2;
    fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec2);
    fs::remove(tmp, ec2);
    if (ec2) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("rename('{}' -> '{}') failed: {}",
                                 tmp,
                                 path.string(),
                                 ec.message()),
      });
    }
  }
  return {};
}

[[nodiscard]] auto read_file(const std::filesystem::path& path)
    -> std::expected<std::string, Error>
{
  const std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("open '{}' for read failed", path.string()),
    });
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in.eof() && in.fail()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("read '{}' failed", path.string()),
    });
  }
  return ss.str();
}

}  // namespace

// ─── CascadeProgress: serialize ────────────────────────────────────────────

namespace
{

[[nodiscard]] auto progress_to_json(const CascadeProgress& p) -> std::string
{
  std::string out;
  out.reserve(256 + (p.levels.size() * 256U));
  out += std::format("{{\n  \"schema_version\": {},\n", p.schema_version);
  out += std::format("  \"run_id\": \"{}\",\n", json_escape(p.run_id));
  out += "  \"levels\": [\n";
  for (std::size_t i = 0; i < p.levels.size(); ++i) {
    const auto& l = p.levels[i];
    out += "    {";
    out += std::format("\"index\": {}, ", l.index);
    out += std::format(R"("name": "{}", )", json_escape(l.name));
    out += std::format(R"("status": "{}", )", status_to_string(l.status));
    out += std::format("\"converged\": {}, ", l.converged ? "true" : "false");
    out += std::format("\"iters\": {}, ", l.iters);
    out += std::format("\"global_iter_after\": {}, ", l.global_iter_after);
    out += std::format(R"("cuts_file": "{}")", json_escape(l.cuts_file));
    out += '}';
    if (i + 1 < p.levels.size()) {
      out += ',';
    }
    out += '\n';
  }
  out += "  ]\n}\n";
  return out;
}

[[nodiscard]] auto parse_progress_level(JsonParser& parser)
    -> CascadeProgressLevel
{
  CascadeProgressLevel level;
  if (!parser.consume('{')) {
    return level;
  }
  while (!parser.failed()) {
    if (parser.peek() == '}') {
      (void)parser.consume('}');
      break;
    }
    const auto key = parser.parse_string();
    if (parser.failed()) {
      return level;
    }
    if (!parser.consume(':')) {
      return level;
    }
    if (key == "index") {
      level.index = static_cast<std::size_t>(parser.parse_int64());
    } else if (key == "name") {
      level.name = parser.parse_string();
    } else if (key == "status") {
      level.status = string_to_status(parser.parse_string());
    } else if (key == "converged") {
      level.converged = parser.parse_bool();
    } else if (key == "iters") {
      level.iters = static_cast<int>(parser.parse_int64());
    } else if (key == "global_iter_after") {
      level.global_iter_after = static_cast<Index>(parser.parse_int64());
    } else if (key == "cuts_file") {
      level.cuts_file = parser.parse_string();
    } else {
      parser.skip_value();
    }
    if (parser.peek() == ',') {
      (void)parser.consume(',');
    }
  }
  return level;
}

}  // namespace

auto save_cascade_progress(const CascadeProgress& progress,
                           const std::filesystem::path& path)
    -> std::expected<void, Error>
{
  return write_atomic(path, progress_to_json(progress));
}

auto load_cascade_progress(const std::filesystem::path& path)
    -> std::expected<CascadeProgress, Error>
{
  auto text = read_file(path);
  if (!text) {
    return std::unexpected(text.error());
  }
  JsonParser parser(*text);
  CascadeProgress result;
  if (!parser.consume('{')) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "CascadeProgress: {}: {}", path.string(), parser.error()),
    });
  }
  while (!parser.failed()) {
    if (parser.peek() == '}') {
      (void)parser.consume('}');
      break;
    }
    const auto key = parser.parse_string();
    if (parser.failed()) {
      break;
    }
    if (!parser.consume(':')) {
      break;
    }
    if (key == "schema_version") {
      result.schema_version = static_cast<int>(parser.parse_int64());
    } else if (key == "run_id") {
      result.run_id = parser.parse_string();
    } else if (key == "levels") {
      if (!parser.consume('[')) {
        break;
      }
      while (!parser.failed()) {
        if (parser.peek() == ']') {
          (void)parser.consume(']');
          break;
        }
        result.levels.push_back(parse_progress_level(parser));
        if (parser.peek() == ',') {
          (void)parser.consume(',');
        }
      }
    } else {
      parser.skip_value();
    }
    if (parser.peek() == ',') {
      (void)parser.consume(',');
    }
  }
  if (parser.failed()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "CascadeProgress: {}: {}", path.string(), parser.error()),
    });
  }
  return result;
}

}  // namespace gtopt
