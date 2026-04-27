/**
 * @file      osi_solver_backend.hpp
 * @brief     OSI-based solver backend for CLP and CBC
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>

class CoinMessageHandler;
class OsiSolverInterface;

namespace gtopt
{

/// Cached state used to replay options + logging + prob name onto a fresh
/// OsiSolverInterface.  Mirrors the CPLEX plugin's CplexPrep: reset_solver_()
/// and clone() read this cache and apply its fields to the new solver so
/// backend state survives a load_problem() cycle or a deep-copy clone.
struct OsiPrep
{
  std::optional<SolverOptions> options {};
  std::string log_filename {};
  int log_level {0};
  std::string prob_name {};
};

/**
 * @brief Solver backend using COIN-OR Open Solver Interface.
 *
 * Wraps OsiClpSolverInterface or OsiCbcSolverInterface depending
 * on the solver name passed to the constructor.
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class OsiSolverBackend final : public SolverBackend
{
public:
  enum class OsiSolverType : uint8_t
  {
    clp,
    cbc,
  };

  explicit OsiSolverBackend(OsiSolverType type);

  /// Constructor from a pre-existing solver (used by clone)
  OsiSolverBackend(OsiSolverType type,
                   std::shared_ptr<OsiSolverInterface> solver);

  ~OsiSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  // ---- problem name ----
  void set_prob_name(const std::string& name) override;
  [[nodiscard]] std::string get_prob_name() const override;

  // ---- bulk load ----
  void load_problem(int ncols,
                    int nrows,
                    const int* matbeg,
                    const int* matind,
                    const double* matval,
                    const double* collb,
                    const double* colub,
                    const double* obj,
                    const double* rowlb,
                    const double* rowub) override;

  // ---- dimensions ----
  [[nodiscard]] int get_num_cols() const override;
  [[nodiscard]] int get_num_rows() const override;

  // ---- column ops ----
  void add_col(double lb, double ub, double obj) override;
  void set_col_lower(int index, double value) override;
  void set_col_upper(int index, double value) override;
  void set_obj_coeff(int index, double value) override;

  // ---- row ops ----
  void add_row(int num_elements,
               const int* columns,
               const double* elements,
               double rowlb,
               double rowub) override;
  void add_rows(int num_rows,
                const int* rowbeg,
                const int* rowind,
                const double* rowval,
                const double* rowlb,
                const double* rowub) override;
  void set_row_lower(int index, double value) override;
  void set_row_upper(int index, double value) override;
  void set_row_bounds(int index, double lb, double ub) override;
  void delete_rows(int num, const int* indices) override;

  // ---- coefficients ----
  [[nodiscard]] double get_coeff(int row, int col) const override;
  void set_coeff(int row, int col, double value) override;
  [[nodiscard]] bool supports_set_coeff() const noexcept override;

  // ---- variable types ----
  void set_continuous(int index) override;
  void set_integer(int index) override;
  [[nodiscard]] bool is_continuous(int index) const override;
  [[nodiscard]] bool is_integer(int index) const override;

  // ---- solution access ----
  [[nodiscard]] const double* col_lower() const override;
  [[nodiscard]] const double* col_upper() const override;
  [[nodiscard]] const double* obj_coefficients() const override;
  [[nodiscard]] const double* row_lower() const override;
  [[nodiscard]] const double* row_upper() const override;
  [[nodiscard]] const double* col_solution() const override;
  [[nodiscard]] const double* reduced_cost() const override;
  [[nodiscard]] const double* row_price() const override;
  [[nodiscard]] double obj_value() const override;

  // ---- solution hints ----
  void set_col_solution(const double* sol) override;
  void set_row_price(const double* price) override;

  // ---- solve ----
  void initial_solve() override;
  void resolve() override;

  // ---- robust-solve mode ----
  void engage_robust_solve() override;
  void disengage_robust_solve() noexcept override;

  // ---- status ----
  [[nodiscard]] bool is_proven_optimal() const override;
  [[nodiscard]] bool is_abandoned() const override;
  [[nodiscard]] bool is_proven_primal_infeasible() const override;
  [[nodiscard]] bool is_proven_dual_infeasible() const override;
  // ---- options ----
  void apply_options(const SolverOptions& opts) override;
  [[nodiscard]] SolverOptions optimal_options() const override;
  [[nodiscard]] LPAlgo get_algorithm() const override;
  [[nodiscard]] int get_threads() const override;
  [[nodiscard]] bool get_presolve() const override;
  [[nodiscard]] int get_log_level() const override;

  // ---- diagnostics ----
  [[nodiscard]] std::optional<double> get_kappa() const override;

  // ---- logging ----
  void open_log(FILE* file, int level) override;
  void close_log() override;
  void set_log_filename(const std::string& filename, int level) override;
  void clear_log_filename() override;

  // ---- names ----
  void push_names(const std::vector<std::string>& col_names,
                  const std::vector<std::string>& row_names) override;
  void write_lp(const char* filename) override;

  // ---- clone ----
  [[nodiscard]] std::unique_ptr<SolverBackend> clone() const override;

private:
  /// Recreate m_solver_ + m_handler_ from m_prep_.  Used by load_problem()
  /// so every bulk load starts with a clean solver instance, guaranteeing
  /// no leftover per-LP state (basis, factorization, work arrays).
  void reset_solver_();

  OsiSolverType m_type_;
  std::shared_ptr<OsiSolverInterface> m_solver_;
  std::unique_ptr<CoinMessageHandler> m_handler_;

  /// Cache of everything needed to replay backend state onto a fresh
  /// OsiSolverInterface (see CplexPrep for the pattern).
  OsiPrep m_prep_;

  // Cached option values (updated by apply_options)
  LPAlgo m_algorithm_ {LPAlgo::default_algo};
  int m_threads_ {0};
  bool m_presolve_ {true};
  int m_log_level_ {0};

  /// Sanitised names cached from the most recent `push_names` call.
  /// `push_names` populates both the CLP-internal name store
  /// (`ClpModel::copyNames`) and the OSI base-class name vectors
  /// (`setColName`/`setRowName` under `OsiNameDiscipline=2`) so that
  /// `OsiSolverInterface::writeLp` emits real labels — the prior
  /// CLP-only fast path left the OSI side empty and the writer fell
  /// back to `R1, R2, ...`.  Sanitisation replaces characters that
  /// CoinLpIO's validator rejects (notably `-`) and fills empty slots
  /// with positional placeholders.
  std::vector<std::string> m_safe_col_names_;
  std::vector<std::string> m_safe_row_names_;

  /// Owned FILE* for set_log_filename; closed in clear_log_filename.
  struct FileCloser
  {
    void operator()(FILE* f) const noexcept
    {
      (void)std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
    }
  };
  using log_file_ptr_t = std::unique_ptr<FILE, FileCloser>;
  log_file_ptr_t m_log_file_ptr_;

  /// Snapshot of OSI/CLP tolerances captured by engage_robust_solve().
  struct RobustState
  {
    double dual_tolerance {};
    double primal_tolerance {};
    int presolve_passes {0};
    int engage_count {0};
  };
  std::optional<RobustState> m_saved_robust_state_;
};

}  // namespace gtopt
