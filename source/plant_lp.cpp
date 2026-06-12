/**
 * @file      plant_lp.cpp
 * @brief     LP build path for Plant elements
 * @date      Sat May 30 12:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Emits Σ-capacity (per-block), Σ-commit (per-stage), and Σ-status ≤ 1
 * (per-stage) rows for each `Plant` element.  Reuses already-built
 * `GeneratorLP.generation_cols` and `CommitmentLP.find_status_cols` —
 * no new LP columns are introduced.
 *
 * Wired into `SystemLP::collections_t` AFTER `CommitmentLP` so the
 * status columns are guaranteed to exist before the commit / uniq rows
 * reference them.
 */

#include <gtopt/commitment_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/plant_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

PlantLP::PlantLP(const Plant& plant, const InputContext& ic)
    : ObjectLP<Plant>(plant, ic, Element::class_name)
{
  generator_indices_.reserve(this->plant().generator_names.size());
  for (const auto& gname : this->plant().generator_names) {
    try {
      auto idx = ic.element_index(ObjectSingleId<GeneratorLP> {gname});
      generator_indices_.emplace_back(idx);
    } catch (const std::exception&) {
      SPDLOG_WARN(
          std::format("Plant '{}' references unknown generator '{}'; "
                      "the variant will be skipped at LP build",
                      this->plant().name,
                      gname));
      generator_indices_.emplace_back();
    }
  }
}

bool PlantLP::add_to_lp(const SystemContext& sc,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }
  if (generator_indices_.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();

  const auto opt_pmax = plant().pmax;
  const auto opt_n_units = plant().n_units;
  const auto want_uniq = plant().uniq_mutex.value_or(false);

  // Early out when nothing to enforce.
  if (!opt_pmax && !opt_n_units && !want_uniq) {
    return true;
  }

  // Resolve generation_cols for every known generator variant.
  std::vector<const BIndexHolder<ColIndex>*> gen_cols_per_variant;
  gen_cols_per_variant.reserve(generator_indices_.size());
  for (const auto& gidx : generator_indices_) {
    if (gidx == ElementIndex<GeneratorLP> {}) {
      gen_cols_per_variant.push_back(nullptr);
      continue;
    }
    auto&& glp = sc.element(gidx);
    if (!glp.is_active(stage)) {
      gen_cols_per_variant.push_back(nullptr);
      continue;
    }
    gen_cols_per_variant.push_back(
        &glp.lookup_generation_cols(scenario, stage));
  }

  // ── Σ-capacity rows: one per (stage, block) ───────────────────────────
  if (opt_pmax) {
    const auto pmax_val = *opt_pmax;
    BIndexHolder<RowIndex> brows;
    map_reserve(brows, blocks.size());
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      SparseRow row {
          .class_name = cname,
          .constraint_name = CapacityName,
          .variable_uid = uid(),
          .context = make_block_context(scenario.uid(), stage.uid(), buid),
      };
      row.less_equal(pmax_val);
      bool has_term = false;
      for (const auto* gcols : gen_cols_per_variant) {
        if (gcols == nullptr) {
          continue;
        }
        const auto it = gcols->find(buid);
        if (it == gcols->end()) {
          continue;
        }
        row[it->second] += 1.0;
        has_term = true;
      }
      if (!has_term) {
        continue;
      }
      brows[buid] = lp.add_row(std::move(row));
    }
    if (!brows.empty()) {
      capacity_rows_[{scenario.uid(), stage.uid()}] = std::move(brows);
    }
  }

  // For commit / uniq rows we need each variant's status column for
  // the FIRST block of the stage (CommitmentLP keys rows per block,
  // but Plant rows are stage-level: Σ status[i] in stage ≤ bound).
  // PLEXOS-style ``<plant>_Uniq`` / ``PlantCommit_*`` constraints
  // bind per-block too; we use the first-block status column as the
  // stage representative — when commitment_period > 1 block this is
  // exactly the same column as every other block of the period.  For
  // per-block-status plants this preserves the strongest cap (status
  // = 1 anywhere ⇒ row binds anywhere).  Per-block rows are a future
  // refinement covered by `n_units`-per-block schedules; v0 keeps
  // the simpler stage-level shape.
  if (opt_n_units || want_uniq) {
    // Build a map: variant index → status_col at first block (or none).
    std::vector<std::optional<ColIndex>> status_at_first_block;
    status_at_first_block.reserve(generator_indices_.size());
    const auto first_buid = blocks.front().uid();
    for (const auto& gidx : generator_indices_) {
      std::optional<ColIndex> resolved {};
      if (gidx != ElementIndex<GeneratorLP> {}) {
        for (const auto& cmt : sc.elements<CommitmentLP>()) {
          if (cmt.generator_sid()
              != ObjectSingleId<GeneratorLP> {sc.element(gidx).uid()})
          {
            continue;
          }
          const auto* ucols = cmt.find_status_cols(scenario, stage);
          if (ucols == nullptr) {
            continue;
          }
          const auto it = ucols->find(first_buid);
          if (it != ucols->end()) {
            resolved = it->second;
          }
          break;
        }
      }
      status_at_first_block.push_back(resolved);
    }

    if (opt_n_units) {
      const auto n_units_val = static_cast<double>(*opt_n_units);
      SparseRow row {
          .class_name = cname,
          .constraint_name = CommitName,
          .variable_uid = uid(),
          .context = make_stage_context(scenario.uid(), stage.uid()),
      };
      row.less_equal(n_units_val);
      bool has_term = false;
      const auto& coeffs = plant().commit_coeffs;
      for (std::size_t i = 0; i < status_at_first_block.size(); ++i) {
        const auto& maybe_col = status_at_first_block[i];
        if (!maybe_col) {
          continue;
        }
        const auto coeff = (i < coeffs.size()) ? coeffs[i] : 1.0;
        row[*maybe_col] += coeff;
        has_term = true;
      }
      if (has_term) {
        commit_rows_[{scenario.uid(), stage.uid()}] =
            lp.add_row(std::move(row));
      }
    }

    if (want_uniq) {
      SparseRow row {
          .class_name = cname,
          .constraint_name = UniqName,
          .variable_uid = uid(),
          .context = make_stage_context(scenario.uid(), stage.uid()),
      };
      row.less_equal(1.0);
      bool has_term = false;
      for (const auto& maybe_col : status_at_first_block) {
        if (!maybe_col) {
          continue;
        }
        row[*maybe_col] += 1.0;
        has_term = true;
      }
      if (has_term) {
        uniq_rows_[{scenario.uid(), stage.uid()}] = lp.add_row(std::move(row));
      }
    }
  }

  return true;
}

bool PlantLP::add_to_output(OutputContext& /*out*/)
{
  // Plant rows currently emit no per-cell output (the duals on
  // `plant_cap_*` / `plant_commit_*` / `plant_uniq_*` are already
  // visible via the standard solver-row dual export).  Hook left for
  // future per-stage Plant-specific metrics.
  return true;
}

}  // namespace gtopt
