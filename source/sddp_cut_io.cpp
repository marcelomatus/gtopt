/**
 * @file      sddp_cut_io.cpp
 * @brief     Cut persistence (save/load) for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut save/load free functions declared in sddp_cut_io.hpp.
 * Extracted from sddp_solver.cpp to reduce file size and improve
 * modularity.  All functions take explicit parameters rather than
 * accessing class members.
 */

#include <algorithm>
#include <cmath>
#include <string>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/fmap.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

// ─── UID lookup helpers ─────────────────────────────────────────────────────

[[nodiscard]] auto build_phase_uid_map(const PlanningLP& planning_lp)
    -> flat_map<PhaseUid, PhaseIndex>
{
  const auto& phases = planning_lp.simulation().phases();
  flat_map<PhaseUid, PhaseIndex> phase_map;
  map_reserve(phase_map, phases.size());
  for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
    phase_map.emplace(phase.uid(), pi);
  }
  return phase_map;
}

[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>
{
  const auto& scenes = planning_lp.simulation().scenes();
  flat_map<SceneUid, SceneIndex> scene_map;
  map_reserve(scene_map, scenes.size());
  for (auto&& [si, scene] : enumerate<SceneIndex>(scenes)) {
    scene_map.emplace(scene.uid(), si);
  }
  return scene_map;
}

// ─── Helper: state-variable name matching ───────────────────────────────────

// ─── Auto-scale alpha helper ────────────────────────────────────────────────

[[nodiscard]] auto boundary_cut_max_avg_coeff(const std::string& filepath)
    -> double
{
  // Read the boundary-cut CSV with the Arrow CSV reader — same path as
  // ``load_boundary_cuts_csv`` so layout detection (CRLF / quoting / type
  // inference) stays identical.  We then average each state column with the
  // Arrow ``mean`` compute kernel (null-aware) and return the max magnitude.
  auto maybe_infile = arrow::io::ReadableFile::Open(filepath);
  if (!maybe_infile.ok()) {
    return 0.0;
  }

  auto read_options = arrow::csv::ReadOptions::Defaults();
  auto parse_options = arrow::csv::ParseOptions::Defaults();
  auto convert_options = arrow::csv::ConvertOptions::Defaults();
  convert_options.column_types["iteration"] = arrow::int32();
  convert_options.column_types["scene"] = arrow::int32();
  convert_options.column_types["rhs"] = arrow::float64();
  convert_options.include_missing_columns = false;

  auto maybe_reader =
      arrow::csv::TableReader::Make(arrow::io::default_io_context(),
                                    *maybe_infile,
                                    read_options,
                                    parse_options,
                                    convert_options);
  if (!maybe_reader.ok()) {
    return 0.0;
  }
  auto maybe_table = (*maybe_reader)->Read();
  if (!maybe_table.ok()) {
    return 0.0;
  }
  const auto& table = *maybe_table;
  const auto schema = table->schema();

  // State-variable columns are every column after the structured prefix:
  // ``[iteration,] scene, rhs, <state_vars...>``.
  const bool has_iteration_col =
      schema->num_fields() > 0 && schema->field(0)->name() == "iteration";
  const int state_var_start = has_iteration_col ? 3 : 2;

  double max_coeff = 0.0;
  for (int fi = state_var_start; fi < schema->num_fields(); ++fi) {
    auto col = table->column(fi);
    // Cast to float64 so the mean kernel is uniform (state columns may be
    // inferred as int64 when every coefficient is integral).
    if (col->type()->id() != arrow::Type::DOUBLE) {
      arrow::compute::CastOptions cast_opts;
      cast_opts.to_type = arrow::float64();
      auto cast_result = arrow::compute::Cast(col, cast_opts);
      if (!cast_result.ok()) {
        continue;
      }
      col = cast_result->chunked_array();
    }
    auto mean_result = arrow::compute::Mean(col);
    if (!mean_result.ok()) {
      continue;
    }
    const auto& mean_scalar = mean_result->scalar_as<arrow::DoubleScalar>();
    if (!mean_scalar.is_valid) {
      continue;
    }
    max_coeff = std::max(max_coeff, std::abs(mean_scalar.value));
  }
  return max_coeff;
}

[[nodiscard]] auto effective_scale_alpha(const PlanningLP& planning_lp,
                                         double option_scale_alpha,
                                         double cut_max_coeff) -> double
{
  if (option_scale_alpha > 0.0) {
    return option_scale_alpha;
  }

  // α carries the future cost (money), so its column scale must cover BOTH
  // regimes it couples: the cut's ``wv·efin`` coefficients AND the objective
  // scale (every other money term α is added to).  ``scale_objective()`` is
  // 1.0 for SDDP/cascade and under ``--no-scale``, so the floor only lifts on
  // the scaled monolithic LP.
  const double obj_floor =
      std::max(1.0, planning_lp.options().scale_objective());

  // α only exists when boundary cuts are installed, so this is always called
  // with a cut to scale against.  Rule:
  //   scale_alpha = max( scale_objective , 10^ceil(log10(max_coeff)) )
  // where max_coeff = max_i |avg(coeff_i)| over the cut state columns.  This
  // matches α to the magnitude of the cut coefficients it balances and
  // mirrors the log10 round-up SDDP already uses for its α estimate
  // (sddp_method.cpp); rounding up to the next power of ten keeps α at a
  // clean decimal scale instead of an arbitrary coefficient value.  A
  // non-positive ``cut_max_coeff`` (unreadable / empty CSV — no usable cut)
  // degenerates to the objective floor.
  if (cut_max_coeff <= 0.0) {
    return obj_floor;
  }
  const double pow10 = std::pow(10.0, std::ceil(std::log10(cut_max_coeff)));
  return std::max(obj_floor, pow10);
}
// ``extract_iteration_from_name`` was removed in 2026-05.  Every
// consumer now reads the iteration index directly from the matching
// struct field (``StoredCut::iteration_index``,
// ``RawBoundaryCut::iteration_index``) rather than parsing it back out
// of a generated row label.  See:
//   * ``sddp_cut_parquet.cpp::load_cuts_parquet`` — reads the
//     ``iteration`` int32 column directly.
//   * ``sddp_boundary_cuts.cpp`` / ``sddp_named_cuts.cpp`` — both
//     parsers populate ``iteration_index`` while reading CSV rows
//     and reuse that variable downstream.

}  // namespace gtopt
