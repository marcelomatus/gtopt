/**
 * @file      scip_solver_backend.cpp
 * @brief     SCIP solver backend implementation (buffer-and-replay)
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * NOTE on logging: spdlog is now a shared library (libspdlog.so) that this
 * dlopen-loaded MODULE plugin links against (via gtopt::gtopt), sharing the
 * core's one global logger/registry — so hard SCIP errors route through
 * `spdlog::error` like every other plugin (no more std::fprintf-to-stderr
 * fallback).  SCIP's own per-iteration diagnostics remain on SCIP's native
 * message handler (`display/verblevel = 0`, or its logfile under
 * log_mode=detailed); a failed SCIP_RETCODE only marks the solve unsolved so
 * the caller sees a non-optimal status — it never aborts.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

#include "scip_solver_backend.hpp"

#include <scip/cons_sos2.h>
#include <scip/scip.h>
#include <scip/scipdefplugins.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Conventional "no bound" sentinel reported to gtopt (matches HiGHS / cuOpt /
/// OSI).  SCIP's native infinity (1e20) is used internally; bounds at/above
/// this sentinel are clamped to SCIPinfinity() before they reach SCIP.
constexpr double k_scip_plugin_inf = 1e30;

/// Clamp a bound to SCIP's native +/-infinity so SCIP never sees a literal
/// 1e30 (which it would treat as a finite, ill-scaled value).
[[nodiscard]] double clamp_inf(double v, double scip_inf) noexcept
{
  if (v >= scip_inf) {
    return scip_inf;
  }
  if (v <= -scip_inf) {
    return -scip_inf;
  }
  return v;
}

/// Apply the gtopt SolverOptions subset SCIP understands.  Pure parameter
/// mutation; guarded so an unknown/renamed parameter never aborts.
SCIP_RETCODE apply_options_to_scip(SCIP* scip, const SolverOptions& opts)
{
  SCIP_CALL(SCIPsetIntParam(scip, "display/verblevel", 0));
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    SCIP_CALL(SCIPsetRealParam(scip, "limits/time", *tl));
  }
  if (const auto gap = opts.mip_gap; gap && *gap >= 0.0) {
    SCIP_CALL(SCIPsetRealParam(scip, "limits/gap", *gap));
  }
  if (const auto agap = opts.mip_gap_abs; agap && *agap >= 0.0) {
    SCIP_CALL(SCIPsetRealParam(scip, "limits/absgap", *agap));
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0.0) {
    SCIP_CALL(SCIPsetRealParam(scip, "numerics/feastol", *feps));
  }
  return SCIP_OKAY;
}

/// Sanitise a gtopt label into a SCIP-LP-writer-safe token: keep
/// alphanumerics and `_`, map every other character to `_`.  Empty or
/// all-illegal input falls back to @p generic so every var/cons still has a
/// non-empty, unique-per-index name (the generic id embeds the index).
[[nodiscard]] std::string sanitize_name(const std::string& raw,
                                        const std::string& generic)
{
  if (raw.empty()) {
    return generic;
  }
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    out.push_back(
        (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') ? c
                                                                       : '_');
  }
  // A leading digit is illegal for an LP-format token; prefix the generic
  // stem (which starts with a letter) so the gtopt label stays a substring.
  if (std::isdigit(static_cast<unsigned char>(out.front())) != 0) {
    return generic + "_" + out;
  }
  return out;
}

/// Build the original SCIP problem (vars + linear constraints) from `model`.
/// Caller owns `vars`/`conss` and must release each (SCIPreleaseVar /
/// SCIPreleaseCons) on the success path; on an early SCIP_CALL return the
/// caller frees the whole SCIP instance, which releases them too.
///
/// When @p col_names / @p row_names are non-null and long enough, their
/// sanitised entries name the SCIP variables / linear constraints so a
/// `write_lp` dump carries gtopt labels; otherwise generic `x<j>` / `c<i>`
/// names are used (the solve path, where names are irrelevant).
SCIP_RETCODE scip_build_problem(SCIP* scip,
                                const ScipModel& model,
                                std::vector<SCIP_VAR*>& vars,
                                std::vector<SCIP_CONS*>& conss,
                                const std::vector<std::string>* col_names,
                                const std::vector<std::string>* row_names)
{
  const double inf = SCIPinfinity(scip);
  SCIP_CALL(SCIPsetObjsense(scip, SCIP_OBJSENSE_MINIMIZE));

  vars.assign(static_cast<std::size_t>(model.num_cols), nullptr);
  for (int j = 0; j < model.num_cols; ++j) {
    const auto u = static_cast<std::size_t>(j);
    const double lb = clamp_inf(model.col_lb[u], inf);
    const double ub = clamp_inf(model.col_ub[u], inf);
    SCIP_VARTYPE vtype = SCIP_VARTYPE_CONTINUOUS;
    if (model.col_type[u] == 'I') {
      // Treat a [0,1] integer as binary, otherwise general integer.
      vtype = (model.col_lb[u] >= 0.0 && model.col_ub[u] <= 1.0)
          ? SCIP_VARTYPE_BINARY
          : SCIP_VARTYPE_INTEGER;
    }
    // SCIP requires a non-NULL, unique variable name.  Prefer the pushed
    // gtopt label (sanitised) so write_lp carries real names; else "x<j>".
    const std::string generic = "x" + std::to_string(j);
    const std::string vname =
        (col_names != nullptr && u < col_names->size())
        ? sanitize_name((*col_names)[u], generic)
        : generic;
    SCIP_CALL(SCIPcreateVarBasic(
        scip, &vars[u], vname.c_str(), lb, ub, model.col_obj[u], vtype));
    SCIP_CALL(SCIPaddVar(scip, vars[u]));
  }

  conss.assign(static_cast<std::size_t>(model.num_rows), nullptr);
  for (int i = 0; i < model.num_rows; ++i) {
    const auto u = static_cast<std::size_t>(i);
    const double lhs = clamp_inf(model.row_lb[u], inf);
    const double rhs = clamp_inf(model.row_ub[u], inf);
    // SCIPcreateConsBasicLinear runs strlen(name) — NULL is NOT allowed here
    // (unlike SCIPcreateVarBasic, which treats NULL as auto-name).  Prefer the
    // pushed gtopt row label (sanitised); else a unique "c<i>".
    const std::string generic = "c" + std::to_string(i);
    const std::string cname =
        (row_names != nullptr && u < row_names->size())
        ? sanitize_name((*row_names)[u], generic)
        : generic;
    SCIP_CALL(SCIPcreateConsBasicLinear(
        scip, &conss[u], cname.c_str(), 0, nullptr, nullptr, lhs, rhs));
    for (const auto& [col, val] : model.row_entries[u]) {
      SCIP_CALL(SCIPaddCoefLinear(
          scip, conss[u], vars[static_cast<std::size_t>(col)], val));
    }
    SCIP_CALL(SCIPaddCons(scip, conss[u]));
  }

  // SOS2 sets (issue #504 L-secant chord).  Each set becomes one native
  // SCIP SOS2 constraint enforcing at-most-two-adjacent-non-zero over its
  // columns, in the listed (geometric breakpoint) order.  Natural-order
  // weights (NULL) suffice — gtopt does not expose custom SOS2 weights.
  // The SOS2 constraints are appended to `conss` so the caller releases
  // them alongside the linear rows; the dual-recovery loop iterates only
  // the first `model.num_rows` entries and never reaches them.
  for (const auto& set : model.sos2_sets) {
    if (set.size() < 2) {
      continue;  // SOS2 over < 2 columns is vacuous
    }
    std::vector<SCIP_VAR*> sos_vars;
    sos_vars.reserve(set.size());
    for (const int col : set) {
      sos_vars.push_back(vars[static_cast<std::size_t>(col)]);
    }
    const std::string sname = "sos2_" + std::to_string(conss.size());
    SCIP_CONS* scons = nullptr;
    SCIP_CALL(SCIPcreateConsBasicSOS2(scip,
                                      &scons,
                                      sname.c_str(),
                                      static_cast<int>(sos_vars.size()),
                                      sos_vars.data(),
                                      nullptr));
    SCIP_CALL(SCIPaddCons(scip, scons));
    conss.push_back(scons);
  }
  return SCIP_OKAY;
}

/// Install a buffered MIP start as a SCIP partial solution over the integer
/// columns, tuning the completesol heuristic under `effort == repair` so SCIP
/// completes/repairs it into a feasible incumbent.
SCIP_RETCODE scip_install_mip_start(SCIP* scip,
                                    const ScipModel& model,
                                    const std::vector<double>& start,
                                    MipStartEffort effort,
                                    const std::vector<SCIP_VAR*>& vars)
{
  if (start.size() != static_cast<std::size_t>(model.num_cols)) {
    return SCIP_OKAY;  // size mismatch — nothing to install (benign)
  }
  bool any_integer = false;
  for (const char t : model.col_type) {
    if (t == 'I') {
      any_integer = true;
      break;
    }
  }
  if (!any_integer) {
    return SCIP_OKAY;  // pure LP — no integer start to repair
  }

  if (effort == MipStartEffort::repair) {
    // Feasibility emphasis + let completesol fill an arbitrarily large unknown
    // fraction (we only fix the integer columns; continuous stay unknown).
    SCIP_CALL(SCIPsetEmphasis(scip, SCIP_PARAMEMPHASIS_FEASIBILITY, TRUE));
    SCIP_CALL(
        SCIPsetRealParam(scip, "heuristics/completesol/maxunknownrate", 1.0));
  }

  SCIP_SOL* sol = nullptr;
  SCIP_CALL(SCIPcreatePartialSol(scip, &sol, nullptr));
  for (int j = 0; j < model.num_cols; ++j) {
    const auto u = static_cast<std::size_t>(j);
    if (model.col_type[u] == 'I') {
      SCIP_CALL(SCIPsetSolVal(scip, sol, vars[u], std::round(start[u])));
    }
  }
  SCIP_Bool stored = FALSE;
  SCIP_CALL(SCIPaddSolFree(scip, &sol, &stored));
  return SCIP_OKAY;
}

/// Full build -> (optional MIP start) -> solve -> snapshot.  All SCIP_CALL
/// sequencing is confined here so a bad retcode bubbles out as a value, never
/// as an abort.  `scip` is created/freed by the caller.
SCIP_RETCODE scip_build_and_solve(SCIP* scip,
                                  const ScipModel& model,
                                  const SolverOptions& opts,
                                  const std::string& log_filename,
                                  const std::vector<double>& mip_start,
                                  MipStartEffort mip_effort,
                                  ScipSolutionCache& out,
                                  double& solving_time)
{
  SCIP_CALL(SCIPincludeDefaultPlugins(scip));
  SCIP_CALL(SCIPcreateProbBasic(scip, "gtopt_scip"));
  SCIP_CALL(apply_options_to_scip(scip, opts));

  // File logging (log_mode=detailed): apply_options_to_scip sets
  // display/verblevel=0 (quiet); when a log file is requested, raise the
  // verbosity back to NORMAL and redirect SCIP's message output to it so the
  // framework's "<stem>.log" is created and populated.
  if (!log_filename.empty()) {
    SCIP_CALL(SCIPsetIntParam(
        scip, "display/verblevel", static_cast<int>(SCIP_VERBLEVEL_NORMAL)));
    SCIPsetMessagehdlrLogfile(scip, log_filename.c_str());
  }

  std::vector<SCIP_VAR*> vars;
  std::vector<SCIP_CONS*> conss;
  SCIP_CALL(scip_build_problem(scip, model, vars, conss, nullptr, nullptr));

  if (!mip_start.empty()) {
    SCIP_CALL(scip_install_mip_start(scip, model, mip_start, mip_effort, vars));
  }

  // A pure-LP solve is where gtopt reads duals + reduced costs (the
  // dual-recovery re-solve: pin integers, relax, resolve).  Disable SCIP
  // presolve there so no column is fixed/aggregated away — otherwise
  // SCIPgetVarRedcost reports 0 for the pinned (lb==ub) commitment columns (the
  // committed-dual gap) and the reconstructed duals are not matrix-consistent.
  // MIP solves keep presolve (their duals are not read).
  bool any_integer = false;
  for (const char t : model.col_type) {
    if (t == 'I') {
      any_integer = true;
      break;
    }
  }
  if (!any_integer) {
    SCIP_CALL(SCIPsetIntParam(scip, "presolving/maxrounds", 0));
    // Also disable domain propagation: it would tighten a binding constraint
    // into a variable bound and move the shadow price onto that bound (reduced
    // cost), so the constraint's own LP dual would read back as 0.  Keeping
    // propagation off leaves the price on the row, where the dual-recovery
    // re-solve reads it.
    SCIP_CALL(SCIPsetIntParam(scip, "propagating/maxrounds", 0));
    SCIP_CALL(SCIPsetIntParam(scip, "propagating/maxroundsroot", 0));
  }

  SCIP_CALL(SCIPsolve(scip));
  solving_time = SCIPgetSolvingTime(scip);

  out.status = static_cast<int>(SCIPgetStatus(scip));
  out.primal.assign(static_cast<std::size_t>(model.num_cols), 0.0);
  out.reduced.assign(static_cast<std::size_t>(model.num_cols), 0.0);
  out.dual.assign(static_cast<std::size_t>(model.num_rows), 0.0);

  SCIP_SOL* best = SCIPgetBestSol(scip);
  out.solved = (best != nullptr);
  if (best != nullptr) {
    for (int j = 0; j < model.num_cols; ++j) {
      const auto u = static_cast<std::size_t>(j);
      out.primal[u] = SCIPgetSolVal(scip, best, vars[u]);
    }
    out.obj = SCIPgetPrimalbound(scip);
  }

  // Duals/reduced costs are only meaningful for a pure-LP optimum;
  // `any_integer` was determined before the solve (it also gated presolve).
  if (!any_integer && out.status == static_cast<int>(SCIP_STATUS_OPTIMAL)) {
    // Row duals via the high-level constraint API SCIPgetDualSolVal.  This is
    // the version-robust way to read a linear constraint's dual in the SOLVED
    // stage: unlike a raw 1:1 LPI row read (SCIPlpiGetSol), it stays correct
    // when SCIP has internally rewritten a now-single-variable constraint —
    // e.g. the commitment row `gen - 100*u <= 0` after `u` is pinned to 1
    // becomes `gen <= 100` — into a variable *bound*.  That rewrite DROPS the
    // row from the transformed LP (observed on SCIP 10: nlpi_rows < num_rows),
    // which broke the old LPI approach: the row-count mismatch fell through to
    // SCIPgetDualsolLinear, which returns 0 post-solve, zeroing every dual.
    // SCIPgetDualSolVal reports the price via its `boundconstraint` path in
    // exactly that case, so the recovered LMPs are matrix-consistent again.
    for (int i = 0; i < model.num_rows; ++i) {
      const auto u = static_cast<std::size_t>(i);
      double dual_val = 0.0;
      SCIP_Bool bound_constraint = FALSE;
      if (SCIPgetDualSolVal(scip, conss[u], &dual_val, &bound_constraint)
          == SCIP_OKAY)
      {
        out.dual[u] = dual_val;
      }
    }
    // Column reduced costs.  With SCIP presolve disabled for this pure-LP solve
    // (see the solve setup above), no column is fixed/aggregated away, so
    // SCIPgetVarRedcost returns the exact LP reduced cost for EVERY column —
    // including the pinned (lb==ub) commitment columns of gtopt's dual-recovery
    // LP, which presolve would otherwise read back as 0 (the committed-dual
    // gap).  Matches CLP.
    for (int j = 0; j < model.num_cols; ++j) {
      const auto u = static_cast<std::size_t>(j);
      const double rc = SCIPgetVarRedcost(scip, vars[u]);
      // Complementary-slackness cleanup: at an optimal primal-dual pair a
      // column strictly interior to its bounds is basic, so its reduced
      // cost is exactly 0.  SCIP's SoPlex can report a nonzero reduced
      // cost for such a column when its optimal basis differs from the
      // (equally optimal, degenerate) vertex whose primal we read — the
      // shadow price then lands on BOTH the binding row dual and this
      // interior column, double-counting the marginal.  Zeroing interior
      // columns restores complementarity and matches CPLEX / HiGHS / CLP,
      // which is what gtopt's SDDP dual accounting expects.  Columns at a
      // bound (including the pinned lb==ub commitment columns of the
      // dual-recovery re-solve) are strictly not interior, so their
      // reduced cost is preserved untouched.
      const double xj = out.primal[u];
      const double lb = model.col_lb[u];
      const double ub = model.col_ub[u];
      const double tol =
          1e-7 * (1.0 + std::max(std::abs(lb), std::abs(ub)));
      const bool interior = (xj > lb + tol) && (xj < ub - tol);
      out.reduced[u] = interior ? 0.0 : rc;
    }
  }

  // Release our handle on every var/cons (SCIP keeps its own reference).
  for (auto*& c : conss) {
    if (c != nullptr) {
      SCIP_CALL(SCIPreleaseCons(scip, &c));
    }
  }
  for (auto*& v : vars) {
    if (v != nullptr) {
      SCIP_CALL(SCIPreleaseVar(scip, &v));
    }
  }
  return SCIP_OKAY;
}

}  // namespace

ScipSolverBackend::ScipSolverBackend() = default;
ScipSolverBackend::~ScipSolverBackend() = default;

std::string_view ScipSolverBackend::solver_name() const noexcept
{
  return "scip";
}

std::string ScipSolverBackend::solver_version() const
{
  return std::to_string(SCIPmajorVersion()) + "."
      + std::to_string(SCIPminorVersion()) + "."
      + std::to_string(SCIPtechVersion());
}

double ScipSolverBackend::plugin_infinity() noexcept
{
  return k_scip_plugin_inf;
}

double ScipSolverBackend::infinity() const noexcept
{
  return plugin_infinity();
}

bool ScipSolverBackend::supports_mip() const noexcept
{
  return true;
}

void ScipSolverBackend::set_prob_name(const std::string& name)
{
  m_prob_name_ = name;
}

std::string ScipSolverBackend::get_prob_name() const
{
  return m_prob_name_;
}

void ScipSolverBackend::load_problem(int ncols,
                                     int nrows,
                                     const int* matbeg,
                                     const int* matind,
                                     const double* matval,
                                     const double* collb,
                                     const double* colub,
                                     const double* obj,
                                     const double* rowlb,
                                     const double* rowub)
{
  m_model_ = {};
  m_sol_ = {};
  m_mip_start_.clear();
  if (ncols == 0 && nrows == 0) {
    return;
  }

  m_model_.num_cols = ncols;
  m_model_.num_rows = nrows;
  const auto nc = static_cast<std::size_t>(ncols);
  const auto nr = static_cast<std::size_t>(nrows);

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_model_.col_lb.assign(collb, collb + nc);
  m_model_.col_ub.assign(colub, colub + nc);
  m_model_.col_obj.assign(obj, obj + nc);
  m_model_.col_type.assign(nc, 'C');
  m_model_.row_lb.assign(rowlb, rowlb + nr);
  m_model_.row_ub.assign(rowub, rowub + nr);
  m_model_.row_entries.assign(nr, {});

  // CSC matrix: column j owns entries [matbeg[j], matbeg[j+1]).
  for (int j = 0; j < ncols; ++j) {
    for (int k = matbeg[j]; k < matbeg[j + 1]; ++k) {
      const int row = matind[k];
      m_model_.row_entries[static_cast<std::size_t>(row)][j] = matval[k];
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

int ScipSolverBackend::get_num_cols() const
{
  return m_model_.num_cols;
}

int ScipSolverBackend::get_num_rows() const
{
  return m_model_.num_rows;
}

void ScipSolverBackend::add_col(double lb, double ub, double obj)
{
  m_model_.col_lb.push_back(lb);
  m_model_.col_ub.push_back(ub);
  m_model_.col_obj.push_back(obj);
  m_model_.col_type.push_back('C');
  ++m_model_.num_cols;
}

void ScipSolverBackend::add_cols(int num_cols,
                                 const int* colbeg,
                                 const int* colind,
                                 const double* colval,
                                 const double* collb,
                                 const double* colub,
                                 const double* colobj)
{
  if (num_cols == 0) {
    return;
  }
  const int base = m_model_.num_cols;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (int c = 0; c < num_cols; ++c) {
    const int gcol = base + c;
    m_model_.col_lb.push_back(collb[c]);
    m_model_.col_ub.push_back(colub[c]);
    m_model_.col_obj.push_back(colobj[c]);
    m_model_.col_type.push_back('C');
    for (int k = colbeg[c]; k < colbeg[c + 1]; ++k) {
      const int row = colind[k];
      m_model_.row_entries[static_cast<std::size_t>(row)][gcol] = colval[k];
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_model_.num_cols += num_cols;
}

void ScipSolverBackend::set_col_lower(int index, double value)
{
  m_model_.col_lb[static_cast<std::size_t>(index)] = value;
}

void ScipSolverBackend::set_col_upper(int index, double value)
{
  m_model_.col_ub[static_cast<std::size_t>(index)] = value;
}

void ScipSolverBackend::set_obj_coeff(int index, double value)
{
  m_model_.col_obj[static_cast<std::size_t>(index)] = value;
}

void ScipSolverBackend::add_row(int num_elements,
                                const int* columns,
                                const double* elements,
                                double rowlb,
                                double rowub)
{
  std::map<int, double> entries;
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (int k = 0; k < num_elements; ++k) {
    entries[columns[k]] = elements[k];
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_model_.row_lb.push_back(rowlb);
  m_model_.row_ub.push_back(rowub);
  m_model_.row_entries.push_back(std::move(entries));
  ++m_model_.num_rows;
}

void ScipSolverBackend::add_rows(int num_rows,
                                 const int* rowbeg,
                                 const int* rowind,
                                 const double* rowval,
                                 const double* rowlb,
                                 const double* rowub)
{
  if (num_rows == 0) {
    return;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (int r = 0; r < num_rows; ++r) {
    std::map<int, double> entries;
    for (int k = rowbeg[r]; k < rowbeg[r + 1]; ++k) {
      entries[rowind[k]] = rowval[k];
    }
    m_model_.row_lb.push_back(rowlb[r]);
    m_model_.row_ub.push_back(rowub[r]);
    m_model_.row_entries.push_back(std::move(entries));
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_model_.num_rows += num_rows;
}

void ScipSolverBackend::set_row_lower(int index, double value)
{
  m_model_.row_lb[static_cast<std::size_t>(index)] = value;
}

void ScipSolverBackend::set_row_upper(int index, double value)
{
  m_model_.row_ub[static_cast<std::size_t>(index)] = value;
}

void ScipSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  const auto u = static_cast<std::size_t>(index);
  m_model_.row_lb[u] = lb;
  m_model_.row_ub[u] = ub;
}

void ScipSolverBackend::delete_rows(int num, const int* indices)
{
  if (num <= 0) {
    return;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::vector<int> drop(indices, indices + num);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::ranges::sort(drop, std::greater<>());
  drop.erase(std::ranges::unique(drop).begin(), drop.end());
  for (const int idx : drop) {
    if (idx < 0 || idx >= m_model_.num_rows) {
      continue;
    }
    const auto u = static_cast<std::size_t>(idx);
    m_model_.row_lb.erase(m_model_.row_lb.begin()
                          + static_cast<std::ptrdiff_t>(u));
    m_model_.row_ub.erase(m_model_.row_ub.begin()
                          + static_cast<std::ptrdiff_t>(u));
    m_model_.row_entries.erase(m_model_.row_entries.begin()
                               + static_cast<std::ptrdiff_t>(u));
    --m_model_.num_rows;
  }
}

double ScipSolverBackend::get_coeff(int row, int col) const
{
  // Guard against an out-of-range row (e.g. a query on an empty LP with
  // zero rows): `operator[]` on an empty vector is UB.  Absent rows carry
  // no coefficients, so the matrix entry is 0.
  if (row < 0 || row >= m_model_.num_rows) {
    return 0.0;
  }
  const auto& entries = m_model_.row_entries[static_cast<std::size_t>(row)];
  const auto it = entries.find(col);
  return it != entries.end() ? it->second : 0.0;
}

void ScipSolverBackend::set_coeff(int row, int col, double value)
{
  m_model_.row_entries[static_cast<std::size_t>(row)][col] = value;
}

bool ScipSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

void ScipSolverBackend::add_sos2(std::span<const int> columns)
{
  // SOS2 over fewer than two columns is vacuous (mirrors the base
  // contract).  Otherwise buffer the ordered column list; it is lowered
  // to a native SCIP SOS2 constraint on the next solve.
  if (columns.size() < 2) {
    return;
  }
  m_model_.sos2_sets.emplace_back(columns.begin(), columns.end());
}

void ScipSolverBackend::set_continuous(int index)
{
  m_model_.col_type[static_cast<std::size_t>(index)] = 'C';
}

void ScipSolverBackend::set_integer(int index)
{
  m_model_.col_type[static_cast<std::size_t>(index)] = 'I';
}

bool ScipSolverBackend::is_continuous(int index) const
{
  return m_model_.col_type[static_cast<std::size_t>(index)] == 'C';
}

bool ScipSolverBackend::is_integer(int index) const
{
  return m_model_.col_type[static_cast<std::size_t>(index)] == 'I';
}

int ScipSolverBackend::relax_all_integers()
{
  int relaxed = 0;
  for (auto& t : m_model_.col_type) {
    if (t == 'I') {
      t = 'C';
      ++relaxed;
    }
  }
  return relaxed;
}

const double* ScipSolverBackend::col_lower() const
{
  return m_model_.col_lb.data();
}

const double* ScipSolverBackend::col_upper() const
{
  return m_model_.col_ub.data();
}

const double* ScipSolverBackend::obj_coefficients() const
{
  return m_model_.col_obj.data();
}

const double* ScipSolverBackend::row_lower() const
{
  return m_model_.row_lb.data();
}

const double* ScipSolverBackend::row_upper() const
{
  return m_model_.row_ub.data();
}

const double* ScipSolverBackend::stable_col_buffer(
    const std::vector<double>& snap, std::size_t n) const
{
  if (m_sol_.solved && snap.size() == n) {
    return snap.data();
  }
  // Unsolved / freshly-cloned / size-mismatched: hand back a zero-filled
  // buffer of the model's column count so a `get_numcols()`-sized view
  // over the pointer stays in-bounds (matches the always-valid-buffer
  // contract the OSI/HiGHS/CPLEX backends satisfy).
  if (m_zero_cols_.size() != n) {
    m_zero_cols_.assign(n, 0.0);
  }
  return m_zero_cols_.data();
}

const double* ScipSolverBackend::stable_row_buffer(
    const std::vector<double>& snap, std::size_t n) const
{
  if (m_sol_.solved && snap.size() == n) {
    return snap.data();
  }
  if (m_zero_rows_.size() != n) {
    m_zero_rows_.assign(n, 0.0);
  }
  return m_zero_rows_.data();
}

const double* ScipSolverBackend::col_solution() const
{
  return stable_col_buffer(m_sol_.primal,
                           static_cast<std::size_t>(m_model_.num_cols));
}

const double* ScipSolverBackend::reduced_cost() const
{
  return stable_col_buffer(m_sol_.reduced,
                           static_cast<std::size_t>(m_model_.num_cols));
}

const double* ScipSolverBackend::row_price() const
{
  return stable_row_buffer(m_sol_.dual,
                           static_cast<std::size_t>(m_model_.num_rows));
}

double ScipSolverBackend::obj_value() const
{
  return m_sol_.obj;
}

void ScipSolverBackend::set_col_solution(const double* /*sol*/)
{
  // No warm start: SCIP rebuilds cold every solve (buffer-and-replay).
}

