/**
 * @file      sddp_feasibility.cpp
 * @brief     SDDP feasibility backpropagation implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp — implements the iterative feasibility
 * backpropagation loop that propagates infeasibility backward through
 * phases using elastic filter and Benders feasibility cuts.
 */

#include <gtopt/benders_cut.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

auto SDDPMethod::feasibility_backpropagate(SceneIndex scene_index,
                                           PhaseIndex start_phase,
                                           int total_cuts,
                                           const SolverOptions& opts,
                                           IterationIndex iteration_index)
    -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene_index];
  int cuts_added = 0;

  // Iterate backward from start_phase to phase 0
  for (auto back_phase = start_phase;; --back_phase) {
    if (back_phase > PhaseIndex {0}) {
      SPDLOG_WARN("{}: infeasible after cut, backpropagating to phase {}",
                  sddp_log("Backward",
                           iteration_index,
                           scene_uid(scene_index),
                           phase_uid(back_phase)),
                  phase_uid(back_phase - PhaseIndex {1}));
    }

    // Clone the LP, apply elastic filter, solve the clone.
    // The original LP is never modified by the elastic filter.
    auto elastic_result = elastic_solve(scene_index, back_phase, opts);
    if (elastic_result.has_value()) {
      if (back_phase > PhaseIndex {0}) {
        // Build a feasibility-like cut for the previous phase
        const auto prev_bp = back_phase - PhaseIndex {1};
        auto& prev_li =
            planning_lp().system(scene_index, prev_bp).linear_interface();
        const auto& prev_state = phase_states[prev_bp];

        if (m_options_.elastic_filter_mode == ElasticFilterMode::backpropagate)
        {
          // PLP mechanism: instead of building a feasibility cut,
          // propagate the elastic-clone dependent-column solution
          // values back as updated bounds on the source columns in
          // the previous phase.  This forces the previous phase to
          // produce a trial point that is known feasible for the
          // current phase, avoiding further infeasibility without
          // adding a cut row.
          const auto& dep_sol = elastic_result->clone.get_col_sol_raw();
          for (const auto& link : prev_state.outgoing_links) {
            const double feasible_val = dep_sol[link.dependent_col];
            prev_li.set_col_low_raw(link.source_col, feasible_val);
            prev_li.set_col_upp_raw(link.source_col, feasible_val);
          }
          SPDLOG_TRACE(
              "SDDP backward (BackpropagateBounds): scene {} phase {} "
              "bounds updated to elastic trial values",
              scene_index,
              prev_bp);
        } else {
          // single_cut or multi_cut mode:
          // Always add the regular Benders feasibility cut.
          auto feas_cut =
              build_benders_cut(prev_state.alpha_col,
                                prev_state.outgoing_links,
                                elastic_result->clone.get_col_cost_raw(),
                                elastic_result->clone.get_obj_value(),
                                m_options_.scale_alpha,
                                m_options_.cut_coeff_eps);
          feas_cut.class_name = "Sddp";
          feas_cut.constraint_name = "fcut";
          feas_cut.context = make_iteration_context(scene_uid(scene_index),
                                                    phase_uid(back_phase),
                                                    iteration_index,
                                                    total_cuts + cuts_added);
          rescale_benders_cut(
              feas_cut, prev_state.alpha_col, m_options_.cut_coeff_max);
          filter_cut_coefficients(
              feas_cut, prev_state.alpha_col, m_options_.cut_coeff_eps);

          {
            const auto cut_row = prev_li.add_row(feas_cut);
            store_cut(
                scene_index, prev_bp, feas_cut, CutType::Feasibility, cut_row);
          }
          ++cuts_added;

          // multi_cut: also add one bound-constraint cut per
          // state variable whose elastic slack was activated.
          // Auto-switch to multi_cut when:
          //   threshold == 0 (always), OR
          //   threshold > 0 and counter > threshold.
          const bool use_multi_cut =
              (m_options_.elastic_filter_mode == ElasticFilterMode::multi_cut)
              || (m_options_.multi_cut_threshold == 0)
              || (m_options_.multi_cut_threshold > 0
                  && m_infeasibility_counter_[scene_index][back_phase]
                      > m_options_.multi_cut_threshold);

          if (use_multi_cut) {
            auto mc_cuts = build_multi_cuts(
                *elastic_result,
                prev_state.outgoing_links,
                make_iteration_context(scene_uid(scene_index),
                                       phase_uid(back_phase),
                                       iteration_index,
                                       total_cuts + cuts_added));

            for (auto& mc : mc_cuts) {
              const auto cut_row = prev_li.add_row(mc);
              store_cut(
                  scene_index, prev_bp, mc, CutType::Feasibility, cut_row);
              ++cuts_added;
            }
          }
        }

        // Re-solve the previous phase with updated cuts or bounds
        auto r3 = prev_li.resolve(opts);
        if (r3.has_value() && prev_li.is_optimal()) {
          break;  // Feasibility restored
        }
        // Continue backpropagating to back_pi - 1
      } else {
        break;  // Restored at phase 0
      }
    } else if (back_phase == PhaseIndex {0}) {
      // Phase 0 with no elastic filter available = scene infeasible
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message =
              std::format("SDDP: scene {} is infeasible (backpropagated to "
                          "phase 0)",
                          scene_index),
      });
    }
  }

  return cuts_added;
}

}  // namespace gtopt
