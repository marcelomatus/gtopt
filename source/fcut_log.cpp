/**
 * @file      fcut_log.cpp
 * @brief     PLP-style feasibility-cut debug log implementation
 * @date      2026-07-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <format>

#include <gtopt/fcut_log.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

void FcutLogWriter::configure(bool enabled, std::string log_directory)
{
  const std::scoped_lock lock(m_mutex_);
  if (m_out_.is_open()) {
    m_out_.close();
  }
  m_directory_ = std::move(log_directory);
  m_enabled_ = enabled && !m_directory_.empty();
}

void FcutLogWriter::write(std::string_view record)
{
  if (!m_enabled_) {
    return;
  }
  const std::scoped_lock lock(m_mutex_);
  if (!m_out_.is_open()) {
    std::error_code mkdir_ec;
    std::filesystem::create_directories(m_directory_, mkdir_ec);
    const auto path = std::filesystem::path(m_directory_) / filename;
    m_out_.open(path, std::ios::out | std::ios::app);
    if (!m_out_.is_open()) {
      spdlog::warn("fcut_log: cannot open {} — disabling the fcut log",
                   path.string());
      m_enabled_ = false;
      return;
    }
  }
  m_out_ << record;
  if (record.empty() || record.back() != '\n') {
    m_out_ << '\n';
  }
  // Flush per record: the log's whole purpose is live post-mortem of
  // runs that may die mid-iteration, and the write rate (one record
  // per infeasibility event) is far too low for buffering to matter.
  m_out_.flush();
}

std::string format_fcut_cut_lines(std::span<const StateVarLink> links,
                                  std::span<const SparseRow> cuts)
{
  // `>=`-rows have uppb = +DblMax; `<=`-rows have lowb = -DblMax.
  // Use half-max thresholds so equilibrated near-max values still
  // classify correctly.
  constexpr double kHalfMax = SparseRow::DblMax / 2;

  const auto link_for = [&](ColIndex col) -> const StateVarLink*
  {
    for (const auto& link : links) {
      if (link.source_col == col) {
        return &link;
      }
    }
    return nullptr;
  };

  std::string out;
  for (const auto& cut : cuts) {
    for (const auto& [col, coef] : cut.cmap) {
      if (const auto* link = link_for(col); link != nullptr) {
        out += std::format("cut: {}:{}{} {} col={} coef={:.17g}\n",
                           link->class_name.full_name(),
                           link->uid,
                           link->name.empty() ? std::string {}
                                              : std::format(":{}", link->name),
                           link->col_name,
                           col,
                           coef);
      } else {
        out += std::format("cut: ?:? ? col={} coef={:.17g}\n", col, coef);
      }
    }
    if (cut.lowb > -kHalfMax) {
      out += std::format("rhs: >= {:.17g}\n", cut.lowb);
    } else {
      out += std::format("rhs: <= {:.17g}\n", cut.uppb);
    }
  }
  return out;
}

}  // namespace gtopt