void ScipSolverBackend::set_row_price(const double* /*price*/)
{
  // No warm start (see set_col_solution).
}

bool ScipSolverBackend::set_mip_start(std::span<const double> col_values,
                                      MipStartEffort effort)
{
  if (col_values.size() != static_cast<std::size_t>(m_model_.num_cols)) {
    return false;
  }
  m_mip_start_.assign(col_values.begin(), col_values.end());
  m_mip_start_effort_ = effort;
  return true;
}

void ScipSolverBackend::solve_()
{
  SCIP* scip = nullptr;
  if (SCIPcreate(&scip) != SCIP_OKAY || scip == nullptr) {
    spdlog::error("gtopt[scip]: SCIPcreate failed");
    m_sol_.solved = false;
    m_sol_.status = static_cast<int>(SCIP_STATUS_UNKNOWN);
    return;
  }

  double solving_time = 0.0;
  const SCIP_RETCODE rc = scip_build_and_solve(scip,
                                               m_model_,
                                               m_options_,
                                               m_log_filename_,
                                               m_mip_start_,
                                               m_mip_start_effort_,
                                               m_sol_,
                                               solving_time);
  if (rc != SCIP_OKAY) {
    spdlog::error("gtopt[scip]: solve failed (SCIP_RETCODE {})",
                  static_cast<int>(rc));
    m_sol_.solved = false;
    m_sol_.status = static_cast<int>(SCIP_STATUS_UNKNOWN);
  }
  m_last_effort_.seconds = solving_time;
  m_last_effort_.ticks = solving_time;

  SCIPfree(&scip);
}

