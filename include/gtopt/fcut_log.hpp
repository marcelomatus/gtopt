/**
 * @file      fcut_log.hpp
 * @brief     PLP-style feasibility-cut debug log (plpfact.log analogue)
 * @date      2026-07-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * PLP writes every feasibility-cut event to a dedicated `plpfact.log`
 * (gated by `FactDBL >= 1`): the infeasibility detection line, one
 * coefficient line per cut variable, the RHS, and the install
 * confirmation on the destination stage.  gtopt mirrors that here as
 * `gtopt_fcut.log` in the resolved `log_directory` (sibling of
 * `gtopt_<N>.log`), gated by `sddp_options.fcut_log` / `--fcut-log`.
 *
 * Record grammar (one line per record, grep-friendly keys, physical
 * units — the emitted `SparseRow`s are already physical-space):
 *
 * ```text
 * INFEASIBLE iter=<i> scene=<s> phase=<p> cycle=<n> status=<solver status>
 * mode: <aggregated|multi_cut|state_repair|farkas_recursive> iter=… dest=<p-1>
 * cut: <Class>:<uid>[:<name>] <var> col=<c> coef=<v>
 * rhs: <sense> <v>
 * INSTALLED iter=<i> scene=<s> phase=<p> dest=<p-1> rows=<n>
 * HOLGURAS iter=<i> scene=<s> phase=<p> mode=<m> fact_eps=<eps>
 * FAIL iter=<i> scene=<s> phase=<p> status=<st> reason=<why>
 * ROLLBACK iter=<i> scene=<s> rows=<n>
 * ```
 *
 * The `mode:` + `cut:`/`rhs:` + `INSTALLED` block of one event is
 * written as a single atomic record (forward passes run scene-parallel,
 * so per-line writes would interleave across scenes).
 */

#pragma once

#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include <gtopt/benders_cut.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

/**
 * @class FcutLogWriter
 * @brief Mutex-serialised writer for the `gtopt_fcut.log` debug file.
 *
 * Owned by `SDDPMethod` (stable lifetime across the scene-parallel
 * forward passes).  `configure()` is called once at `solve()` start —
 * before any pool task runs — after which `enabled()` is a const read.
 * The file is opened lazily on the first `write()` (append mode, so
 * cascade levels and hot-start reruns extend the same file with their
 * own session records instead of truncating earlier evidence).
 */
class FcutLogWriter
{
public:
  /// File name inside the resolved log directory — sibling of the
  /// `gtopt_<N>.log` file the standalone binary writes there.
  static constexpr std::string_view filename = "gtopt_fcut.log";

  FcutLogWriter() = default;
  FcutLogWriter(const FcutLogWriter&) = delete;
  FcutLogWriter& operator=(const FcutLogWriter&) = delete;
  FcutLogWriter(FcutLogWriter&&) = delete;
  FcutLogWriter& operator=(FcutLogWriter&&) = delete;
  ~FcutLogWriter() = default;

  /// Arm (or disarm) the writer.  Must be called before the parallel
  /// passes start; an empty @p log_directory disables the writer even
  /// when @p enabled is true.  Re-configuring closes any open file so
  /// the next write reopens under the new directory.
  void configure(bool enabled, std::string log_directory);

  /// True when records will be written.  Call sites use this to skip
  /// record assembly entirely on the (default) disabled path.
  [[nodiscard]] bool enabled() const noexcept { return m_enabled_; }

  /// Append one record (multi-line allowed) atomically and flush.
  /// A trailing newline is added when @p record lacks one.  No-op when
  /// disabled; disables itself with a WARN when the file cannot be
  /// opened.
  void write(std::string_view record);

private:
  bool m_enabled_ {false};
  std::string m_directory_ {};
  std::ofstream m_out_ {};
  std::mutex m_mutex_ {};
};

/// Format the `cut:` / `rhs:` lines of one cut-emission record.
///
/// One `cut:` line per coefficient of each row in @p cuts, resolved
/// against @p links by `source_col` to recover the element identity
/// (`Class:uid[:name] var`); coefficients on columns with no matching
/// link fall back to the bare column index.  Each row closes with one
/// `rhs: <sense> <value>` line — `>=`-rows report `lowb`, `<=`-rows
/// (multi_cut can emit both senses) report `uppb`.  Values are printed
/// with `{:.17g}` so the log round-trips the installed doubles exactly,
/// like PLP's `cf:`/`rhsi:` lines in `plpfact.log`.
[[nodiscard]] std::string format_fcut_cut_lines(
    std::span<const StateVarLink> links, std::span<const SparseRow> cuts);

}  // namespace gtopt
