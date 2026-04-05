/**
 * @file      output_context.hpp
 * @brief     Output context for writing LP results to Parquet/CSV
 * @date      Mon Mar 24 20:39:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the OutputContext class and the probe_parquet_codec
 * utility for writing LP solution results to Parquet or CSV files.
 */

#pragma once

#include <string>
#include <string_view>

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

/// Probe the Arrow/Parquet runtime to determine the best available codec for
/// the requested name.  Uses `arrow::util::Codec::IsAvailable()` — the
/// correct runtime check — rather than `parquet::IsCodecSupported()`, which
/// only validates the enum value and does **not** detect codecs that were
/// absent when the Arrow library was compiled.
///
/// Falls back (with a WARN log) to `"gzip"`, then to `""` (uncompressed) when
/// the requested codec is unavailable.
///
/// **Call once at program startup** (e.g. in `gtopt_main()` after loading
/// options) and store the result in `planning.options.output_compression` so
/// that every downstream write uses the same pre-validated codec without
/// re-probing on each file.
[[nodiscard]] std::string probe_parquet_codec(std::string_view requested);

class OutputContext
{
public:
  using ArrowFields = std::vector<ArrowField>;
  using ArrowArrays = std::vector<ArrowArray>;
  using ArrowFieldArrays = std::pair<ArrowFields, ArrowArrays>;

  using ValidVector = std::vector<bool>;
  template<typename Type = double>
  using FieldType =
      std::tuple<Name, std::vector<Type>, ValidVector, const ArrowFieldArrays*>;

  template<typename Type = double>
  using FieldVector = std::vector<FieldType<Type>>;

  using ClassFieldName = std::pair<std::string_view, Name>;
  template<typename Type = double>
  using FieldVectorMap = std::map<ClassFieldName, FieldVector<Type>>;

  explicit OutputContext(const SystemContext& psc,
                         LinearInterface& linear_interface,
                         SceneUid scene_uid = SceneUid {0},
                         PhaseUid phase_uid = PhaseUid {0});

  [[nodiscard]] auto&& options() const { return sc.get().options(); }

  // ── STB/GSTB block-indexed overloads ─────────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const GSTBIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
  }

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const STBIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &stb_prelude,
              block_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const GSTBIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const STBIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const GSTBIndexHolder<RowIndex>& holder)
  {
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STBIndexHolder<RowIndex>& holder)
  {
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              sc.get().block_icost_factors());
  }

  /// add_row_dual with an additional per-(scenario,stage) back-scale factor.
  /// Used by StorageLP::add_to_output for daily-cycle dual correction.
  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STBIndexHolder<RowIndex>& holder,
                              const STIndexHolder<double>& st_scale)
  {
    add_field_st_scaled(
        cname, row_name, "dual", id, holder, row_dual_span, st_scale);
  }

  /// add_row_dual using discount-only scaling (`scale_obj / discount[t]`).
  constexpr void add_row_dual_raw(std::string_view cname,
                                  std::string_view row_name,
                                  const Id& id,
                                  const STBIndexHolder<RowIndex>& holder)
  {
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &stb_prelude,
              sc.get().block_discount_icost_factors());
  }

  // ── ST scenario-stage-indexed overloads ──────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const STIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &st_prelude,
              scenario_stage_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const STIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &st_prelude,
              sc.get().scenario_stage_icost_factors());
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const STIndexHolder<RowIndex>& holder)
  {
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &st_prelude,
              sc.get().scenario_stage_icost_factors());
  }

  // ── T stage-indexed overloads ────────────────────────────────────

  constexpr void add_col_sol(std::string_view cname,
                             std::string_view col_name,
                             const Id& id,
                             const TIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "sol",
              id,
              holder,
              col_sol_span,
              &t_prelude,
              stage_factor_matrix_t {});
  }

  constexpr void add_col_cost(std::string_view cname,
                              std::string_view col_name,
                              const Id& id,
                              const TIndexHolder<ColIndex>& holder)
  {
    add_field(cname,
              col_name,
              "cost",
              id,
              holder,
              col_cost_span,
              &t_prelude,
              sc.get().stage_icost_factors());
  }

  constexpr void add_row_dual(std::string_view cname,
                              std::string_view row_name,
                              const Id& id,
                              const TIndexHolder<RowIndex>& holder)
  {
    add_field(cname,
              row_name,
              "dual",
              id,
              holder,
              row_dual_span,
              &t_prelude,
              sc.get().stage_icost_factors());
  }

  void write() const;

private:
  std::reference_wrapper<const SystemContext> sc;

  SceneUid m_scene_uid_;
  PhaseUid m_phase_uid_;

  ScaledView col_sol_span;
  ScaledView col_cost_span;
  ScaledView row_dual_span;

  ArrowFieldArrays stb_prelude;
  ArrowFieldArrays st_prelude;
  ArrowFieldArrays t_prelude;

  FieldVectorMap<double> field_vector_map;

  // ── private helpers ──────────────────────────────────────────────

  [[nodiscard]] constexpr auto field_name(const Id& id) const
  {
    return options().use_uid_fname() ? as_label<':'>("uid", get_uid(id))
                                     : as_label<':'>(get_name(id), get_uid(id));
  }

  template<typename IndexHolder,
           typename Span,
           typename Prelude,
           typename Factor = std::span<double>>
  void add_field(std::string_view cname,
                 std::string_view fname,
                 std::string_view sname,
                 const Id& id,
                 const IndexHolder& holder,
                 const Span& value_span,
                 const Prelude* prelude,
                 const Factor& factor)
  {
    if (holder.empty() || value_span.empty()) {
      return;
    }

    auto&& [values, valid] =
        sc.get().flat(holder, [&](auto i) { return value_span[i]; }, factor);

    if (values.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, as_label(fname, sname)}]
        .emplace_back(
            field_name(id), std::move(values), std::move(valid), prelude);
  }

  /// add_field variant with additional per-(scenario,stage) back-scale.
  template<typename IndexHolder, typename Span>
  void add_field_st_scaled(std::string_view cname,
                           std::string_view fname,
                           std::string_view sname,
                           const Id& id,
                           const IndexHolder& holder,
                           const Span& value_span,
                           const STIndexHolder<double>& st_scale)
  {
    if (holder.empty() || value_span.empty()) {
      return;
    }

    auto&& [values, valid] = sc.get().flat(
        holder,
        [&](auto i) { return value_span[i]; },
        sc.get().block_icost_factors(),
        st_scale);

    if (values.empty()) {
      return;
    }

    field_vector_map[ClassFieldName {cname, as_label(fname, sname)}]
        .emplace_back(
            field_name(id), std::move(values), std::move(valid), &stb_prelude);
  }
};

}  // namespace gtopt