void ScipSolverBackend::initial_solve()
{
  solve_();
}

void ScipSolverBackend::resolve()
{
  solve_();
}

SolveEffort ScipSolverBackend::last_solve_effort() const
{
  return m_last_effort_;
}

void ScipSolverBackend::engage_robust_solve()
{
  if (!m_saved_robust_options_.has_value()) {
    m_saved_robust_options_ = m_options_;
  }
  const double cur = m_options_.feasible_eps.value_or(1e-6);
  m_options_.feasible_eps = std::min(cur * 10.0, 1e-2);
}

void ScipSolverBackend::disengage_robust_solve() noexcept
{
  if (m_saved_robust_options_.has_value()) {
    m_options_ = *m_saved_robust_options_;
    m_saved_robust_options_.reset();
  }
}

bool ScipSolverBackend::is_proven_optimal() const
{
  return m_sol_.solved
      && m_sol_.status == static_cast<int>(SCIP_STATUS_OPTIMAL);
}

bool ScipSolverBackend::is_abandoned() const
{
  return m_sol_.status == static_cast<int>(SCIP_STATUS_UNKNOWN);
}

bool ScipSolverBackend::is_proven_primal_infeasible() const
{
  return m_sol_.status == static_cast<int>(SCIP_STATUS_INFEASIBLE);
}

