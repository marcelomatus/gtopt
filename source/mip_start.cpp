/**
 * @file      mip_start.cpp
 * @brief     Initial-MIP-solution (warm-start) pipeline implementation
 * @date      2026-06-23
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>

#include <arrow/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/domain_rules.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/mip_start.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace detail
{

/// Round a relaxed value to its nearest feasible integer, biased by
/// `threshold`: a relaxed binary at `v` rounds up iff `v >= threshold`
/// (`floor(v + 1 - threshold)`), then clamp to `[lb, ub]`.
[[nodiscard]] double round_with_threshold(double v,
                                          double threshold,
                                          double lb,
                                          double ub) noexcept
{
  const double r = std::floor(v + (1.0 - threshold));
  return std::clamp(r, lb, ub);
}

/// Stage 1 (round) + stage 2 (domain rules: commitment repair) shared
/// by every base generator.  Rounds the relaxation's integer columns and runs
/// the default `DomainRulePipeline` (min-up/down, … — see
/// domain_rules.hpp) over the result; continuous columns keep their
/// relaxation value.  @return the dense raw candidate start (empty iff the
/// relaxation produced no primal — the caller skips).
/// Stage 2 — apply the seed + domain-rule pipeline in place on `start`.
/// Loads the optional external commitment seed (registered FIRST so it is the
/// base the remaining rules repair) and runs the default `DomainRulePipeline`
/// (min-up/down, commitment-logic v/w, …).  Shared by the round-from-relaxation
/// generator and the seed-only (no-relaxation) path.
void apply_seed_and_domain_rules(MipStartContext& ctx,
                                 std::vector<double>& start,
                                 std::string_view generator_name)
{
  SeedCommitmentMap seed;
  if (const auto& sf = ctx.opts.seed_solution_file;
      sf.has_value() && !sf->empty())
  {
    if (auto loaded = load_seed_commitment(*sf)) {
      seed = std::move(*loaded);
    } else {
      spdlog::warn("MIP-start[{}]: seed_solution_file '{}' not loaded: {}",
                   generator_name,
                   *sf,
                   loaded.error().message);
    }
  }
  const DomainRulePipeline pipeline =
      make_default_domain_rules(ctx.opts.domain_rules, std::move(seed));
  const int flipped = pipeline.apply(start,
                                     DomainRuleContext {
                                         .commitments = ctx.commitments,
                                         .injections = ctx.injections,
                                     });
  if (flipped > 0) {
    spdlog::info("MIP-start[{}]: domain rules flipped {} status values",
                 generator_name,
                 flipped);
  }
}

[[nodiscard]] std::vector<double> rounded_start_with_rules(
    MipStartContext& ctx, std::string_view generator_name)
{
  auto& li = ctx.li;
  const auto sol = li.get_col_sol_raw();
  std::vector<double> start(sol.begin(), sol.end());
  if (sol.empty()) {
    return start;
  }
  // Stage 1 — generic rounding of the integer columns.
  const auto lb = li.get_col_low_raw();
  const auto ub = li.get_col_upp_raw();
  const double threshold = ctx.opts.round.threshold.value_or(0.5);
  for (const int i : ctx.int_cols) {
    const auto u = static_cast<std::size_t>(i);
    const double l = (u < lb.size()) ? lb[u] : 0.0;
    const double h = (u < ub.size()) ? ub[u] : 1.0;
    start[u] = round_with_threshold(sol[u], threshold, l, h);
  }
  // Stage 2 — domain rules (power-system knowledge: min-up/down, …).
  apply_seed_and_domain_rules(ctx, start, generator_name);
  return start;
}

/// Seed-only start: no LP relaxation.  Builds a zero base of width `ncols` and
/// lets the seed + commitment-logic domain rules set every integer commitment
/// column directly.  Continuous columns stay at 0 (a sparse-injecting backend
/// like CPLEX drops them and completes the dispatch as its warm root LP).
[[nodiscard]] std::vector<double> seed_only_start(
    MipStartContext& ctx, std::string_view generator_name)
{
  const auto ncols = static_cast<std::size_t>(ctx.li.get_numcols());
  std::vector<double> start(ncols, 0.0);
  apply_seed_and_domain_rules(ctx, start, generator_name);
  return start;
}

}  // namespace detail

namespace
{

/// P1 generator: round the integer columns of the solved LP relaxation,
/// leaving the continuous (dispatch) columns at their relaxation value.
/// Storage-safe — operates on the whole-horizon relaxation, no time chunking.
class WarmStartGenerator final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "warmstart";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    // Stage 1 (round) + stage 2 (domain rules); nothing else.
    auto start = detail::rounded_start_with_rules(ctx, "warmstart");
    if (start.empty()) {
      return std::nullopt;
    }
    return start;
  }
};

/// File generator: replay an integer solution dumped by a previous solve
/// (`dump_integer_solution`).  The dumped `<index> <value>` lines carry the
/// integer-column raw values found by another solver; this overlays them onto
/// THIS solver's own LP-relaxation primal (the continuous columns stay at the
/// relaxation value, exactly like `warmstart`, but the integer columns come
/// from the file instead of rounding).  Enables a cross-solver hand-off: e.g.
/// HiGHS (strong MIP heuristics) finds a feasible incumbent and dumps it, then
/// cuOpt replays it as its start.  Both runs build the identical deterministic
/// flat LP, so raw column indices match 1:1 — validated by the `ncols` header
/// and a per-index integer-column membership check (no silent skip on
/// mismatch).
class FileMipStart final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "file";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    const auto& path = ctx.opts.from_file;
    if (!path.has_value() || path->empty()) {
      spdlog::warn(
          "MIP-start[file]: no mip_start.from_file configured; nothing to "
          "replay");
      return std::nullopt;
    }
    std::ifstream in(*path);
    if (!in) {
      spdlog::error("MIP-start[file]: cannot open '{}' for reading", *path);
      return std::nullopt;
    }

    const auto sol = ctx.li.get_col_sol_raw();  // RAW relaxation primal = base
    if (sol.empty()) {
      return std::nullopt;
    }
    const auto ncols = static_cast<std::size_t>(ctx.li.get_numcols());
    const std::unordered_set<int> int_set(ctx.int_cols.begin(),
                                          ctx.int_cols.end());

    std::vector<double> start(sol.begin(), sol.end());
    std::size_t file_ncols = 0;
    std::size_t placed = 0;
    std::size_t skipped = 0;
    std::string token;
    while (in >> token) {
      if (token == "ncols") {
        in >> file_ncols;
        if (file_ncols != ncols) {
          spdlog::error(
              "MIP-start[file]: column-count mismatch (dump ncols={}, this LP "
              "ncols={}); refusing to replay a start built for a different LP",
              file_ncols,
              ncols);
          return std::nullopt;
        }
        continue;
      }
      if (token == "nint") {
        in >> token;  // value consumed, informational only
        continue;
      }
      if (!token.empty() && token.front() == '#') {
        std::getline(in, token);  // comment line — discard remainder
        continue;
      }
      // Otherwise `token` is a column index; the next token is its value.
      int idx = -1;
      double value = 0.0;
      try {
        idx = std::stoi(token);
      } catch (const std::exception&) {
        continue;  // not an index token; skip
      }
      if (!(in >> value)) {
        break;
      }
      const auto u = static_cast<std::size_t>(idx);
      if (idx < 0 || u >= ncols || !int_set.contains(idx)) {
        ++skipped;
        continue;
      }
      start[u] = value;
      ++placed;
    }

    if (placed == 0) {
      spdlog::error(
          "MIP-start[file]: dump '{}' yielded no usable integer values "
          "(placed=0, skipped={})",
          *path,
          skipped);
      return std::nullopt;
    }
    spdlog::info(
        "MIP-start[file]: replayed {} integer values from '{}' ({} skipped, "
        "{} integer cols in this LP)",
        placed,
        *path,
        skipped,
        ctx.int_cols.size());
    return start;
  }
};

/// Report the saturated / binding constraints (nonzero dual) of the solved LP
/// relaxation — solver-agnostic, read from the row duals.
void report_saturated_rows(LinearInterface& li, int max_items)
{
  const auto duals = li.get_row_dual_raw();
  if (duals.empty()) {
    spdlog::info("MIP-start: no row duals available; cannot report saturation");
    return;
  }
  constexpr double tol = 1e-7;

  struct Hit
  {
    double mag {};
    double dual {};
    int row {};
  };
  std::vector<Hit> hits;
  for (std::size_t i = 0; i < duals.size(); ++i) {
    const double d = duals[i];
    if (std::abs(d) > tol) {
      hits.push_back({
          .mag = std::abs(d),
          .dual = d,
          .row = static_cast<int>(i),
      });
    }
  }
  if (hits.empty()) {
    spdlog::info("MIP-start: LP relaxation has no binding (saturated) rows");
    return;
  }
  std::ranges::sort(hits,
                    [](const Hit& a, const Hit& b) { return a.mag > b.mag; });

  const int n = std::min<int>(static_cast<int>(hits.size()), max_items);
  spdlog::info(
      "MIP-start: {} binding (saturated) rows in LP relaxation (top {}):",
      hits.size(),
      n);
  for (int k = 0; k < n; ++k) {
    const auto& hit = hits[static_cast<std::size_t>(k)];
    const auto* lbl = li.row_label_at(RowIndex {hit.row});
    if (lbl != nullptr) {
      spdlog::info("  [{}] {}:{} dual={:.6g}",
                   hit.row,
                   lbl->class_name,
                   lbl->constraint_name,
                   hit.dual);
    } else {
      spdlog::info("  [{}] row_{} dual={:.6g}", hit.row, hit.row, hit.dual);
    }
  }
}

}  // namespace

std::unique_ptr<MipStartGenerator> make_mip_start_generator(
    const MipStartOptions& opts)
{
  // `from_file` set → replay a dumped integer solution; otherwise the default
  // round + domain-rules candidate.
  if (opts.from_file.has_value() && !opts.from_file->empty()) {
    return std::make_unique<FileMipStart>();
  }
  return std::make_unique<WarmStartGenerator>();
}

std::expected<void, Error> dump_integer_solution(const LinearInterface& li,
                                                 const std::string& path)
{
  const auto sol = li.get_col_sol_raw();
  if (sol.empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = "MIP-start dump: no primal solution to dump",
    });
  }
  const int ncols = static_cast<int>(li.get_numcols());

  // Collect integer columns + their raw values (integrality must still be
  // intact at the call site — before any dual-recovery relaxation).
  std::vector<std::pair<int, double>> ints;
  ints.reserve((static_cast<std::size_t>(ncols) / 8) + 1);
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      ints.emplace_back(i, sol[static_cast<std::size_t>(i)]);
    }
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("MIP-start dump: cannot open '{}' for writing", path),
    });
  }
  out << "# gtopt mip_start integer solution (index value)\n";
  out << std::format("ncols {}\n", ncols);
  out << std::format("nint {}\n", ints.size());
  for (const auto& [idx, value] : ints) {
    // `{}` renders the shortest round-trippable representation of the double.
    out << std::format("{} {}\n", idx, value);
  }
  if (!out) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start dump: write to '{}' failed", path),
    });
  }
  spdlog::info(
      "MIP-start dump: wrote {} integer columns to '{}'", ints.size(), path);
  return {};
}

std::expected<SeedCommitmentMap, Error> load_seed_commitment(
    const std::string& path)
{
  // Reuse the Arrow CSV reader — the same path boundary_cuts.csv takes
  // (`sddp_cut_io.cpp`) — so header detection, CRLF, quoting, blank-line
  // skipping and type inference are handled by a battle-tested reader instead
  // of a bespoke parser.  Required header + columns: `generator_uid` (int),
  // `block_uid` (int), `u` (0/1 status).
  auto maybe_infile = arrow::io::ReadableFile::Open(path);
  if (!maybe_infile.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: cannot open '{}': {}",
                               path,
                               maybe_infile.status().ToString()),
    });
  }

  auto read_options = arrow::csv::ReadOptions::Defaults();
  auto parse_options = arrow::csv::ParseOptions::Defaults();
  auto convert_options = arrow::csv::ConvertOptions::Defaults();
  convert_options.column_types["generator_uid"] = arrow::int32();
  convert_options.column_types["block_uid"] = arrow::int32();
  convert_options.column_types["u"] = arrow::float64();
  convert_options.include_missing_columns = false;

  auto maybe_reader =
      arrow::csv::TableReader::Make(arrow::io::default_io_context(),
                                    *maybe_infile,
                                    read_options,
                                    parse_options,
                                    convert_options);
  if (!maybe_reader.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: unreadable CSV '{}': {}",
                               path,
                               maybe_reader.status().ToString()),
    });
  }
  auto maybe_table = (*maybe_reader)->Read();
  if (!maybe_table.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: parse failed on '{}': {}",
                               path,
                               maybe_table.status().ToString()),
    });
  }
  // Combine chunks so the three columns are single, row-aligned arrays.
  auto maybe_combined = (*maybe_table)->CombineChunks();
  if (!maybe_combined.ok()) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("MIP-start seed: combine failed on '{}': {}",
                               path,
                               maybe_combined.status().ToString()),
    });
  }
  const auto& table = *maybe_combined;

  const auto gcol = table->GetColumnByName("generator_uid");
  const auto bcol = table->GetColumnByName("block_uid");
  const auto ucol = table->GetColumnByName("u");
  if (gcol == nullptr || bcol == nullptr || ucol == nullptr) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "MIP-start seed '{}': need columns generator_uid, block_uid, u",
            path),
    });
  }

  SeedCommitmentMap seed;
  seed.reserve(static_cast<std::size_t>(table->num_rows()));
  for (int ci = 0; ci < gcol->num_chunks(); ++ci) {
    const auto ga =
        std::dynamic_pointer_cast<arrow::Int32Array>(gcol->chunk(ci));
    const auto ba =
        std::dynamic_pointer_cast<arrow::Int32Array>(bcol->chunk(ci));
    const auto ua =
        std::dynamic_pointer_cast<arrow::DoubleArray>(ucol->chunk(ci));
    if (!ga || !ba || !ua) {
      continue;
    }
    for (int64_t i = 0; i < ga->length(); ++i) {
      if (ga->IsValid(i) && ba->IsValid(i) && ua->IsValid(i)) {
        seed[seed_commitment_key(ga->Value(i), ba->Value(i))] = ua->Value(i);
      }
    }
  }
  spdlog::info("MIP-start seed: loaded {} (generator,block) statuses from '{}'",
               seed.size(),
               path);
  return seed;
}

std::expected<MipStartReport, Error> apply_mip_start(
    LinearInterface& li,
    const SolverOptions& base_opts,
    const MipStartOptions& opts,
    std::span<const CommitmentRunInfo> commitments,
    std::span<const PeakInjectionInfo> injections,
    const FlatLinearProblem* flat_lp)
{
  MipStartReport report;

  const bool enabled = opts.enabled.value_or(false);
  const bool relax_check = opts.relax.check.value_or(false);
  if (!enabled && !relax_check) {
    return report;  // feature off
  }

  // Snapshot the integer columns BEFORE relaxing — relaxation hides
  // integrality, so this must run first.
  std::vector<int> int_cols;
  const int ncols = static_cast<int>(li.get_numcols());
  for (int i = 0; i < ncols; ++i) {
    if (li.is_integer(ColIndex {i})) {
      int_cols.push_back(i);
    }
  }
  if (int_cols.empty()) {
    return report;  // pure LP — nothing to seed or diagnose
  }
  // Restore integrality via the backend's bulk path (one CPXchgprobtype on
  // CPLEX, a per-column loop otherwise).  Done in place (no clone) — cloning
  // the full monolithic LP per scene would re-trigger the known CPXcloneprob
  // global-side-effect crash.
  const auto restore_integrality = [&] { li.restore_integers(int_cols); };

  // ── Fast path: seed-only, no throwaway relaxation ───────────────────────
  // When `skip_relaxation` is set and a seed CSV fully specifies the integer
  // commitment, skip Stage 0 entirely: the relaxation's only surviving output
  // for a sparse-injecting backend (CPLEX) is the rounded integers, which the
  // seed overrides anyway — so solving it is a throwaway LP the MIP root then
  // repeats.  Build the seed start directly and inject; the backend completes
  // the continuous dispatch as its single warm root LP (effort defaults to
  // `solve_fixed` → dual simplex off the fixed integers, no crossover).
  if (enabled && opts.skip_relaxation.value_or(false)
      && opts.seed_solution_file.has_value()
      && !opts.seed_solution_file->empty())
  {
    MipStartContext ctx {
        .li = li,
        .relax_opts = base_opts,
        .int_cols = int_cols,
        .opts = opts,
        .commitments = commitments,
        .injections = injections,
        .flat_lp = flat_lp,
    };
    auto start = detail::seed_only_start(ctx, "seed");
    const auto effort =
        opts.inject.effort.value_or(MipStartEffort::solve_fixed);
    report.injected = li.set_mip_start(start, effort);
    report.source = "seed";
    spdlog::info(
        "MIP-start[seed]: {} ({} integer cols, effort={}, skip_relaxation — "
        "no relaxation solve)",
        report.injected ? "injected" : "backend declined",
        int_cols.size(),
        enum_name(effort));
    return report;
  }

  // Relaxation solve options: base overlaid with the relax-specific overrides
  // so the relaxation can use a different algorithm / params than the MIP.
  SolverOptions relax_opts = base_opts;
  if (opts.relax.solver_options.has_value()) {
    relax_opts.overlay(*opts.relax.solver_options);
  }

  // ── Stage A: solve & analyze the LP relaxation ──────────────────────────
  li.relax_integers();
  const auto rr = li.resolve(relax_opts);
  report.relaxation_solved = true;
  const bool feasible = rr.has_value() && li.is_optimal();
  report.relaxation_feasible = feasible;
  if (feasible) {
    report.relax_obj = li.get_obj_value();
  }

  if (!feasible) {
    const auto action =
        opts.relax.on_infeasible.value_or(RelaxInfeasibleAction::stop);
    if (action == RelaxInfeasibleAction::feasopt) {
      if (auto conflicts = li.diagnose_infeasibility()) {
        spdlog::error(
            "MIP-start: LP relaxation INFEASIBLE — {} conflicting constraints "
            "(minimal infeasible subsystem):",
            conflicts->size());
        for (const auto& c : *conflicts) {
          spdlog::error("  conflict: {}", c);
        }
      } else {
        spdlog::error(
            "MIP-start: LP relaxation INFEASIBLE — backend cannot diagnose the "
            "conflict on this solver");
      }
    }
    restore_integrality();
    if (action == RelaxInfeasibleAction::warn) {
      spdlog::warn(
          "MIP-start: LP relaxation infeasible; proceeding to the MIP solve "
          "anyway (on_infeasible=warn)");
      return report;
    }
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::string {"MIP-start: LP relaxation is infeasible "
                                "(on_infeasible="}
            + std::string {enum_name(action)} + ")",
    });
  }

  // Feasible: optional saturated-row report.
  if (opts.relax.report_saturated.value_or(false)) {
    report_saturated_rows(li, 50);
  }

  // ── Stage B: integer-solution generation & injection ────────────────────
  if (!enabled) {
    restore_integrality();  // diagnosis-only run (relax.check)
    return report;
  }

  auto gen = make_mip_start_generator(opts);
  if (!gen) {
    spdlog::warn("MIP-start: no generator available; skipping injection");
    restore_integrality();
    return report;
  }

  MipStartContext ctx {
      .li = li,
      .relax_opts = relax_opts,
      .int_cols = int_cols,
      .opts = opts,
      .commitments = commitments,
      .injections = injections,
      .flat_lp = flat_lp,
  };
  auto start = gen->generate(ctx);  // stage 1 (round) + stage 2 (domain rules)
  std::string source {gen->name()};

  // Stage 4 (optional): SCIP repair.  Composable with any base candidate and
  // any active solver — turns the round+rules candidate into a genuinely
  // feasible integer solution via SCIP's completesol/repair.  On failure (no
  // SCIP plugin / no flat LP / nothing feasible) keep the pre-SCIP candidate.
  if (start && opts.scip_repair.enabled.value_or(false)) {
    if (auto repaired = scip_repair_candidate(ctx, *start)) {
      start = std::move(repaired);
      source += "+scip";
    } else {
      spdlog::warn(
          "MIP-start[scip]: repair stage produced no result; keeping the "
          "round+rules candidate from '{}'",
          gen->name());
    }
  }

  // Re-establish integrality BEFORE injecting: the backend MIP-start API
  // requires the problem to be a MIP (CPXaddmipstarts needs integer columns).
  restore_integrality();

  if (!start) {
    spdlog::warn("MIP-start[{}]: produced no start", source);
    return report;
  }

  // Stage 5 — inject with `effort` ("solver repair").  Default `repair`: the
  // candidate is near-feasible but typically still nicks a few Pmin /
  // user-constraint / reserve rows, so CHECKFEAS would discard it; `repair`
  // lets the active backend mend those residuals (and on CPLEX this composes
  // with the SCIP stage as structural-then-numerical repair).  SystemLP then
  // runs the MIP solve (the "resolve").
  const auto effort = opts.inject.effort.value_or(MipStartEffort::repair);
  report.injected = li.set_mip_start(*start, effort);
  report.source = source;
  spdlog::info(
      "MIP-start[{}]: {} ({} integer cols, effort={}, relax_obj={:.6g})",
      source,
      report.injected ? "injected" : "backend declined",
      int_cols.size(),
      enum_name(effort),
      report.relax_obj.value_or(0.0));
  return report;
}

}  // namespace gtopt
