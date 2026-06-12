// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      log_capture.hpp
 * @brief     spdlog ring-buffer sink helper for doctest assertions
 * @date      2026-04-10
 *
 * Provides a small RAII wrapper that installs a `ringbuffer_sink` on
 * spdlog's default logger for the lifetime of the scope, so a test can
 * check whether a specific error/warning message was emitted without
 * scraping stderr.  On destruction the original sinks and log level are
 * restored.
 *
 * Usage:
 * @code
 *   LogCapture logs;
 *   call_something_that_logs_an_error();
 *   REQUIRE(logs.contains("non-convex use of abs"));
 * @endcode
 */

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

namespace gtopt::test
{

class LogCapture
{
public:
  /// Install a fresh ringbuffer sink on the default logger.
  /// `capacity` is the max number of retained messages; older messages
  /// are dropped FIFO-style when the buffer wraps.
  explicit LogCapture(std::size_t capacity = 256)
      : m_sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(capacity))
      , m_saved_sinks_(spdlog::default_logger()->sinks())
      , m_saved_level_(spdlog::default_logger()->level())
  {
    auto logger = spdlog::default_logger();
    logger->sinks().clear();
    logger->sinks().push_back(m_sink_);
    // Ensure info/warn/error/debug all get captured.
    logger->set_level(spdlog::level::trace);
  }

  ~LogCapture()
  {
    auto logger = spdlog::default_logger();
    logger->sinks() = m_saved_sinks_;
    logger->set_level(m_saved_level_);
  }

  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  LogCapture(LogCapture&&) = delete;
  LogCapture& operator=(LogCapture&&) = delete;

  /// All captured messages, in insertion order, formatted as plain text.
  [[nodiscard]] std::vector<std::string> messages() const
  {
    return m_sink_->last_formatted();
  }

  /// Convenience: return true if any captured message contains @p needle.
  [[nodiscard]] bool contains(std::string_view needle) const
  {
    const auto msgs = m_sink_->last_formatted();
    return std::ranges::any_of(
        msgs, [needle](const std::string& m) { return m.contains(needle); });
  }

  /// Drop all captured messages without tearing down the sink.
  void clear()
  {
    m_sink_ =
        std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(m_sink_capacity_);
    auto logger = spdlog::default_logger();
    logger->sinks().clear();
    logger->sinks().push_back(m_sink_);
  }

private:
  std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> m_sink_;
  std::vector<spdlog::sink_ptr> m_saved_sinks_;
  spdlog::level::level_enum m_saved_level_ {spdlog::level::info};
  std::size_t m_sink_capacity_ {256};
};

}  // namespace gtopt::test