bool ScipSolverBackend::is_proven_dual_infeasible() const
{
  return m_sol_.status == static_cast<int>(SCIP_STATUS_UNBOUNDED);
}

void ScipSolverBackend::apply_options(const SolverOptions& opts)
{
  m_options_ = opts;
  m_log_level_ = opts.log_level;
}

SolverOptions ScipSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::default_algo,
      .presolve = true,
      // Match the other backends' fallback budget.  SCIP rebuilds cold and
      // ignores the LPAlgo hint, so a fallback re-solve of an infeasible LP
      // just re-proves infeasibility, but keeping the budget > 0 gives the
      // LinearInterface its documented "(after algorithm fallback cycle)"
      // diagnostics and fallback-solve accounting parity with CLP/HiGHS.
      .max_fallbacks = 2,
  };
}

LPAlgo ScipSolverBackend::get_algorithm() const
{
  return m_options_.algorithm;
}

int ScipSolverBackend::get_threads() const
{
  return m_options_.threads;
}

bool ScipSolverBackend::get_presolve() const
{
  return m_options_.presolve;
}

int ScipSolverBackend::get_log_level() const
{
  return m_log_level_;
}

std::optional<double> ScipSolverBackend::get_kappa() const
{
  return std::nullopt;
}

void ScipSolverBackend::open_log(FILE* /*file*/, int level)
{
  m_log_level_ = level;
}

