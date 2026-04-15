/**
 * @file      cplex_solver_backend.cpp
 * @brief     CPLEX C Callable Library solver backend implementation
 * @date      Tue Mar 25 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cplex_solver_backend.hpp"

#include <gtopt/solver_options.hpp>
#include <ilcplex/cplex.h>

namespace gtopt
{

namespace
{

/// Convert ranged-row bounds (lb, ub) to CPLEX sense + rhs + range.
///
/// CPLEX stores rows as   sense  rhs  [rangeval]
///   'L' : row <= rhs
///   'G' : row >= rhs
///   'E' : row == rhs
///   'R' : rhs <= row <= rhs + |rangeval|
///
/// @param lb       Row lower bound (use -cpx_inf for no lower bound)
/// @param ub       Row upper bound (use +cpx_inf for no upper bound)
/// @param cpx_inf  CPLEX infinity constant (CPX_INFBOUND)
/// @param sense    [out] CPLEX constraint sense character
/// @param rhs      [out] CPLEX right-hand-side value
/// @param range    [out] CPLEX range value (only used for 'R' sense)
void bounds_to_cplex(double lb,
                     double ub,
                     double cpx_inf,
                     char& sense,
                     double& rhs,
                     double& range)
{
  const bool lb_inf = (lb <= -cpx_inf);
  const bool ub_inf = (ub >= cpx_inf);

  if (lb_inf && ub_inf) {
    // Free row: -inf <= row <= +inf  →  any sense works; use 'G' with -inf
    sense = 'G';
    rhs = -cpx_inf;
    range = 0.0;
  } else if (lb_inf) {
    sense = 'L';
    rhs = ub;
    range = 0.0;
  } else if (ub_inf) {
    sense = 'G';
    rhs = lb;
    range = 0.0;
  } else if (std::abs(ub - lb) < 1e-12) {
    sense = 'E';
    rhs = lb;
    range = 0.0;
  } else {
    // Ranged row: lb <= row <= ub
    sense = 'R';
    rhs = lb;
    range = ub - lb;
  }
}

/// Recover row lower bound from CPLEX sense/rhs/range.
double cplex_row_lb(char sense, double rhs, double range, double cpx_inf)
{
  switch (sense) {
    case 'L':
      return -cpx_inf;
    case 'G':
    case 'E':
      return rhs;
    case 'R':
      return (range > 0) ? rhs : rhs + range;
    default:
      return -cpx_inf;
  }
}

/// Recover row upper bound from CPLEX sense/rhs/range.
double cplex_row_ub(char sense, double rhs, double range, double cpx_inf)
{
  switch (sense) {
    case 'L':
    case 'E':
      return rhs;
    case 'G':
      return cpx_inf;
    case 'R':
      return rhs + std::abs(range);
    default:
      return cpx_inf;
  }
}

}  // namespace

CplexSolverBackend::CplexSolverBackend()
{
  int status = 0;
  m_env_ = CPXopenCPLEX(&status);
  if (m_env_ == nullptr) {
    throw std::runtime_error(
        std::format("CPLEX: CPXopenCPLEX failed with status {}", status));
  }
  // Screen output off by default
  CPXsetintparam(m_env_, CPX_PARAM_SCRIND, CPX_OFF);

  m_lp_ = CPXcreateprob(m_env_, &status, "gtopt");
  if (m_lp_ == nullptr) {
    CPXcloseCPLEX(&m_env_);
    throw std::runtime_error(
        std::format("CPLEX: CPXcreateprob failed with status {}", status));
  }
}

CplexSolverBackend::~CplexSolverBackend()
{
  if (m_lp_ != nullptr) {
    CPXfreeprob(m_env_, &m_lp_);
  }
  if (m_env_ != nullptr) {
    CPXcloseCPLEX(&m_env_);
  }
}

std::string_view CplexSolverBackend::solver_name() const noexcept
{
  return "cplex";
}

std::string CplexSolverBackend::solver_version() const
{
  // CPX_VERSION format: VVRRMMFF (e.g. 22010100 → 22.1.1)
  constexpr int ver = CPX_VERSION;
  return std::format(
      "{}.{}.{}", ver / 1000000, (ver / 10000) % 100, (ver / 100) % 100);
}

double CplexSolverBackend::infinity() const noexcept
{
  return CPX_INFBOUND;
}

bool CplexSolverBackend::supports_mip() const noexcept
{
  return true;
}

void CplexSolverBackend::set_prob_name(const std::string& name)
{
  CPXchgprobname(m_env_, m_lp_, name.c_str());
}

std::string CplexSolverBackend::get_prob_name() const
{
  std::array<char, 256> buf {};
  int surplus = 0;
  if (CPXgetprobname(
          m_env_, m_lp_, buf.data(), static_cast<int>(buf.size()), &surplus)
      == 0)
  {
    return {buf.data()};
  }
  return {};
}

void CplexSolverBackend::load_problem(int ncols,
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
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  m_solve_status_ = 0;

  // Delete old problem data
  int status = 0;
  CPXfreeprob(m_env_, &m_lp_);
  m_lp_ = CPXcreateprob(m_env_, &status, "gtopt");
  if (m_lp_ == nullptr) {
    throw std::runtime_error(
        std::format("CPLEX: CPXcreateprob failed with status {}", status));
  }

  if (ncols == 0 && nrows == 0) {
    return;
  }

  // Convert row bounds to CPLEX sense/rhs/range
  std::vector<char> sense(static_cast<size_t>(nrows));
  std::vector<double> rhs(static_cast<size_t>(nrows));
  std::vector<double> range(static_cast<size_t>(nrows));
  const auto cpx_inf = CPX_INFBOUND;

  for (int i = 0; i < nrows; ++i) {
    const auto idx = static_cast<size_t>(i);
    bounds_to_cplex(
        rowlb[i],  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        rowub[i],  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        cpx_inf,
        sense[idx],
        rhs[idx],
        range[idx]);
  }

  // CPLEX requires sorted row indices within each column (CSC format).
  // The LinearProblem::flatten() two-pass algorithm produces unsorted
  // indices (rows are added to columns in row-enumeration order, not
  // sorted order). Create sorted copies for CPLEX.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // matbeg/matind/matval are C-API raw pointers from caller
  const auto nnz =
      (ncols > 0) ? static_cast<size_t>(matbeg[ncols]) : size_t {0};
  std::vector<int> sorted_matind(matind, matind + nnz);
  std::vector<double> sorted_matval(matval, matval + nnz);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // Sort each column's row indices and reorder values accordingly
  for (int col = 0; col < ncols; ++col) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto begin = static_cast<size_t>(matbeg[col]);
    const auto end =
        (col + 1 < ncols) ? static_cast<size_t>(matbeg[col + 1]) : nnz;
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    if (end <= begin + 1) {
      continue;  // Column has 0 or 1 entries — already sorted
    }

    // Build index permutation that sorts row indices in this column
    std::vector<size_t> perm(end - begin);
    std::ranges::iota(perm, begin);
    std::ranges::sort(perm,
                      [&sorted_matind](size_t a, size_t b)
                      { return sorted_matind[a] < sorted_matind[b]; });

    // Apply permutation to both matind and matval for this column
    std::vector<int> temp_ind(end - begin);
    std::vector<double> temp_val(end - begin);
    for (size_t i = 0; i < perm.size(); ++i) {
      temp_ind[i] = sorted_matind[perm[i]];
      temp_val[i] = sorted_matval[perm[i]];
    }
    std::ranges::copy(
        temp_ind, sorted_matind.begin() + static_cast<std::ptrdiff_t>(begin));
    std::ranges::copy(
        temp_val, sorted_matval.begin() + static_cast<std::ptrdiff_t>(begin));
  }

  // CPLEX requires all pointers to be non-null, even for zero-element
  // matrices.  Provide safe defaults when the caller passes nullptr.
  const auto ncols_sz = static_cast<size_t>(ncols);

  std::vector<int> buf_matbeg(ncols_sz + 1, 0);
  std::vector<int> buf_matind(1, 0);
  std::vector<double> buf_matval(1, 0.0);
  std::vector<double> buf_obj(ncols_sz, 0.0);
  std::vector<double> buf_collb(ncols_sz, 0.0);
  std::vector<double> buf_colub(ncols_sz, 0.0);

  const int* safe_matbeg = matbeg != nullptr ? matbeg : buf_matbeg.data();
  const int* safe_matind =
      sorted_matind.empty() ? buf_matind.data() : sorted_matind.data();
  const double* safe_matval =
      sorted_matval.empty() ? buf_matval.data() : sorted_matval.data();
  const double* safe_obj = obj != nullptr ? obj : buf_obj.data();
  const double* safe_collb = collb != nullptr ? collb : buf_collb.data();
  const double* safe_colub = colub != nullptr ? colub : buf_colub.data();

  // Compute matcnt from matbeg differences (CPLEX 22.1 requires it).
  std::vector<int> matcnt(ncols_sz, 0);
  for (int c = 0; c < ncols; ++c) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    matcnt[static_cast<size_t>(c)] = safe_matbeg[c + 1] - safe_matbeg[c];
  }

  // CPLEX expects column-sparse format via CPXcopylp
  status = CPXcopylp(m_env_,
                     m_lp_,
                     ncols,
                     nrows,
                     CPX_MIN,  // minimization
                     safe_obj,
                     rhs.data(),
                     sense.data(),
                     safe_matbeg,
                     matcnt.data(),
                     safe_matind,
                     safe_matval,
                     safe_collb,
                     safe_colub,
                     range.data());

  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXcopylp failed with status {}", status));
  }
}

int CplexSolverBackend::get_num_cols() const
{
  return CPXgetnumcols(m_env_, m_lp_);
}

int CplexSolverBackend::get_num_rows() const
{
  return CPXgetnumrows(m_env_, m_lp_);
}

void CplexSolverBackend::add_col(double lb, double ub, double obj)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  const int matbeg = 0;
  CPXaddcols(
      m_env_, m_lp_, 1, 0, &obj, &matbeg, nullptr, nullptr, &lb, &ub, nullptr);
}

void CplexSolverBackend::set_col_lower(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  const char bound_type = 'L';
  CPXchgbds(m_env_, m_lp_, 1, &index, &bound_type, &value);
}

void CplexSolverBackend::set_col_upper(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  const char bound_type = 'U';
  CPXchgbds(m_env_, m_lp_, 1, &index, &bound_type, &value);
}

void CplexSolverBackend::set_obj_coeff(int index, double value)
{
  m_prob_cached_ = false;
  CPXchgobj(m_env_, m_lp_, 1, &index, &value);
}

void CplexSolverBackend::add_row(int num_elements,
                                 const int* columns,
                                 const double* elements,
                                 double rowlb,
                                 double rowub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  char sense {};
  double rhs {};
  double range {};
  bounds_to_cplex(rowlb, rowub, CPX_INFBOUND, sense, rhs, range);

  const int rmatbeg = 0;
  int status = CPXaddrows(m_env_,
                          m_lp_,
                          0,
                          1,
                          num_elements,
                          &rhs,
                          &sense,
                          &rmatbeg,
                          columns,
                          elements,
                          nullptr,
                          nullptr);
  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXaddrows failed with status {}", status));
  }

  // Set range value for ranged rows
  if (sense == 'R' && std::abs(range) > 1e-20) {
    const int row_idx = CPXgetnumrows(m_env_, m_lp_) - 1;
    CPXchgrngval(m_env_, m_lp_, 1, &row_idx, &range);
  }
}

void CplexSolverBackend::add_rows(int num_rows,
                                  const int* rowbeg,
                                  const int* rowind,
                                  const double* rowval,
                                  const double* rowlb,
                                  const double* rowub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  // Convert lb/ub bounds to CPLEX sense/rhs/range vectors
  std::vector<char> senses(static_cast<size_t>(num_rows));
  std::vector<double> rhs_vec(static_cast<size_t>(num_rows));
  std::vector<double> range_vec(static_cast<size_t>(num_rows));

  for (int r = 0; r < num_rows; ++r) {
    bounds_to_cplex(
        rowlb[r], rowub[r], CPX_INFBOUND, senses[r], rhs_vec[r], range_vec[r]);
  }

  int status = CPXaddrows(m_env_,
                          m_lp_,
                          0,
                          num_rows,
                          rowbeg[num_rows],
                          rhs_vec.data(),
                          senses.data(),
                          rowbeg,
                          rowind,
                          rowval,
                          nullptr,
                          nullptr);
  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXaddrows failed with status {}", status));
  }

  // Set range values for ranged rows
  const int first_new_row = CPXgetnumrows(m_env_, m_lp_) - num_rows;
  for (int r = 0; r < num_rows; ++r) {
    if (senses[r] == 'R' && std::abs(range_vec[r]) > 1e-20) {
      const int row_idx = first_new_row + r;
      CPXchgrngval(m_env_, m_lp_, 1, &row_idx, &range_vec[r]);
    }
  }
}

void CplexSolverBackend::set_row_lower(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  // Get current row upper bound to recompute sense/rhs/range
  char old_sense {};
  double old_rhs {};
  double old_range {};
  CPXgetrowinfeas(
      m_env_, m_lp_, nullptr, nullptr, 0, 0);  // ensure internal state
  CPXgetsense(m_env_, m_lp_, &old_sense, index, index);
  CPXgetrhs(m_env_, m_lp_, &old_rhs, index, index);
  CPXgetrngval(m_env_, m_lp_, &old_range, index, index);

  const double old_ub =
      cplex_row_ub(old_sense, old_rhs, old_range, CPX_INFBOUND);

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(value, old_ub, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_, m_lp_, 1, &index, &new_sense);
  CPXchgrhs(m_env_, m_lp_, 1, &index, &new_rhs);
  CPXchgrngval(m_env_, m_lp_, 1, &index, &new_range);
}

void CplexSolverBackend::set_row_upper(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  char old_sense {};
  double old_rhs {};
  double old_range {};
  CPXgetsense(m_env_, m_lp_, &old_sense, index, index);
  CPXgetrhs(m_env_, m_lp_, &old_rhs, index, index);
  CPXgetrngval(m_env_, m_lp_, &old_range, index, index);

  const double old_lb =
      cplex_row_lb(old_sense, old_rhs, old_range, CPX_INFBOUND);

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(old_lb, value, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_, m_lp_, 1, &index, &new_sense);
  CPXchgrhs(m_env_, m_lp_, 1, &index, &new_rhs);
  CPXchgrngval(m_env_, m_lp_, 1, &index, &new_range);
}

void CplexSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(lb, ub, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_, m_lp_, 1, &index, &new_sense);
  CPXchgrhs(m_env_, m_lp_, 1, &index, &new_rhs);
  CPXchgrngval(m_env_, m_lp_, 1, &index, &new_range);
}

void CplexSolverBackend::delete_rows(int num, const int* indices)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  // CPXdelrows expects a sorted range [begin, end].
  // We need to delete individual indices — use CPXdelsetrows with a delstat
  // array.
  const int nrows = CPXgetnumrows(m_env_, m_lp_);
  std::vector<int> delstat(static_cast<size_t>(nrows), 0);
  for (int i = 0; i < num; ++i) {
    const auto idx = static_cast<size_t>(
        indices[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (idx < delstat.size()) {
      delstat[idx] = 1;
    }
  }
  CPXdelsetrows(m_env_, m_lp_, delstat.data());
}

double CplexSolverBackend::get_coeff(int row, int col) const
{
  double value = 0.0;
  CPXgetcoef(m_env_, m_lp_, row, col, &value);
  return value;
}

void CplexSolverBackend::set_coeff(int row, int col, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  CPXchgcoef(m_env_, m_lp_, row, col, value);
}

bool CplexSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

void CplexSolverBackend::set_continuous(int index)
{
  m_sol_cached_ = false;
  // If problem is MIP, change column type to continuous
  const int cplex_type = CPXgetprobtype(m_env_, m_lp_);
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    const char ctype = CPX_CONTINUOUS;
    CPXchgctype(m_env_, m_lp_, 1, &index, &ctype);
  }
}

void CplexSolverBackend::set_integer(int index)
{
  m_sol_cached_ = false;
  // Promote to MIP if needed
  const int cplex_type = CPXgetprobtype(m_env_, m_lp_);
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    CPXchgprobtype(m_env_, m_lp_, CPXPROB_MILP);
  }
  const char ctype = CPX_INTEGER;
  CPXchgctype(m_env_, m_lp_, 1, &index, &ctype);
}

bool CplexSolverBackend::is_continuous(int index) const
{
  const int cplex_type = CPXgetprobtype(m_env_, m_lp_);
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    return true;  // All continuous in LP
  }
  char ctype {};
  CPXgetctype(m_env_, m_lp_, &ctype, index, index);
  return ctype == CPX_CONTINUOUS;
}

bool CplexSolverBackend::is_integer(int index) const
{
  return !is_continuous(index);
}

void CplexSolverBackend::cache_problem_data() const
{
  if (m_prob_cached_) {
    return;
  }

  const int ncols = CPXgetnumcols(m_env_, m_lp_);
  const int nrows = CPXgetnumrows(m_env_, m_lp_);

  m_collb_.resize(static_cast<size_t>(ncols));
  m_colub_.resize(static_cast<size_t>(ncols));
  m_obj_.resize(static_cast<size_t>(ncols));

  if (ncols > 0) {
    CPXgetlb(m_env_, m_lp_, m_collb_.data(), 0, ncols - 1);
    CPXgetub(m_env_, m_lp_, m_colub_.data(), 0, ncols - 1);
    CPXgetobj(m_env_, m_lp_, m_obj_.data(), 0, ncols - 1);
  }

  m_rowlb_.resize(static_cast<size_t>(nrows));
  m_rowub_.resize(static_cast<size_t>(nrows));

  if (nrows > 0) {
    std::vector<char> sense(static_cast<size_t>(nrows));
    std::vector<double> rhs(static_cast<size_t>(nrows));
    std::vector<double> range(static_cast<size_t>(nrows));
    CPXgetsense(m_env_, m_lp_, sense.data(), 0, nrows - 1);
    CPXgetrhs(m_env_, m_lp_, rhs.data(), 0, nrows - 1);
    CPXgetrngval(m_env_, m_lp_, range.data(), 0, nrows - 1);

    for (int i = 0; i < nrows; ++i) {
      const auto idx = static_cast<size_t>(i);
      m_rowlb_[idx] =
          cplex_row_lb(sense[idx], rhs[idx], range[idx], CPX_INFBOUND);
      m_rowub_[idx] =
          cplex_row_ub(sense[idx], rhs[idx], range[idx], CPX_INFBOUND);
    }
  }

  m_prob_cached_ = true;
}

void CplexSolverBackend::cache_solution() const
{
  if (m_sol_cached_) {
    return;
  }

  const int ncols = CPXgetnumcols(m_env_, m_lp_);
  const int nrows = CPXgetnumrows(m_env_, m_lp_);

  m_col_solution_.resize(static_cast<size_t>(ncols));
  m_reduced_cost_.resize(static_cast<size_t>(ncols));
  m_row_price_.resize(static_cast<size_t>(nrows));

  if (ncols > 0) {
    CPXgetx(m_env_, m_lp_, m_col_solution_.data(), 0, ncols - 1);
    CPXgetdj(m_env_, m_lp_, m_reduced_cost_.data(), 0, ncols - 1);
  }

  if (nrows > 0) {
    CPXgetpi(m_env_, m_lp_, m_row_price_.data(), 0, nrows - 1);
  }

  m_sol_cached_ = true;
}

const double* CplexSolverBackend::col_lower() const
{
  cache_problem_data();
  return m_collb_.data();
}

const double* CplexSolverBackend::col_upper() const
{
  cache_problem_data();
  return m_colub_.data();
}

const double* CplexSolverBackend::obj_coefficients() const
{
  cache_problem_data();
  return m_obj_.data();
}

const double* CplexSolverBackend::row_lower() const
{
  cache_problem_data();
  return m_rowlb_.data();
}

const double* CplexSolverBackend::row_upper() const
{
  cache_problem_data();
  return m_rowub_.data();
}

const double* CplexSolverBackend::col_solution() const
{
  cache_solution();
  return m_col_solution_.data();
}

const double* CplexSolverBackend::reduced_cost() const
{
  cache_solution();
  return m_reduced_cost_.data();
}

const double* CplexSolverBackend::row_price() const
{
  cache_solution();
  return m_row_price_.data();
}

double CplexSolverBackend::obj_value() const
{
  double val = 0.0;
  CPXgetobjval(m_env_, m_lp_, &val);
  return val;
}

void CplexSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr) {
    return;
  }
  const auto ncols = static_cast<size_t>(CPXgetnumcols(m_env_, m_lp_));
  // Cache the provided solution so that col_solution() returns it
  // immediately, without requiring a re-solve.
  m_col_solution_.assign(
      sol,
      sol + ncols);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_sol_cached_ = true;
  // Also provide it to CPLEX as a warm-start hint.
  CPXcopystart(m_env_,
               m_lp_,
               nullptr,  // cstat (basis statuses for columns)
               nullptr,  // rstat (basis statuses for rows)
               sol,  // primal values
               nullptr,  // dual values
               nullptr,  // slack values
               nullptr);  // dj values
}

void CplexSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr) {
    return;
  }
  m_sol_cached_ = false;
  CPXcopystart(m_env_,
               m_lp_,
               nullptr,  // cstat
               nullptr,  // rstat
               nullptr,  // primal
               price,  // dual
               nullptr,  // slack
               nullptr);  // dj
}

void CplexSolverBackend::initial_solve()
{
  m_sol_cached_ = false;

  const int cplex_type = CPXgetprobtype(m_env_, m_lp_);
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_, m_lp_);
  } else {
    m_solve_status_ = CPXlpopt(m_env_, m_lp_);
  }
}

void CplexSolverBackend::resolve()
{
  m_sol_cached_ = false;

  const int cplex_type = CPXgetprobtype(m_env_, m_lp_);
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_, m_lp_);
  } else {
    // CPXlpopt respects CPX_PARAM_LPMETHOD set by apply_options().
    // When reuse_basis is true, apply_options() sets LPMETHOD to dual simplex
    // and enables advanced start — so warm-start still works correctly.
    // When barrier is requested, CPXlpopt uses barrier as configured.
    m_solve_status_ = CPXlpopt(m_env_, m_lp_);
  }
}

bool CplexSolverBackend::is_proven_optimal() const
{
  const int stat = CPXgetstat(m_env_, m_lp_);
  return stat == CPX_STAT_OPTIMAL || stat == CPXMIP_OPTIMAL
      || stat == CPXMIP_OPTIMAL_TOL;
}

bool CplexSolverBackend::is_abandoned() const
{
  const int stat = CPXgetstat(m_env_, m_lp_);
  return stat == CPX_STAT_ABORT_USER || stat == CPX_STAT_ABORT_IT_LIM
      || stat == CPX_STAT_ABORT_TIME_LIM || stat == CPX_STAT_ABORT_OBJ_LIM
      || stat == CPX_STAT_NUM_BEST;
}

bool CplexSolverBackend::is_proven_primal_infeasible() const
{
  const int stat = CPXgetstat(m_env_, m_lp_);
  return stat == CPX_STAT_INFEASIBLE || stat == CPXMIP_INFEASIBLE;
}

bool CplexSolverBackend::is_proven_dual_infeasible() const
{
  const int stat = CPXgetstat(m_env_, m_lp_);
  return stat == CPX_STAT_UNBOUNDED || stat == CPXMIP_UNBOUNDED;
}

LPAlgo CplexSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int CplexSolverBackend::get_threads() const
{
  return m_threads_;
}

bool CplexSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int CplexSolverBackend::get_log_level() const
{
  return m_log_level_;
}

SolverOptions CplexSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

void CplexSolverBackend::apply_options(const SolverOptions& opts)
{
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;
  if (opts.threads > 0) {
    CPXsetintparam(m_env_, CPX_PARAM_THREADS, opts.threads);
  }

  CPXsetintparam(m_env_, CPX_PARAM_PREIND, opts.presolve ? CPX_ON : CPX_OFF);

  // Scaling: map SolverScaling → CPLEX CPX_PARAM_SCAIND.
  if (opts.scaling.has_value()) {
    int scaind = 0;  // equilibration (CPLEX default)
    switch (*opts.scaling) {
      case SolverScaling::none:
        scaind = -1;
        break;
      case SolverScaling::automatic:
        scaind = 0;
        break;
      case SolverScaling::aggressive:
        scaind = 1;
        break;
    }
    CPXsetintparam(m_env_, CPX_PARAM_SCAIND, scaind);
  }

  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    CPXsetdblparam(m_env_, CPX_PARAM_EPOPT, *oeps);
  }

  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    CPXsetdblparam(m_env_, CPX_PARAM_EPRHS, *feps);
  }

  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    CPXsetdblparam(m_env_, CPX_PARAM_TILIM, *tl);
  }

  if (opts.reuse_basis && opts.algorithm != LPAlgo::barrier) {
    m_algorithm_ = LPAlgo::dual;
    m_presolve_ = false;

    // Dual simplex with warm start
    CPXsetintparam(m_env_, CPX_PARAM_LPMETHOD, CPX_ALG_DUAL);
    CPXsetintparam(m_env_, CPX_PARAM_PREIND, CPX_OFF);
    CPXsetintparam(m_env_, CPX_PARAM_ADVIND, 1);  // use advanced start
    return;
  }

  switch (opts.algorithm) {
    case LPAlgo::default_algo:
      CPXsetintparam(m_env_, CPX_PARAM_LPMETHOD, CPX_ALG_AUTOMATIC);
      break;
    case LPAlgo::primal:
      CPXsetintparam(m_env_, CPX_PARAM_LPMETHOD, CPX_ALG_PRIMAL);
      break;
    case LPAlgo::dual:
      CPXsetintparam(m_env_, CPX_PARAM_LPMETHOD, CPX_ALG_DUAL);
      break;
    case LPAlgo::barrier:
      CPXsetintparam(m_env_, CPX_PARAM_LPMETHOD, CPX_ALG_BARRIER);
      if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
        CPXsetdblparam(m_env_, CPX_PARAM_BAREPCOMP, *beps);
      }
      // Crossover: primal (benchmark optimal) or none (forward training).
      CPXsetintparam(
          m_env_, CPX_PARAM_BARCROSSALG, opts.crossover ? 1 : CPX_ALG_NONE);
      break;
    case LPAlgo::last_algo:
      break;
  }

  CPXsetintparam(
      m_env_, CPX_PARAM_SCRIND, opts.log_level > 0 ? CPX_ON : CPX_OFF);
}

double CplexSolverBackend::get_kappa() const
{
  double kappa = 1.0;
  if (CPXgetdblquality(m_env_, m_lp_, &kappa, CPX_KAPPA) != 0) {
    kappa = 1.0;
  }
  return kappa;
}

void CplexSolverBackend::open_log(FILE* /*file*/, int level)
{
  CPXsetintparam(m_env_, CPX_PARAM_SCRIND, level > 0 ? CPX_ON : CPX_OFF);
}

