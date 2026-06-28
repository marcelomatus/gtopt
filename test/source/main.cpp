// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      main.cpp
 * @brief     doctest entry point + a clean per-test spdlog default logger
 * @copyright BSD-3-Clause
 *
 * We own `main()` (rather than DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN) so the
 * whole test binary runs against one known-good, **synchronous** spdlog
 * default logger.  Some tests exercise `gtopt_main()`, which installs an
 * `async_logger` as the global default and never restores it; left in
 * place, that breaks every later `LogCapture`-based test (the async backend
 * thread delivers messages *after* the synchronous `contains()` check has
 * already run, and mutating a live async logger's sinks is racy).
 *
 * A doctest listener re-installs our synchronous logger before every test
 * case (and every subcase re-entry), so no test can inherit a polluted
 * global logger from a predecessor.  `LogCapture` then swaps sinks on this
 * synchronous logger and captures deterministically.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include <memory>

#include <doctest/doctest.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace
{

/// The single synchronous logger the whole test binary runs against.
/// Created once; re-installed as the default before each test case.
std::shared_ptr<spdlog::logger> gtopt_test_logger()
{
  static const std::shared_ptr<spdlog::logger> logger = []
  {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto lg = std::make_shared<spdlog::logger>("gtopt_test", std::move(sink));
    lg->set_level(spdlog::level::info);  // spdlog's normal default level
    return lg;
  }();
  return logger;
}

/// Restore our synchronous logger as spdlog's default.
void install_test_logger()
{
  spdlog::set_default_logger(gtopt_test_logger());
}

/// doctest listener that resets the default logger before each test case,
/// neutralising any global-logger pollution left by a previous test (e.g.
/// the async logger installed by `gtopt_main()`).
struct LoggerResetListener : doctest::IReporter
{
  explicit LoggerResetListener(const doctest::ContextOptions&) {}

  void test_case_start(const doctest::TestCaseData&) override
  {
    install_test_logger();
  }
  void test_case_reenter(const doctest::TestCaseData&) override
  {
    install_test_logger();
  }

  // Remaining IReporter hooks are unused.
  void report_query(const doctest::QueryData&) override {}
  void test_run_start() override {}
  void test_run_end(const doctest::TestRunStats&) override {}
  void test_case_end(const doctest::CurrentTestCaseStats&) override {}
  void test_case_exception(const doctest::TestCaseException&) override {}
  void subcase_start(const doctest::SubcaseSignature&) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData&) override {}
  void log_message(const doctest::MessageData&) override {}
  void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace

DOCTEST_REGISTER_LISTENER("gtopt_logger_reset", 1, LoggerResetListener);

int main(int argc, char** argv)
{
  install_test_logger();

  doctest::Context context(argc, argv);
  return context.run();
}