void ScipSolverBackend::close_log()
{
  m_log_level_ = 0;
}

void ScipSolverBackend::set_log_filename(const std::string& filename, int level)
{
  // Mirror HiGHS/MindOpt: append ".log" to the stem; the message handler is
  // installed per solve in scip_build_and_solve.
  m_log_filename_ =
      (level > 0 && !filename.empty()) ? filename + ".log" : std::string {};
  m_log_level_ = level;
}

void ScipSolverBackend::clear_log_filename()
{
  m_log_filename_.clear();
}

void ScipSolverBackend::push_names(const std::vector<std::string>& col_names,
                                   const std::vector<std::string>& row_names)
{
  m_col_names_ = col_names;
  m_row_names_ = row_names;
}

void ScipSolverBackend::write_lp(const char* filename)
{
  SCIP* scip = nullptr;
  if (SCIPcreate(&scip) != SCIP_OKAY || scip == nullptr) {
    return;
  }
  std::vector<SCIP_VAR*> vars;
  std::vector<SCIP_CONS*> conss;
  const std::string path = std::string(filename) + ".lp";
  if (SCIPincludeDefaultPlugins(scip) == SCIP_OKAY
      && SCIPcreateProbBasic(scip, m_prob_name_.c_str()) == SCIP_OKAY
      && scip_build_problem(scip, m_model_, vars, conss, &m_col_names_,
                            &m_row_names_)
          == SCIP_OKAY)
  {
    SCIPwriteOrigProblem(scip, path.c_str(), "lp", FALSE);
    for (auto*& c : conss) {
      if (c != nullptr) {
        SCIPreleaseCons(scip, &c);
      }
    }
    for (auto*& v : vars) {
      if (v != nullptr) {
        SCIPreleaseVar(scip, &v);
      }
    }
  }
  SCIPfree(&scip);
}

std::unique_ptr<SolverBackend> ScipSolverBackend::clone() const
{
  auto cloned = std::make_unique<ScipSolverBackend>();
  cloned->m_model_ = m_model_.clone();
  cloned->m_sol_ = m_sol_;
  cloned->m_options_ = m_options_;
  cloned->m_mip_start_ = m_mip_start_;
  cloned->m_mip_start_effort_ = m_mip_start_effort_;
  cloned->m_prob_name_ = m_prob_name_;
  cloned->m_col_names_ = m_col_names_;
  cloned->m_row_names_ = m_row_names_;
  cloned->m_log_level_ = m_log_level_;
  cloned->m_log_filename_ = m_log_filename_;
  return cloned;
}

}  // namespace gtopt
