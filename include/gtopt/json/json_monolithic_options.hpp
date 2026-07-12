/**
 * @file      json_monolithic_options.hpp
 * @brief     JSON serialization for MonolithicOptions
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_enum_option.hpp>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/monolithic_options.hpp>

namespace daw::json
{
using gtopt::BoundaryCutSharingMode;
using gtopt::BoundaryCutsMode;
using gtopt::MipStartDomainRules;
using gtopt::MipStartEffort;
using gtopt::MipStartInject;
using gtopt::MipStartOptions;
using gtopt::MipStartPeakInjection;
using gtopt::MipStartRelax;
using gtopt::MipStartRound;
using gtopt::MipStartScipRepair;
using gtopt::MipStartWindow;
using gtopt::MonolithicOptions;
using gtopt::RelaxInfeasibleAction;
using gtopt::SolveMode;
using gtopt::SolverOptions;

// ── MipStartWindow ──────────────────────────────────────────────────────────

struct MipStartWindowConstructor
{
  [[nodiscard]] MipStartWindow operator()(OptReal start, OptReal end) const
  {
    return MipStartWindow {.start = start, .end = end};
  }
};

template<>
struct json_data_contract<MipStartWindow>
{
  using constructor_t = MipStartWindowConstructor;

  using type = json_member_list<json_number_null<"start", OptReal>,
                                json_number_null<"end", OptReal>>;

  static auto to_json_data(MipStartWindow const& opt)
  {
    return std::make_tuple(opt.start, opt.end);
  }
};

// ── MipStartPeakInjection ───────────────────────────────────────────────────

struct MipStartPeakInjectionConstructor
{
  [[nodiscard]] MipStartPeakInjection operator()(
      OptBool enabled,
      std::optional<MipStartWindow> peak_window,
      std::optional<MipStartWindow> solar_window) const
  {
    return MipStartPeakInjection {
        .enabled = enabled,
        .peak_window = peak_window.value_or(MipStartWindow {}),
        .solar_window = solar_window.value_or(MipStartWindow {}),
    };
  }
};

template<>
struct json_data_contract<MipStartPeakInjection>
{
  using constructor_t = MipStartPeakInjectionConstructor;

  using type =
      json_member_list<json_bool_null<"enabled", OptBool>,
                       json_class_null<"peak_window", MipStartWindow>,
                       json_class_null<"solar_window", MipStartWindow>>;

  static auto to_json_data(MipStartPeakInjection const& opt)
  {
    return std::make_tuple(opt.enabled,
                           std::optional<MipStartWindow> {opt.peak_window},
                           std::optional<MipStartWindow> {opt.solar_window});
  }
};

// ── MipStartDomainRules ─────────────────────────────────────────────────────

struct MipStartDomainRulesConstructor
{
  [[nodiscard]] MipStartDomainRules operator()(
      OptBool min_up_down,
      OptBool commitment_logic,
      std::optional<MipStartPeakInjection> peak_injection) const
  {
    return MipStartDomainRules {
        .min_up_down = min_up_down,
        .commitment_logic = commitment_logic,
        .peak_injection = peak_injection.value_or(MipStartPeakInjection {}),
    };
  }
};

template<>
struct json_data_contract<MipStartDomainRules>
{
  using constructor_t = MipStartDomainRulesConstructor;

  using type = json_member_list<
      json_bool_null<"min_up_down", OptBool>,
      json_bool_null<"commitment_logic", OptBool>,
      json_class_null<"peak_injection", MipStartPeakInjection>>;

  static auto to_json_data(MipStartDomainRules const& opt)
  {
    return std::make_tuple(
        opt.min_up_down,
        opt.commitment_logic,
        std::optional<MipStartPeakInjection> {opt.peak_injection});
  }
};

// ── MipStartRelax ───────────────────────────────────────────────────────────

struct MipStartRelaxConstructor
{
  [[nodiscard]] MipStartRelax operator()(
      std::optional<SolverOptions> solver_options,
      OptBool check,
      OptBool report_saturated,
      OptName on_infeasible_str) const
  {
    MipStartRelax opts;
    opts.solver_options = std::move(solver_options);
    opts.check = check;
    opts.report_saturated = report_saturated;
    if (on_infeasible_str) {
      opts.on_infeasible = gtopt::require_enum<RelaxInfeasibleAction>(
          "on_infeasible", *on_infeasible_str);
    }
    return opts;
  }
};

template<>
struct json_data_contract<MipStartRelax>
{
  using constructor_t = MipStartRelaxConstructor;

  using type =
      json_member_list<json_class_null<"solver_options", SolverOptions>,
                       json_bool_null<"check", OptBool>,
                       json_bool_null<"report_saturated", OptBool>,
                       json_string_null<"on_infeasible", OptName>>;

  static auto to_json_data(MipStartRelax const& opt)
  {
    return std::make_tuple(opt.solver_options,
                           opt.check,
                           opt.report_saturated,
                           detail::enum_to_opt_name(opt.on_infeasible));
  }
};

// ── MipStartRound ───────────────────────────────────────────────────────────

struct MipStartRoundConstructor
{
  [[nodiscard]] MipStartRound operator()(OptReal threshold) const
  {
    return MipStartRound {.threshold = threshold};
  }
};

template<>
struct json_data_contract<MipStartRound>
{
  using constructor_t = MipStartRoundConstructor;

  using type = json_member_list<json_number_null<"threshold", OptReal>>;

  static auto to_json_data(MipStartRound const& opt)
  {
    return std::make_tuple(opt.threshold);
  }
};

// ── MipStartScipRepair ──────────────────────────────────────────────────────

struct MipStartScipRepairConstructor
{
  [[nodiscard]] MipStartScipRepair operator()(OptBool enabled) const
  {
    return MipStartScipRepair {.enabled = enabled};
  }
};

template<>
struct json_data_contract<MipStartScipRepair>
{
  using constructor_t = MipStartScipRepairConstructor;

  using type = json_member_list<json_bool_null<"enabled", OptBool>>;

  static auto to_json_data(MipStartScipRepair const& opt)
  {
    return std::make_tuple(opt.enabled);
  }
};

// ── MipStartInject ──────────────────────────────────────────────────────────

struct MipStartInjectConstructor
{
  [[nodiscard]] MipStartInject operator()(OptName effort_str) const
  {
    MipStartInject opts;
    if (effort_str) {
      opts.effort = gtopt::require_enum<MipStartEffort>("effort", *effort_str);
    }
    return opts;
  }
};

template<>
struct json_data_contract<MipStartInject>
{
  using constructor_t = MipStartInjectConstructor;

  using type = json_member_list<json_string_null<"effort", OptName>>;

  static auto to_json_data(MipStartInject const& opt)
  {
    return std::make_tuple(detail::enum_to_opt_name(opt.effort));
  }
};

// ── MipStartOptions ─────────────────────────────────────────────────────────

struct MipStartOptionsConstructor
{
  [[nodiscard]] MipStartOptions operator()(
      OptBool enabled,
      std::optional<MipStartRelax> relax,
      std::optional<MipStartRound> round,
      std::optional<MipStartDomainRules> domain_rules,
      std::optional<MipStartScipRepair> scip_repair,
      std::optional<MipStartInject> inject,
      OptName from_file,
      OptName dump_file,
      OptName seed_solution_file,
      OptBool skip_relaxation,
      OptName root_basis_cache_file) const
  {
    return MipStartOptions {
        .enabled = enabled,
        .relax = relax.value_or(MipStartRelax {}),
        .round = round.value_or(MipStartRound {}),
        .domain_rules = domain_rules.value_or(MipStartDomainRules {}),
        .scip_repair = scip_repair.value_or(MipStartScipRepair {}),
        .inject = inject.value_or(MipStartInject {}),
        .from_file = std::move(from_file),
        .dump_file = std::move(dump_file),
        .seed_solution_file = std::move(seed_solution_file),
        .skip_relaxation = skip_relaxation,
        .root_basis_cache_file = std::move(root_basis_cache_file),
    };
  }
};

template<>
struct json_data_contract<MipStartOptions>
{
  using constructor_t = MipStartOptionsConstructor;

  using type =
      json_member_list<json_bool_null<"enabled", OptBool>,
                       json_class_null<"relax", MipStartRelax>,
                       json_class_null<"round", MipStartRound>,
                       json_class_null<"domain_rules", MipStartDomainRules>,
                       json_class_null<"scip_repair", MipStartScipRepair>,
                       json_class_null<"inject", MipStartInject>,
                       json_string_null<"from_file", OptName>,
                       json_string_null<"dump_file", OptName>,
                       json_string_null<"seed_solution_file", OptName>,
                       json_bool_null<"skip_relaxation", OptBool>,
                       json_string_null<"root_basis_cache_file", OptName>>;

  static auto to_json_data(MipStartOptions const& opt)
  {
    return std::make_tuple(
        opt.enabled,
        std::optional<MipStartRelax> {opt.relax},
        std::optional<MipStartRound> {opt.round},
        std::optional<MipStartDomainRules> {opt.domain_rules},
        std::optional<MipStartScipRepair> {opt.scip_repair},
        std::optional<MipStartInject> {opt.inject},
        opt.from_file,
        opt.dump_file,
        opt.seed_solution_file,
        opt.skip_relaxation,
        opt.root_basis_cache_file);
  }
};

/// Custom constructor: converts JSON strings → typed enums
struct MonolithicOptionsConstructor
{
  [[nodiscard]] MonolithicOptions operator()(
      OptName solve_mode_str,
      OptName boundary_cuts_file,
      OptName boundary_cuts_mode_str,
      OptName boundary_cut_sharing_mode_str,
      OptInt boundary_max_iterations,
      std::optional<SolverOptions> solver_options,
      std::optional<MipStartOptions> mip_start) const
  {
    MonolithicOptions opts;
    if (solve_mode_str) {
      opts.solve_mode =
          gtopt::require_enum<SolveMode>("solve_mode", *solve_mode_str);
    }
    opts.boundary_cuts_file = std::move(boundary_cuts_file);
    if (boundary_cuts_mode_str) {
      opts.boundary_cuts_mode = gtopt::require_enum<BoundaryCutsMode>(
          "boundary_cuts_mode", *boundary_cuts_mode_str);
    }
    if (boundary_cut_sharing_mode_str) {
      opts.boundary_cut_sharing_mode =
          gtopt::require_enum<BoundaryCutSharingMode>(
              "boundary_cut_sharing_mode", *boundary_cut_sharing_mode_str);
    }
    opts.boundary_max_iterations = boundary_max_iterations;
    opts.solver_options = std::move(solver_options);
    opts.mip_start = std::move(mip_start);
    return opts;
  }
};

template<>
struct json_data_contract<MonolithicOptions>
{
  using constructor_t = MonolithicOptionsConstructor;

  using type =
      json_member_list<json_string_null<"solve_mode", OptName>,
                       json_string_null<"boundary_cuts_file", OptName>,
                       json_string_null<"boundary_cuts_mode", OptName>,
                       json_string_null<"boundary_cut_sharing_mode", OptName>,
                       json_number_null<"boundary_max_iterations", OptInt>,
                       json_class_null<"solver_options", SolverOptions>,
                       json_class_null<"mip_start", MipStartOptions>>;

  static auto to_json_data(MonolithicOptions const& opt)
  {
    return std::make_tuple(
        detail::enum_to_opt_name(opt.solve_mode),
        opt.boundary_cuts_file,
        detail::enum_to_opt_name(opt.boundary_cuts_mode),
        detail::enum_to_opt_name(opt.boundary_cut_sharing_mode),
        opt.boundary_max_iterations,
        opt.solver_options,
        opt.mip_start);
  }
};

}  // namespace daw::json