void CplexSolverBackend::close_log()
{
  CPXsetintparam(m_env_, CPX_PARAM_SCRIND, CPX_OFF);
}

void CplexSolverBackend::set_log_filename(const std::string& filename,
                                          int level)
{
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    CPXsetlogfilename(m_env_, log_path.c_str(), "a");
    CPXsetintparam(m_env_, CPX_PARAM_SCRIND, CPX_ON);
  }
}

void CplexSolverBackend::clear_log_filename()
{
  CPXsetlogfilename(m_env_, nullptr, nullptr);
  CPXsetintparam(m_env_, CPX_PARAM_SCRIND, CPX_OFF);
}

void CplexSolverBackend::push_names(const std::vector<std::string>& col_names,
                                    const std::vector<std::string>& row_names)
{
  // Set column names — copy to mutable buffer because older CPLEX APIs
  // declare the name parameter as char** (non-const).
  for (int i = 0; std::cmp_less(i, col_names.size()); ++i) {
    if (!col_names[static_cast<size_t>(i)].empty()) {
      std::string name_buf = col_names[static_cast<size_t>(i)];
      auto* name_ptr = name_buf.data();
      CPXchgcolname(m_env_, m_lp_, 1, &i, &name_ptr);
    }
  }

  // Set row names
  for (int i = 0; std::cmp_less(i, row_names.size()); ++i) {
    if (!row_names[static_cast<size_t>(i)].empty()) {
      std::string name_buf = row_names[static_cast<size_t>(i)];
      auto* name_ptr = name_buf.data();
      CPXchgrowname(m_env_, m_lp_, 1, &i, &name_ptr);
    }
  }
}

void CplexSolverBackend::write_lp(const char* filename)
{
  const auto file = std::format("{}.lp", filename);
  CPXwriteprob(m_env_, m_lp_, file.c_str(), "LP");
}

std::unique_ptr<SolverBackend> CplexSolverBackend::clone() const
{
  auto cloned = std::make_unique<CplexSolverBackend>();

  // Clone the problem via CPXcloneprob
  int status = 0;
  CPXfreeprob(cloned->m_env_, &cloned->m_lp_);
  cloned->m_lp_ = CPXcloneprob(cloned->m_env_, m_lp_, &status);
  if (cloned->m_lp_ == nullptr) {
    throw std::runtime_error(
        std::format("CPLEX: CPXcloneprob failed with status {}", status));
  }

  return cloned;
}

}  // namespace gtopt
