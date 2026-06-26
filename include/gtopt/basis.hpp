/**
 * @file      basis.hpp
 * @brief     Solver-agnostic simplex basis (warm-start) primitive
 * @date      2026-06-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A `Basis` is an *index vector*, not the constraint matrix: one status
 * tag per structural column and one per row (logical/slack).  Its size is
 * O(num_cols + num_rows), carrying no matrix data and no solution values
 * (~(n+m) bytes, a few hundred KB on a 45k-col GTEP stage LP).
 *
 * It exists to warm-start a dual (or primal) simplex re-solve of a closely
 * related LP.  Bound-only deltas (different inflow / incoming-state bounds)
 * leave reduced costs untouched, and appended rows (SDDP cuts) extend a
 * basis with their slack basic — both preserve dual feasibility, so the
 * captured basis is an exact dual-simplex seed for the neighbouring LP.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gtopt
{

/**
 * @brief Basis status of one column or row (slack) in a simplex basis.
 *
 * Solver-agnostic; each backend maps to/from its native encoding
 * (CPLEX `cstat`/`rstat`, HiGHS `HighsBasisStatus`, OSI
 * `CoinWarmStartBasis::Status`).  The numeric values intentionally match
 * the CPLEX `CPX_AT_LOWER`/`CPX_BASIC`/`CPX_AT_UPPER`/`CPX_FREE_SUPER`
 * constants, but backends map explicitly rather than rely on that.
 */
enum class BasisStatus : uint8_t
{
  at_lower = 0,  ///< nonbasic, at lower bound
  basic = 1,  ///< in the basis
  at_upper = 2,  ///< nonbasic, at upper bound
  free = 3,  ///< nonbasic free / superbasic / at zero
};

/**
 * @brief A captured simplex basis: one `BasisStatus` per column and row.
 *
 * `col_status.size()` is the structural column count and
 * `row_status.size()` the row count of the LP it was captured from.
 * Empty (`empty()`) means "no basis" — used by callers as the cold-start
 * sentinel on the first iteration.
 */
struct Basis
{
  std::vector<BasisStatus> col_status;  ///< length == num structural cols
  std::vector<BasisStatus> row_status;  ///< length == num rows

  [[nodiscard]] bool empty() const noexcept { return col_status.empty(); }
  [[nodiscard]] std::size_t num_cols() const noexcept
  {
    return col_status.size();
  }
  [[nodiscard]] std::size_t num_rows() const noexcept
  {
    return row_status.size();
  }
  void clear() noexcept
  {
    col_status.clear();
    row_status.clear();
  }
};

/**
 * @brief Reconcile a basis captured from a related LP to current dimensions.
 *
 * The common SDDP case: between the iteration that captured the basis and
 * the one re-using it, cuts have appended rows (column count is stable —
 * the cost-to-go `alpha` column already exists).  This:
 *
 *   - grows `row_status` to `nrows`, the appended rows entering the basis
 *     (slack basic) — the textbook extension that keeps a dual-feasible
 *     basis valid after adding constraints;
 *   - grows `col_status` to `ncols`, appended columns nonbasic at lower;
 *   - truncates surplus statuses if the LP instead shrank (cuts dropped).
 *
 * Each appended row contributes exactly one basic slack, so the basis stays
 * square (`#basic == nrows`) and the backend accepts it verbatim.
 */
inline void reconcile_basis(Basis& basis,
                            std::size_t ncols,
                            std::size_t nrows) noexcept
{
  basis.col_status.resize(ncols, BasisStatus::at_lower);
  basis.row_status.resize(nrows, BasisStatus::basic);
}

}  // namespace gtopt
