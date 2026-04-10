/**
 * @file      lp_fingerprint.cpp
 * @brief     LP structural fingerprint computation and JSON output
 * @date      Mon Apr  7 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>

#include <gtopt/lp_fingerprint.hpp>

namespace gtopt
{

// ── Context type name mapping
// ─────────────────────────────────────────────────

auto context_type_name(const LpContext& context) -> std::string_view
{
  return std::visit(
      []<typename T>(const T& /*unused*/) -> std::string_view
      {
        if constexpr (std::same_as<T, std::monostate>) {
          return "monostate";
        } else if constexpr (std::same_as<T, StageContext>) {
          return "StageContext";
        } else if constexpr (std::same_as<T, BlockContext>) {
          return "BlockContext";
        } else if constexpr (std::same_as<T, BlockExContext>) {
          return "BlockExContext";
        } else if constexpr (std::same_as<T, ScenePhaseContext>) {
          return "ScenePhaseContext";
        } else if constexpr (std::same_as<T, IterationContext>) {
          return "IterationContext";
        } else if constexpr (std::same_as<T, ApertureContext>) {
          return "ApertureContext";
        } else {
          return "unknown";
        }
      },
      context);
}

// ── Minimal SHA-256 implementation
// ──────────────────────────────────────────── Self-contained; avoids adding
// OpenSSL as a project dependency.

namespace
{

constexpr std::array<uint32_t, 64> k_sha256 = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t rotr(uint32_t x, unsigned n)
{
  return (x >> n) | (x << (32 - n));
}

auto sha256(const std::string& input) -> std::string
{
  // Pre-processing: convert input to padded message blocks
  auto msg_len = input.size();
  const uint64_t bit_len = msg_len * 8;

  // Pad: append 0x80, then zeros, then 64-bit big-endian length
  const size_t padded_len = ((msg_len + 9 + 63) / 64) * 64;
  std::vector<uint8_t> msg(padded_len, 0);
  std::memcpy(msg.data(), input.data(), msg_len);
  msg[msg_len] = 0x80;
  for (size_t i = 0; i < 8; ++i) {
    msg[padded_len - 1 - i] =
        static_cast<uint8_t>((bit_len >> (i * 8U)) & 0xFFU);
  }

  // Initialize hash values
  std::array<uint32_t, 8> h = {
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };

  // Process each 512-bit block
  for (size_t offset = 0; offset < padded_len; offset += 64) {
    std::array<uint32_t, 64> w {};
    for (size_t i = 0; i < 16; ++i) {
      const auto base = offset + (i * 4);
      w.at(i) = (static_cast<uint32_t>(msg[base]) << 24U)
          | (static_cast<uint32_t>(msg[base + 1]) << 16U)
          | (static_cast<uint32_t>(msg[base + 2]) << 8U)
          | static_cast<uint32_t>(msg[base + 3]);
    }
    for (size_t i = 16; i < 64; ++i) {
      auto s0 =
          rotr(w.at(i - 15), 7) ^ rotr(w.at(i - 15), 18) ^ (w.at(i - 15) >> 3U);
      auto s1 =
          rotr(w.at(i - 2), 17) ^ rotr(w.at(i - 2), 19) ^ (w.at(i - 2) >> 2U);
      w.at(i) = w.at(i - 16) + s0 + w.at(i - 7) + s1;
    }

    auto [a, b, c, d, e, f, g, hh] = h;
    for (size_t i = 0; i < 64; ++i) {
      auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      auto ch = (e & f) ^ (~e & g);
      auto temp1 = hh + s1 + ch + k_sha256.at(i) + w.at(i);
      auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      auto maj = (a & b) ^ (a & c) ^ (b & c);
      auto temp2 = s0 + maj;

      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  // Format as hex string
  std::string result;
  result.reserve(64);
  for (auto val : h) {
    result += std::format("{:08x}", val);
  }
  return result;
}

/// Compute SHA-256 hash of a sorted template vector.
auto hash_template(const std::vector<FingerprintEntry>& entries) -> std::string
{
  std::string data;
  for (const auto& e : entries) {
    data += e.class_name;
    data += '\0';
    data += e.variable_name;
    data += '\0';
    data += e.context_type;
    data += '\n';
  }
  return sha256(data);
}

}  // namespace

auto sha256_hex(const std::string& input) -> std::string
{
  return sha256(input);
}

// ── Fingerprint computation
// ───────────────────────────────────────────────────

auto compute_lp_fingerprint(const std::vector<SparseCol>& cols,
                            const std::vector<SparseRow>& rows) -> LpFingerprint
{
  LpFingerprint fingerprint;

  // -- Collect column template and stats --
  std::set<FingerprintEntry> col_set;
  fingerprint.stats.total_cols = cols.size();

  for (const auto& col : cols) {
    if (col.class_name.empty()
        || std::holds_alternative<std::monostate>(col.context))
    {
      ++fingerprint.untracked_cols;
      continue;
    }
    col_set.insert(FingerprintEntry {
        .class_name = std::string(col.class_name),
        .variable_name = std::string(col.variable_name),
        .context_type = std::string(context_type_name(col.context)),
    });
    ++fingerprint.stats.cols_by_class[std::string(col.class_name)];
  }
  fingerprint.col_template.assign(col_set.begin(), col_set.end());

  // -- Collect row template and stats --
  std::set<FingerprintEntry> row_set;
  fingerprint.stats.total_rows = rows.size();

  for (const auto& row : rows) {
    if (row.class_name.empty()
        || std::holds_alternative<std::monostate>(row.context))
    {
      ++fingerprint.untracked_rows;
      continue;
    }
    row_set.insert(FingerprintEntry {
        .class_name = std::string(row.class_name),
        .variable_name = std::string(row.constraint_name),
        .context_type = std::string(context_type_name(row.context)),
    });
    ++fingerprint.stats.rows_by_class[std::string(row.class_name)];
  }
  fingerprint.row_template.assign(row_set.begin(), row_set.end());

  // -- Compute hashes --
  fingerprint.col_hash = hash_template(fingerprint.col_template);
  fingerprint.row_hash = hash_template(fingerprint.row_template);
  fingerprint.structural_hash =
      sha256(fingerprint.col_hash + fingerprint.row_hash);

  return fingerprint;
}

// ── JSON output
// ───────────────────────────────────────────────────────────────

namespace
{

/// Escape a string for JSON output (handles quotes and backslashes).
auto json_escape(std::string_view s) -> std::string
{
  std::string result;
  result.reserve(s.size());
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      result += '\\';
    }
    result += c;
  }
  return result;
}

}  // namespace

