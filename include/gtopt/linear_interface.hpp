/**
 * @file      linear_interface.hpp
 * @brief     Header of
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <memory>

#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_options.hpp>
#include <gtopt/osi_solver.hpp>

namespace gtopt
{

class LinearInterface
{
public:
  using SolverInterface = ::osiSolverInterface;
  using solver_ptr_t = std::shared_ptr<SolverInterface>;

  LinearInterface(const LinearInterface&) = delete;
  LinearInterface& operator=(const LinearInterface&) = delete;
  LinearInterface(LinearInterface&&) = delete;
  LinearInterface& operator=(LinearInterface&&) = delete;

  LinearInterface(solver_ptr_t psolver, std::string plog_file);
  explicit LinearInterface(const std::string& plog_file = {});
  explicit LinearInterface(const FlatLinearProblem& flat_lp,
                           const std::string& plog_file = {});

  ~LinearInterface() = default;

  void load_flat(const FlatLinearProblem& flat_lp);

  size_t add_col(const std::string& name);
  size_t add_col(const std::string& name, double collb, double colub);
  size_t add_free_col(const std::string& name);

  size_t add_row(const SparseRow& row, double eps = 0.0);

  [[nodiscard]] size_t get_numrows() const;
  [[nodiscard]] size_t get_numcols() const;

  void set_rhs(size_t row, double rhs);
  void set_row_low(size_t index, double value);
  void set_row_upp(size_t index, double value);
#ifdef OSI_EXTENDED
  double get_coeff(size_t row, size_t column) const;
  void set_coeff(size_t row, size_t column, double value);
#endif
  void set_obj_coeff(size_t index, double value);
  [[nodiscard]] double get_obj_coeff(size_t index) const;

  void set_col_low(size_t index, double value);
  void set_col_upp(size_t index, double value);
  void set_col(size_t index, double value);

  [[nodiscard]] double get_obj_value() const;

  void write_lp(const std::string& filename) const;

  [[nodiscard]] bool initial_solve(const LPOptions& lp_options = {});
  [[nodiscard]] bool resolve(const LPOptions& lp_options = {});

  [[nodiscard]] double get_kappa() const;
  [[nodiscard]] int get_status() const;
  [[nodiscard]] bool is_optimal() const;
  [[nodiscard]] bool is_dual_infeasible() const;
  [[nodiscard]] bool is_prim_infeasible() const;

  void set_continuous(size_t index);
  void set_integer(size_t index);
  void set_binary(size_t index);

  [[nodiscard]] bool is_continuous(size_t index) const;
  [[nodiscard]] bool is_integer(size_t index) const;

  void set_time_limit(double time_limit);

  [[nodiscard]] auto get_row_low() const
  {
    return std::span(solver->getRowLower(), get_numrows());
  }

  [[nodiscard]] auto get_row_upp() const
  {
    return std::span(solver->getRowUpper(), get_numrows());
  }

  [[nodiscard]] auto get_col_low() const
  {
    return std::span(solver->getColLower(), get_numcols());
  }

  [[nodiscard]] auto get_col_upp() const
  {
    return std::span(solver->getColUpper(), get_numcols());
  }

  [[nodiscard]] auto get_col_sol() const
  {
    return std::span(solver->getColSolution(), get_numcols());
  }

  [[nodiscard]] auto get_col_cost() const
  {
    return std::span(solver->getReducedCost(), get_numcols());
  }

  [[nodiscard]] auto get_row_dual() const
  {
    return std::span(solver->getRowPrice(), get_numrows());
  }

  void set_col_sol(std::span<const double> sol);
  void set_row_dual(std::span<const double> dual);

  void set_log_file(const std::string& plog_file);
  [[nodiscard]] constexpr const auto& get_log_file() const { return log_file; }

  void set_prob_name(const std::string& pname);

private:
  void set_solver_opts(const LPOptions& lp_options);

  size_t add_row(const std::string& name,
                 size_t numberElements,
                 const std::span<const int>& columns,
                 const std::span<const double>& elements,
                 double rowlb,
                 double rowub);

  void open_log_handler(int log_level);
  void close_log_handler();

  struct HandlerGuard
  {
    LinearInterface* interface;

    HandlerGuard(HandlerGuard const&) = delete;
    HandlerGuard& operator=(HandlerGuard const&) = delete;
    HandlerGuard(HandlerGuard&&) = delete;
    HandlerGuard& operator=(HandlerGuard&&) = delete;

    explicit HandlerGuard(LinearInterface& pinterface, int log_level)
        : interface(&pinterface)
    {
      interface->open_log_handler(log_level);
    }

    ~HandlerGuard() { interface->close_log_handler(); }
  };

  solver_ptr_t solver;
  std::string log_file {};

  struct FILEcloser
  {
    auto operator()(auto f) const { return fclose(f); }
  };
  using log_file_ptr_t = std::unique_ptr<FILE, FILEcloser>;
  log_file_ptr_t log_file_ptr;
  std::unique_ptr<CoinMessageHandler> handler {};
};

}  // namespace gtopt
