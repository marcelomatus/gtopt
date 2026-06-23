/**
 * @file      cascade_progress.cpp
 * @brief     Read/write the cascade resume checkpoint files.
 *
 * Hand-rolled JSON I/O (no `daw_json_link`) keeps this translation unit
 * self-contained: the `LpContext` variant — embedded in `StateTarget` —
 * is serialized as a tagged 5-tuple, which is awkward to express in
 * daw's contract-based templates.  The on-disk format is small and
 * stable; a manual parser is cheaper to maintain than a custom
 * `json_data_contract` specialization for `std::variant`.
 */

#include <array>
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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtopt/cascade_method.hpp>  // StateTarget
#include <gtopt/cascade_progress.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/uid.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ─── LpContext tag encoding ────────────────────────────────────────────────
//
// Stable on-disk integer tags.  Order MUST match the variant alternative
// order in `lp_context.hpp::LpContext` (so std::variant's `index()` aligns
// with the tag).  Changing an existing tag silently corrupts old
// checkpoint files — only ever append new tags at the end.

constexpr std::size_t kCtxMonostate = 0;
constexpr std::size_t kCtxStage = 1;
constexpr std::size_t kCtxBlock = 2;
constexpr std::size_t kCtxBlockEx = 3;
constexpr std::size_t kCtxScenePhase = 4;
constexpr std::size_t kCtxIteration = 5;
constexpr std::size_t kCtxAperture = 6;
constexpr std::size_t kCtxPhase = 7;

static_assert(std::variant_size_v<LpContext> == 8,
              "LpContext alternative count changed — update tag table");

/// Encode `LpContext` to a fixed-width int array: [tag, v0, v1, v2, v3].
/// Unused slots are zero.  The per-alternative arms write identical
/// patterns into `out[1..4]` whenever two variants share an arity (e.g.
/// Stage/ScenePhase both fill two slots); a templated lambda over the
/// tuple's index sequence collapses those clones — no NOLINT needed.
[[nodiscard]] auto context_to_array(const LpContext& ctx)
    -> std::array<std::int64_t, 5>
{
  std::array<std::int64_t, 5> out {};
  out[0] = static_cast<std::int64_t>(ctx.index());
  std::visit(
      [&]<typename T>(const T& v)
      {
        if constexpr (!std::is_same_v<T, std::monostate>) {
          [&]<std::size_t... Is>(std::index_sequence<Is...>)
          {
            ((out[Is + 1U] = [&]() -> std::int64_t
              {
                using Elem = std::tuple_element_t<Is, T>;
                if constexpr (std::is_integral_v<Elem>) {
                  return std::get<Is>(v);
                } else {
                  return value_of(std::get<Is>(v));
                }
              }()),
             ...);
          }(std::make_index_sequence<std::tuple_size_v<T>> {});
        }
      },
      ctx);
  return out;
}