void write_lp_fingerprint(const LpFingerprint& fingerprint,
                          const std::string& filepath,
                          int scene_uid,
                          int phase_uid)
{
  // Ensure parent directory exists
  if (auto parent = std::filesystem::path(filepath).parent_path();
      !parent.empty())
  {
    std::filesystem::create_directories(parent);
  }

  std::ofstream out(filepath);

  out << "{\n";
  out << std::format("  \"version\": 1,\n");
  out << std::format("  \"scene_uid\": {},\n", scene_uid);
  out << std::format("  \"phase_uid\": {},\n", phase_uid);

  // -- structural section --
  out << "  \"structural\": {\n";

  // columns
  out << "    \"columns\": {\n";
  out << "      \"template\": [\n";
  for (size_t i = 0; i < fingerprint.col_template.size(); ++i) {
    const auto& e = fingerprint.col_template[i];
    out << std::format(
        "        {{\"class\": \"{}\", \"variable\": \"{}\", "
        "\"context_type\": \"{}\"}}",
        json_escape(e.class_name),
        json_escape(e.variable_name),
        json_escape(e.context_type));
    out << (i + 1 < fingerprint.col_template.size() ? ",\n" : "\n");
  }
  out << "      ],\n";
  out << std::format("      \"untracked_count\": {},\n",
                     fingerprint.untracked_cols);
  out << std::format("      \"hash\": \"{}\"\n", fingerprint.col_hash);
  out << "    },\n";

  // rows
  out << "    \"rows\": {\n";
  out << "      \"template\": [\n";
  for (size_t i = 0; i < fingerprint.row_template.size(); ++i) {
    const auto& e = fingerprint.row_template[i];
    out << std::format(
        "        {{\"class\": \"{}\", \"constraint\": \"{}\", "
        "\"context_type\": \"{}\"}}",
        json_escape(e.class_name),
        json_escape(e.variable_name),
        json_escape(e.context_type));
    out << (i + 1 < fingerprint.row_template.size() ? ",\n" : "\n");
  }
  out << "      ],\n";
  out << std::format("      \"untracked_count\": {},\n",
                     fingerprint.untracked_rows);
  out << std::format("      \"hash\": \"{}\"\n", fingerprint.row_hash);
  out << "    },\n";

  out << std::format("    \"hash\": \"{}\"\n", fingerprint.structural_hash);
  out << "  },\n";

  // -- stats section --
  out << "  \"stats\": {\n";
  out << std::format("    \"total_cols\": {},\n", fingerprint.stats.total_cols);
  out << std::format("    \"total_rows\": {},\n", fingerprint.stats.total_rows);

  // cols_by_class
  out << "    \"cols_by_class\": {\n";
  {
    size_t i = 0;
    for (const auto& [cls, count] : fingerprint.stats.cols_by_class) {
      out << std::format("      \"{}\": {}", json_escape(cls), count);
      out << (++i < fingerprint.stats.cols_by_class.size() ? ",\n" : "\n");
    }
  }
  out << "    },\n";

  // rows_by_class
  out << "    \"rows_by_class\": {\n";
  {
    size_t i = 0;
    for (const auto& [cls, count] : fingerprint.stats.rows_by_class) {
      out << std::format("      \"{}\": {}", json_escape(cls), count);
      out << (++i < fingerprint.stats.rows_by_class.size() ? ",\n" : "\n");
    }
  }
  out << "    }\n";

  out << "  }\n";
  out << "}\n";
}

}  // namespace gtopt
