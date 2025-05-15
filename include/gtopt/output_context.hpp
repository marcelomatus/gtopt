/**
 * @file      output_context.hpp
 * @brief     Header of
 * @date      Mon Mar 24 20:39:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

class OutputContext
{
public:
  using ArrowFields = std::vector<ArrowField>;
  using ArrowArrays = std::vector<ArrowArray>;
  using ArrowFieldArrays = std::pair<ArrowFields, ArrowArrays>;

  using ColSolSpan = decltype(std::declval<LinearInterface&>().get_col_sol());
  using RowDualSpan = decltype(std::declval<LinearInterface&>().get_row_dual());
  using ColCostSpan = decltype(std::declval<LinearInterface&>().get_col_cost());

  using ValidVector = std::vector<bool>;
  template<typename Type = double>
  using FieldType =
      std::tuple<Name, std::vector<Type>, ValidVector, const ArrowFieldArrays*>;

  template<typename Type = double>
  using FieldVector = std::vector<FieldType<Type>>;

  using ClassFieldName = std::pair<Name, Name>;
  template<typename Type = double>
  using FieldVectorMap = std::map<ClassFieldName, FieldVector<Type>>;

  explicit OutputContext(const SystemContext& psc,
                         const LinearInterface& linear_interface);

  [[nodiscard]] auto&& options() const { return sc.get().options(); }

  template<typename Holder, typename Op, typename Factor>
  constexpr auto flat(const Holder& holder, Op op, Factor factor) const
  {
    return sc.get().flat(holder, op, factor);
  }

  [[nodiscard]] constexpr auto field_name(const Id& id) const
  {
    return options().use_uid_fname() ? as_label<':'>("uid", get_uid(id))
                                     : as_label<':'>(get_name(id), get_uid(id));
  }

  template<typename IndexHolder,
           typename Span,
           typename Prelude,
           typename Operation,
           typename Factor = std::span<double>>
  void add_field(const std::string_view& cname,
                 const std::string_view& fname,
                 const std::string_view& sname,
                 const Id& id,
                 const IndexHolder& holder,
                 const Span& value_span,
                 const Prelude* prelude,
                 Operation op,
                 const Factor& factor)
  {
    if (holder.empty()) {
      return;
    }

    auto&& [values, valid] =
        flat(holder, [&](auto i) { return op(value_span[i]); }, factor);

    if (values.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, as_label(fname, sname)}]
        .emplace_back(
            field_name(id), std::move(values), std::move(valid), prelude);
  }

  template<typename Operation = std::identity>
  constexpr void add_col_sol(const std::string_view& cname,
                             const std::string_view& col_name,
                             const Id& id,
                             const GSTBIndexHolder& holder,
                             Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     col_sol_span,
                     &stb_prelude,
                     op,
                     block_factor_matrix_t {});
  }

  template<typename Operation = std::identity>
  constexpr void add_col_sol(const std::string_view& cname,
                             const std::string_view& col_name,
                             const Id& id,
                             const STBIndexHolder& holder,
                             Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     col_sol_span,
                     &stb_prelude,
                     op,
                     block_factor_matrix_t {});
  }

  template<typename Operation = std::identity>
  constexpr void add_col_cost(const std::string_view& cname,
                              const std::string_view& col_name,
                              const Id& id,
                              const GSTBIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     col_cost_span,
                     &stb_prelude,
                     op,
                     block_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_col_cost(const std::string_view& cname,
                              const std::string_view& col_name,
                              const Id& id,
                              const STBIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     col_cost_span,
                     &stb_prelude,
                     op,
                     block_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_row_dual(const std::string_view& cname,
                              const std::string_view& row_name,
                              const Id& id,
                              const GSTBIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     row_name,
                     "dual",
                     id,
                     holder,
                     row_dual_span,
                     &stb_prelude,
                     op,
                     block_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_row_dual(const std::string_view& cname,
                              const std::string_view& row_name,
                              const Id& id,
                              const STBIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     row_name,
                     "dual",
                     id,
                     holder,
                     row_dual_span,
                     &stb_prelude,
                     op,
                     block_cost_factors);
  }

  ///
  template<typename Operation = std::identity>
  constexpr void add_col_sol(const std::string_view& cname,
                             const std::string_view& col_name,
                             const Id& id,
                             const STIndexHolder& holder,
                             Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     col_sol_span,
                     &st_prelude,
                     op,
                     scenario_stage_factor_matrix_t {});
  }

  template<typename Operation = std::identity>
  constexpr void add_col_cost(const std::string_view& cname,
                              const std::string_view& col_name,
                              const Id& id,
                              const STIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     col_cost_span,
                     &st_prelude,
                     op,
                     scenario_stage_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_row_dual(const std::string_view& cname,
                              const std::string_view& row_name,
                              const Id& id,
                              const STIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     row_name,
                     "dual",
                     id,
                     holder,
                     row_dual_span,
                     &st_prelude,
                     op,
                     scenario_stage_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_col_sol(const std::string_view& cname,
                             const std::string_view& col_name,
                             const Id& id,
                             const TIndexHolder& holder,
                             Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "sol",
                     id,
                     holder,
                     col_sol_span,
                     &t_prelude,
                     op,
                     stage_factor_matrix_t {});
  }

  template<typename Operation = std::identity>
  constexpr void add_col_cost(const std::string_view& cname,
                              const std::string_view& col_name,
                              const Id& id,
                              const TIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     col_name,
                     "cost",
                     id,
                     holder,
                     col_cost_span,
                     &t_prelude,
                     op,
                     stage_cost_factors);
  }

  template<typename Operation = std::identity>
  constexpr void add_row_dual(const std::string_view& cname,
                              const std::string_view& row_name,
                              const Id& id,
                              const TIndexHolder& holder,
                              Operation op = {})
  {
    return add_field(cname,
                     row_name,
                     "dual",
                     id,
                     holder,
                     row_dual_span,
                     &t_prelude,
                     op,
                     stage_cost_factors);
  }

  void write() const;

private:
  std::reference_wrapper<const SystemContext> sc;

  double sol_obj_value;
  double sol_status;
  double sol_kappa;

  ColSolSpan col_sol_span;
  ColCostSpan col_cost_span;
  RowDualSpan row_dual_span;

  block_factor_matrix_t block_cost_factors;
  stage_factor_matrix_t stage_cost_factors;
  scenario_stage_factor_matrix_t scenario_stage_cost_factors;

  ArrowFieldArrays stb_prelude;
  ArrowFieldArrays st_prelude;
  ArrowFieldArrays t_prelude;

  FieldVectorMap<double> field_vector_map;
};

}  // namespace gtopt