/// Inverse of `context_to_array`.  Returns `monostate` on unknown tags
/// (forward-compat with future variant alternatives).
[[nodiscard]] auto context_from_array(const std::array<std::int64_t, 5>& a)
    -> LpContext
{
  const auto tag = static_cast<std::size_t>(a[0]);
  switch (tag) {
    case kCtxMonostate:
      return std::monostate {};
    case kCtxStage:
      return StageContext {
          make_uid<Scenario>(static_cast<uid_t>(a[1])),
          make_uid<Stage>(static_cast<uid_t>(a[2])),
      };
    case kCtxBlock:
      return BlockContext {
          make_uid<Scenario>(static_cast<uid_t>(a[1])),
          make_uid<Stage>(static_cast<uid_t>(a[2])),
          make_uid<Block>(static_cast<uid_t>(a[3])),
      };
    case kCtxBlockEx:
      return BlockExContext {
          make_uid<Scenario>(static_cast<uid_t>(a[1])),
          make_uid<Stage>(static_cast<uid_t>(a[2])),
          make_uid<Block>(static_cast<uid_t>(a[3])),
          static_cast<int>(a[4]),
      };
    case kCtxScenePhase:
      return ScenePhaseContext {
          make_uid<Scene>(static_cast<uid_t>(a[1])),
          make_uid<Phase>(static_cast<uid_t>(a[2])),
      };
    case kCtxIteration:
      return IterationContext {
          make_uid<Scene>(static_cast<uid_t>(a[1])),
          make_uid<Phase>(static_cast<uid_t>(a[2])),
          make_uid<Iteration>(static_cast<uid_t>(a[3])),
          static_cast<int>(a[4]),
      };
    case kCtxAperture:
      return ApertureContext {
          make_uid<Scene>(static_cast<uid_t>(a[1])),
          make_uid<Phase>(static_cast<uid_t>(a[2])),
          make_uid<Scenario>(static_cast<uid_t>(a[3])),
          static_cast<int>(a[4]),
      };
    case kCtxPhase:
      return PhaseContext {
          make_uid<Scene>(static_cast<uid_t>(a[1])),
          make_uid<Phase>(static_cast<uid_t>(a[2])),
          Uid {static_cast<uid_t>(a[3])},
      };
    default:
      SPDLOG_WARN(
          "CascadeProgress: unknown LpContext tag {} — defaulting to monostate",
          tag);
      return std::monostate {};
  }
}

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
    out += std::format(R"("cuts_file": "{}", )", json_escape(l.cuts_file));
    out += std::format(R"("state_targets_file": "{}")",
                       json_escape(l.state_targets_file));
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
    } else if (key == "state_targets_file") {
      level.state_targets_file = parser.parse_string();
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

// ─── StateTargets: serialize ───────────────────────────────────────────────

auto save_state_targets(std::span<const StateTarget> targets,
                        const std::filesystem::path& path)
    -> std::expected<void, Error>
{
  std::string out;
  out.reserve(128 + (targets.size() * 192U));
  out += "{\n  \"schema_version\": 1,\n  \"targets\": [\n";
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto& t = targets[i];
    const auto ctx = context_to_array(t.context);
    out += "    {";
    out += std::format(R"("class_name": "{}", )", json_escape(t.class_name));
    out += std::format(R"("col_name": "{}", )", json_escape(t.col_name));
    out += std::format("\"uid\": {}, ", static_cast<std::int64_t>(t.uid));
    out += std::format("\"scene_index\": {}, ",
                       static_cast<std::int64_t>(t.scene_index));
    out += std::format("\"phase_index\": {}, ",
                       static_cast<std::int64_t>(t.phase_index));
    out += std::format("\"target_value\": {:.17g}, ", t.target_value);
    out += std::format("\"var_scale\": {:.17g}, ", t.var_scale);
    out += std::format("\"context\": [{}, {}, {}, {}, {}]",
                       ctx[0],
                       ctx[1],
                       ctx[2],
                       ctx[3],
                       ctx[4]);
    out += '}';
    if (i + 1 < targets.size()) {
      out += ',';
    }
    out += '\n';
  }
  out += "  ]\n}\n";
  return write_atomic(path, out);
}

auto load_state_targets(const std::filesystem::path& path)
    -> std::expected<std::vector<StateTarget>, Error>
{
  auto text = read_file(path);
  if (!text) {
    return std::unexpected(text.error());
  }
  JsonParser parser(*text);
  std::vector<StateTarget> targets;
  if (!parser.consume('{')) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message =
            std::format("StateTargets: {}: {}", path.string(), parser.error()),
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
    if (key == "targets") {
      if (!parser.consume('[')) {
        break;
      }
      while (!parser.failed()) {
        if (parser.peek() == ']') {
          (void)parser.consume(']');
          break;
        }
        if (!parser.consume('{')) {
          break;
        }
        StateTarget t;
        std::array<std::int64_t, 5> ctx_arr {};
        bool have_context = false;
        while (!parser.failed()) {
          if (parser.peek() == '}') {
            (void)parser.consume('}');
            break;
          }
          const auto tkey = parser.parse_string();
          if (parser.failed()) {
            break;
          }
          if (!parser.consume(':')) {
            break;
          }
          if (tkey == "class_name") {
            t.class_name = parser.parse_string();
          } else if (tkey == "col_name") {
            t.col_name = parser.parse_string();
          } else if (tkey == "uid") {
            t.uid = static_cast<Uid>(parser.parse_int64());
          } else if (tkey == "scene_index") {
            t.scene_index =
                SceneIndex {static_cast<Index>(parser.parse_int64())};
          } else if (tkey == "phase_index") {
            t.phase_index =
                PhaseIndex {static_cast<Index>(parser.parse_int64())};
          } else if (tkey == "target_value") {
            t.target_value = parser.parse_double();
          } else if (tkey == "var_scale") {
            t.var_scale = parser.parse_double();
          } else if (tkey == "context") {
            if (!parser.consume('[')) {
              break;
            }
            for (auto& slot : ctx_arr) {
              slot = parser.parse_int64();
              if (&slot != &ctx_arr.back() && !parser.consume(',')) {
                break;
              }
            }
            (void)parser.consume(']');
            have_context = true;
          } else {
            parser.skip_value();
          }
          if (parser.peek() == ',') {
            (void)parser.consume(',');
          }
        }
        if (have_context) {
          t.context = context_from_array(ctx_arr);
        }
        targets.push_back(std::move(t));
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
        .message =
            std::format("StateTargets: {}: {}", path.string(), parser.error()),
    });
  }
  return targets;
}

}  // namespace gtopt
