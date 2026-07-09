/**
 * @file      sddp_investment_master.cpp
 * @brief     OptGen-style investment-master Benders loop — implementation.
 * @date      2026-07-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_capacity_cuts.hpp>
#include <gtopt/sddp_investment_master.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Resolve an `OptTRealFieldSched` to a scalar (broadcast / first-stage
/// value).  Mirrors `emission_source_lp.cpp::resolve_stage_scalar`: the
/// master treats an expansion candidate's `expcap`/`expmod` as a single
/// here-and-now magnitude, so the scalar (or first vector entry) is what
/// it needs.  FileSched-backed schedules are not expandable without the
/// InputContext machinery and yield 0 (the candidate is then skipped by
/// the positive-headroom gate below).
[[nodiscard]] double field_scalar(const OptTRealFieldSched& field) noexcept
{
  if (!field.has_value()) {
    return 0.0;
  }
  const auto& val = *field;
  if (const auto* r = std::get_if<Real>(&val)) {
    return *r;
  }
  if (const auto* vec = std::get_if<std::vector<Real>>(&val)) {
    return vec->empty() ? 0.0 : vec->front();
  }
  return 0.0;
}

/// Epsilon expmod ceiling used to pin a candidate's build to ZERO while
/// keeping its `capainst`/`capacost` state published.  Small enough that
/// the operational run evaluates the true zero-build point (max buildable
/// capacity = `expcap · kZeroPinExpmod` ≈ 0), yet strictly positive so the
/// `expcap · expmod > 0` gate in `CapacityObjectBase::add_to_lp` still
/// fires — the master needs a live `capainst` column at K̂ ≈ 0 to read the
/// value-function subgradient there.
constexpr double kZeroPinExpmod = 1.0e-6;

/// Pin a candidate's `expmod` build ceiling to EXACTLY the master proposal
/// `n` (the pure-data capacity-fixing route, design §7.3 caveat 4).  The
/// build stays an `expmod` column so its CAPEX is still charged on the
/// `capacost` state — the operational cost, hence the full-value cut RHS
/// that anchors θ_s, stays inclusive of investment cost.  `expcap` is
/// untouched.
///
///   * `n > 0` → `expmod = n`: the operational LP builds min(n, its own
///     optimum) = n (building the cheap unit is operationally worthwhile),
///     so the run prices exactly the proposal's K = expcap·n and the
///     full-value cut is anchored there.
///   * `n == 0` → `expmod = kZeroPinExpmod` (≈ 0, not the original
///     headroom): this genuinely fixes the operational run to the
///     zero-build point so its cost V_s(0) and the capainst subgradient
///     are read at K ≈ 0.  Keeping the ORIGINAL headroom here (the pre-fix
///     behaviour) let the operational LP build the unit ANYWAY — the
///     master has no CAPEX term, so it never saw the zero-build cost and
///     never learned to reject build 0 (the loop stalled at build 0 with
///     LB far below the optimum).  The tiny-but-positive ceiling keeps the
///     `capainst` state published so the cut still flows.
void pin_expmod(OptTRealFieldSched& expmod, double n)
{
  expmod = RealFieldSched {n > 0.0 ? n : kZeroPinExpmod};
}

/// True when @p g / @p d is an expansion candidate (positive build
/// headroom) — the same `expcap · expmod > 0` gate
/// `CapacityObjectBase::add_to_lp` uses to publish `capainst`.
template<typename Elem>
[[nodiscard]] std::optional<ExpansionCandidate> as_candidate(
    const Elem& elem, std::string_view class_name)
{
  const double expcap = field_scalar(elem.expcap);
  const double expmod = field_scalar(elem.expmod);
  if (expcap <= 0.0 || expmod <= 0.0) {
    return std::nullopt;
  }
  const bool is_int =
      elem.integer_expmod.has_value() ? *elem.integer_expmod : true;
  return ExpansionCandidate {
      .class_name = std::string {class_name},
      .uid = elem.uid,
      .name = elem.name,
      .expcap = expcap,
      .max_expmod = expmod,
      .integer = is_int,
  };
}

}  // namespace

auto find_expansion_candidates(const Planning& planning)
    -> std::vector<ExpansionCandidate>
{
  std::vector<ExpansionCandidate> out;
  for (const auto& g : planning.system.generator_array) {
    if (auto c = as_candidate(g, "Generator")) {
      out.push_back(std::move(*c));
    }
  }
  for (const auto& d : planning.system.demand_array) {
    if (auto c = as_candidate(d, "Demand")) {
      out.push_back(std::move(*c));
    }
  }
  return out;
}

namespace
{

/// The master MIP, built once on a standalone `LinearInterface` and
/// re-solved after each batch of cuts.  Columns (candidate order):
///   * `n_g`  — integer build count in `[0, max_expmod]`
///   * `K_g`  — accumulated capacity `= base + expcap · n_g` (equality row)
///   * `θ_s`  — one per scene, free-below, objective coefficient 1
/// The master aggregates each candidate's capacity into ONE `K_g` (single
/// here-and-now build, v1); the capacity accounting row is
///   `K_g − expcap · n_g = base_capacity_g`.
class MasterMIP
{
public:
  MasterMIP(const std::vector<ExpansionCandidate>& candidates,
            std::size_t num_scenes,
            const std::vector<double>& base_capacity,
            std::string_view solver_name,
            SolverOptions solver_opts)
      : m_lp_(solver_name)
      , m_num_candidates_(candidates.size())
      , m_num_scenes_(num_scenes)
      , m_solver_opts_(std::move(solver_opts))
  {
    // Pin a tight MIP gap so the master returns a bit-exact integer
    // optimum — the LB it reports must not be loosened by an early
    // branch-and-bound cutoff (design §6.3 ε-validity, applied to the
    // master this time).
    m_solver_opts_.mip_gap = 1.0e-9;
    m_solver_opts_.mip_gap_abs = 1.0e-9;

    m_n_col_.reserve(m_num_candidates_);
    m_k_col_.reserve(m_num_candidates_);
    for (const auto& [i, cand] : enumerate(candidates)) {
      const auto n_col = m_lp_.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = cand.max_expmod,
          .cost = 0.0,
      });
      if (cand.integer) {
        m_lp_.set_integer(n_col);
      }
      const auto k_col = m_lp_.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = base_capacity[i] + (cand.expcap * cand.max_expmod),
          .cost = 0.0,
      });
      // K_g − expcap · n_g = base_capacity_g.
      SparseRow acc;
      acc[k_col] = 1.0;
      acc[n_col] = -cand.expcap;
      (void)m_lp_.add_row(acc.equal(base_capacity[i]));
      m_n_col_.push_back(n_col);
      m_k_col_.push_back(k_col);
    }

    // One epigraph θ_s per scene.  θ is a signed cost-to-go, floored
    // well below any physically attainable operational cost so the very
    // first master solve (no cuts) is bounded rather than −∞.  Each cut
    // then lifts the floor toward the true expected cost.
    m_theta_col_.reserve(m_num_scenes_);
    for (std::size_t s = 0; s < m_num_scenes_; ++s) {
      m_theta_col_.push_back(m_lp_.add_col(SparseCol {
          .lowb = -theta_floor_,
          .uppb = DblMax,
          .cost = 1.0,
      }));
    }
  }

  /// Map a candidate (class, uid) to its `K_g` column, or nullopt.
  [[nodiscard]] std::optional<ColIndex> k_col_for(
      const std::vector<ExpansionCandidate>& cands,
      std::string_view class_name,
      Uid uid) const
  {
    for (const auto& [i, c] : enumerate(cands)) {
      if (c.class_name == class_name && c.uid == uid) {
        return m_k_col_[i];
      }
    }
    return std::nullopt;
  }

  /// Add one capacity cut, projected onto master `K` coordinates, to the
  /// scene's epigraph:  `θ_s ≥ rhs − Σ_j coeff_j · K_j`, i.e.
  /// `θ_s + Σ_j coeff_j · K_j ≥ rhs` (the stored row convention).
  /// Coefficients on the SAME `K_j` (repeated per-stage capainst) are
  /// accumulated — the master's single `K_g` aggregates the horizon.
  /// @return true when the cut references at least one resolvable K.
  bool add_capacity_cut(const std::vector<ExpansionCandidate>& cands,
                        std::size_t scene_index,
                        const CapacityCut& cut)
  {
    SparseRow row;
    row[m_theta_col_[scene_index]] = 1.0;
    bool any = false;
    for (const auto& coeff : cut.coefficients) {
      // Only the installed-capacity coordinate maps to a master build
      // decision; `capacost` is the CAPEX accumulator already folded
      // into θ via the cut RHS, so it needs no master column.
      if (coeff.col_name != CapacityObjectBase::CapainstName) {
        continue;
      }
      const auto k = k_col_for(cands, coeff.class_name, coeff.uid);
      if (!k.has_value()) {
        continue;
      }
      row[*k] += coeff.coeff;
      any = true;
    }
    if (!any) {
      return false;
    }
    (void)m_lp_.add_row(row.greater_equal(cut.rhs));
    return true;
  }

  /// Add a FULL-value Benders optimality cut on scene @p scene_index's
  /// epigraph — the correct support of the per-scene operational value
  /// Φ_s(K) = V_s^{(0)}(K) (the WHOLE phase-0 objective, stage-1 dispatch
  /// + CAPEX + cost-to-go), NOT merely the phase-0 α tail.
  ///
  /// Given the pinned build capacities K̂_j, the value V_s = Φ_s(K̂) and
  /// the subgradients g_j = ∂Φ_s/∂K_j (the sum over every (phase, block)
  /// of the `capacity` constraint dual `generation ≤ capainst`, NOT the
  /// structurally-zero basic `capainst` column reduced cost — see
  /// `SceneCapSubgradients`; in the same probability-folded LP space as
  /// θ_s), convexity of Φ_s gives the valid support
  ///
  ///     θ_s ≥ V_s + Σ_j g_j · (K_j − K̂_j)
  ///         = (V_s − Σ_j g_j·K̂_j) + Σ_j g_j·K_j
  ///
  /// which in the master's `≥ rhs` row convention (θ + Σ coeff·K ≥ rhs)
  /// is  coeff_j = −g_j,  rhs = V_s − Σ_j g_j·K̂_j.
  ///
  /// This is what fixes the OptGen loop: the phase-0 α-cut alone bounds
  /// only the cost-to-go (stages ≥ 1), so a θ_s bound by it omits the
  /// here-and-now stage-0 dispatch/CAPEX and the master under-values
  /// capacity (LB stuck below the true optimum, no build).  The
  /// full-value cut restores θ_s ≥ Φ_s(K) so LB climbs to the two-stage
  /// optimum and the master builds.
  void add_full_value_cut(std::size_t scene_index,
                          double scene_value,
                          std::span<const ColIndex> k_cols,
                          std::span<const double> subgrad,
                          std::span<const double> k_hat)
  {
    SparseRow row;
    row[m_theta_col_[scene_index]] = 1.0;
    double rhs = scene_value;
    for (std::size_t j = 0; j < k_cols.size(); ++j) {
      const double g = subgrad[j];
      row[k_cols[j]] += -g;
      rhs -= g * k_hat[j];
    }
    (void)m_lp_.add_row(row.greater_equal(rhs));
  }

  /// The candidate `K_g` build columns, in candidate order.
  [[nodiscard]] const std::vector<ColIndex>& k_cols() const noexcept
  {
    return m_k_col_;
  }

  /// Solve the master MIP.  On success returns the objective (Σ_s θ_s,
  /// the loop LB); build counts are read via `builds()`.
  [[nodiscard]] std::expected<double, Error> solve()
  {
    auto r = m_lp_.initial_solve(m_solver_opts_);
    if (!r.has_value()) {
      return std::unexpected(std::move(r.error()));
    }
    if (!m_lp_.is_optimal()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = "investment master: MIP did not solve to optimality",
      });
    }
    const auto sol = m_lp_.get_col_sol();
    m_builds_.assign(m_num_candidates_, 0.0);
    for (std::size_t i = 0; i < m_num_candidates_; ++i) {
      // Round the integer build to scrub solver tolerance noise before it
      // feeds the operational pin.
      m_builds_[i] = std::round(sol[m_n_col_[i]]);
    }
    return m_lp_.get_obj_value();
  }

  [[nodiscard]] const std::vector<double>& builds() const noexcept
  {
    return m_builds_;
  }

private:
  static constexpr double DblMax = gtopt::DblMax;
  /// θ floor: large enough to never bind at a physical optimum, finite so
  /// the cutless master is bounded.
  static constexpr double theta_floor_ = 1.0e12;

  LinearInterface m_lp_;
  std::size_t m_num_candidates_;
  std::size_t m_num_scenes_;
  SolverOptions m_solver_opts_;
  std::vector<ColIndex> m_n_col_;
  std::vector<ColIndex> m_k_col_;
  std::vector<ColIndex> m_theta_col_;
  std::vector<double> m_builds_;
};

/// Build the operational-oracle copy of @p planning.
///
/// Two mutations, both essential to a SOUND OptGen loop:
///
///  1. **Relax expansion integrality** — the candidate `integer_expmod`
///     flags are cleared so the operational SDDP's `expmod` columns are
///     CONTINUOUS.  Integers live ONLY in the master (design §7.2); the
///     operational oracle must be a convex LP so its phase-0 cuts are
///     valid convex supports of Φ(K) (Theorem O1/O2) — NOT the §1
///     unsound MIP-reduced-cost path.  This is exactly why PSR runs an
///     LP operational model under OptGen rather than SDDiP.
///  2. **Pin `expmod = n*`** (only when @p pin) — fixes the operational
///     build to the master proposal so the run prices that capacity
///     exactly (valid UB) and emits a tight cut there.  `n == 0` keeps the
///     original headroom so the state/cut keeps publishing (no stall).
[[nodiscard]] Planning make_operational_copy(
    const Planning& planning,
    const std::vector<ExpansionCandidate>& cands,
    const std::vector<double>& builds,
    bool pin)
{
  Planning copy = planning;
  auto mutate = [&](auto& elem_array, std::string_view class_name)
  {
    for (auto& elem : elem_array) {
      for (const auto& [i, c] : enumerate(cands)) {
        if (c.class_name != class_name || c.uid != elem.uid) {
          continue;
        }
        // Continuous-relax the operational expansion (LP oracle).
        elem.integer_expmod = false;
        if (pin) {
          pin_expmod(elem.expmod, builds[i]);
        }
      }
    }
  };
  mutate(copy.system.generator_array, "Generator");
  mutate(copy.system.demand_array, "Demand");
  return copy;
}

/// Per-scene capacity subgradient g_{s,j} = ∂V_s/∂K_j for every expansion
/// candidate j, read from the LIVE operational LP as the sum, over every
/// (phase, block), of the `capacity` constraint dual (`generation ≤
/// capainst`).
///
/// ## Why NOT the capainst column reduced cost
///
/// The obvious-looking source — the `capainst` column's reduced cost via
/// `get_col_cost()` — is IDENTICALLY 0: `capainst` is a BASIC column,
/// structurally pinned by its own defining equality `capainst = base +
/// expcap·expmod + prev_capainst` (capacity_object_lp.cpp), so its reduced
/// cost is 0 by simplex stationarity and every resulting cut is FLAT
/// (`θ_s ≥ V_s`, no K-dependence) → the master never learns capacity has
/// value and the loop falsely "converges" at build 0.  Neither does any
/// single row dual work: at the value-function kinks (K = demand) the LP
/// basis is degenerate and the individual `capainst`-equality / `expmod`
/// duals read a masked, capacity-independent value.
///
/// ## The correct source: the `capacity` constraint dual
///
/// The marginal value of one more MW of INSTALLED capacity is the shadow
/// price of the dispatch ceiling `generation ≤ capainst` (generator_lp.cpp
/// `CapacityName` row, coeff +1 on capainst, −1 on generation).  Its dual
/// π_cap ≥ 0 is a valid subgradient of the convex value function at the
/// trial point (standard LP sensitivity).  Each per-block dual is already
/// folded by its `cost_factor = prob × discount × duration`, the SAME
/// probability-folded, `scale_objective`-unscaled space as V_s =
/// `get_obj_value()` (= `scene_lower_bounds`), so the plain SUM over all
/// (phase, block) capacity rows is g_{s,j} directly in the master's θ_s
/// units — no extra scaling.  Because the master pins the same build to
/// every stage, every stage's ceiling moves with K_j, so summing across
/// phases folds in the stage-≥1 cost-to-go.  Sign: ∂V_s/∂K_j = −Σ π_cap
/// (relaxing the ceiling REDUCES cost).  This matches the validated
/// backward-pass convention in `build_benders_cut_physical`
/// (`row[K_j] = −g`, `rhs = V_s − Σ g·K̂`).
///
/// One entry per (scene, candidate).  A candidate whose phase-0 `capainst`
/// state is not resolvable in a scene (e.g. the backend was released under
/// low_memory) yields a NaN slot; the caller falls back to the α-cut
/// projection for that scene.
struct SceneCapSubgradients
{
  /// subgrad[scene_index][candidate_index] = g_{s,j}; NaN when unavailable.
  std::vector<std::vector<double>> subgrad;
  /// khat[scene_index][candidate_index] = the pinned capainst value K̂_j.
  std::vector<std::vector<double>> khat;
  /// value[scene_index] = V_s^{(0)}(K̂), the full phase-0 objective.
  std::vector<double> value;
  bool all_resolved {true};
};

[[nodiscard]] SceneCapSubgradients read_scene_cap_subgradients(
    PlanningLP& plp,
    const std::vector<ExpansionCandidate>& cands,
    std::span<const double> scene_values)
{
  const auto& sim = plp.simulation();
  const auto num_scenes = static_cast<std::size_t>(sim.scene_count());
  constexpr auto qnan = std::numeric_limits<double>::quiet_NaN();

  SceneCapSubgradients out;
  out.subgrad.assign(num_scenes, std::vector<double>(cands.size(), qnan));
  out.khat.assign(num_scenes, std::vector<double>(cands.size(), qnan));
  out.value.assign(num_scenes, qnan);

  constexpr std::string_view kCapacityConstraint {"capacity"};

  const auto p0 = first_phase_index();
  const auto nphases = sim.phase_count();
  for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
    const auto s = static_cast<std::size_t>(si);
    if (s < scene_values.size()) {
      out.value[s] = scene_values[s];
    }

    // Guard on a LIVE phase-0 backend — under low_memory the backend may
    // have been released after the run; mark the scene unresolved and let
    // the α-cut fallback price it.
    auto& li0 = plp.system(si, p0).linear_interface();
    if (!li0.has_backend() || li0.is_backend_released()) {
      out.all_resolved = false;
      continue;
    }
    const auto& svars = sim.state_variables(si, p0);
    for (const auto& [j, c] : enumerate(cands)) {
      // K̂ — the pinned trial capacity, read from the phase-0 capainst
      // state variable's primal (physical).  The master's single K_g
      // aggregates the candidate's capacity across the horizon.
      double k_hat = qnan;
      for (const auto& [key, svar] : svars) {
        if (key.col_name == CapacityObjectBase::CapainstName
            && key.class_name == c.class_name && key.uid == c.uid)
        {
          k_hat = svar.col_sol_physical();
          break;
        }
      }

      // ∂V_s/∂K_j — the marginal value of one more MW of INSTALLED
      // capacity across the whole scene horizon.  It is NOT the basic
      // `capainst` column's reduced cost (structurally 0), nor a single
      // phase's row dual (degenerate at the value-function kinks).  It is
      // the sum over EVERY (phase, block) of the `capacity` constraint's
      // dual `generation ≤ capainst` — the shadow price of the capacity
      // ceiling in dispatch.  Each per-block dual is already folded by its
      // `cost_factor = prob × discount × duration` (the same space as V_s =
      // get_obj_value()), so the plain sum is the folded marginal in the
      // master's θ_s units — no extra scaling.  Summing across phases folds
      // in the stage-≥1 cost-to-go directly (the master pins the same build
      // to every stage, so every stage's capacity ceiling moves with K_j).
      double dV_dK = 0.0;
      for (const auto pi : iota_range<PhaseIndex>(0, nphases)) {
        auto& lip = plp.system(si, pi).linear_interface();
        if (!lip.has_backend() || lip.is_backend_released()) {
          continue;
        }
        const auto duals = lip.get_row_dual();
        const auto nrows = lip.get_numrows();
        for (Index ri = 0; ri < nrows; ++ri) {
          const auto* lbl = lip.row_label_at(RowIndex {ri});
          if (lbl != nullptr && lbl->constraint_name == kCapacityConstraint
              && lbl->class_name == c.class_name && lbl->variable_uid == c.uid)
          {
            const auto uri = static_cast<std::size_t>(ri);
            if (uri < duals.size() && std::isfinite(duals[RowIndex {ri}])) {
              // The capacity row is `capainst − generation ≥ 0`; its dual
              // ≥ 0 is the value of RELAXING the ceiling, i.e. +∂(−cost)/∂K.
              // ∂V_s/∂K = −(that value), so we accumulate the negative.
              dV_dK -= duals[RowIndex {ri}];
            }
          }
        }
      }

      if (!std::isfinite(k_hat)) {
        out.all_resolved = false;
        continue;
      }
      out.subgrad[s][j] = dV_dK;
      out.khat[s][j] = k_hat;
      SPDLOG_DEBUG(
          "investment master subgradient: scene {} cand '{}' V={:.6g} "
          "dV/dK={:.6g} K̂={:.6g}",
          s,
          c.name,
          (s < scene_values.size() ? scene_values[s] : qnan),
          dV_dK,
          k_hat);
    }
  }
  return out;
}

}  // namespace

auto solve_investment_master(const Planning& planning,
                             const InvestmentMasterOptions& opts)
    -> std::expected<InvestmentMasterResult, Error>
{
  const auto candidates = find_expansion_candidates(planning);
  if (candidates.empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message =
            "investment master: no expansion candidates (no element carries "
            "positive expcap · expmod)",
    });
  }

  // Base capacity per candidate — the operational LP's `capainst` floor
  // (`stage_capacity`), read as a scalar so the master's `K_g` accounting
  // row `K_g − expcap · n_g = base` matches the operational recursion.
  std::vector<double> base_capacity;
  base_capacity.reserve(candidates.size());
  auto base_of = [&](std::string_view class_name, Uid uid) -> double
  {
    for (const auto& g : planning.system.generator_array) {
      if (class_name == "Generator" && g.uid == uid) {
        return field_scalar(g.capacity);
      }
    }
    for (const auto& d : planning.system.demand_array) {
      if (class_name == "Demand" && d.uid == uid) {
        return field_scalar(d.capacity);
      }
    }
    return 0.0;
  };
  for (const auto& c : candidates) {
    base_capacity.push_back(base_of(c.class_name, c.uid));
  }

  // Scene count from the operational simulation.  A scene with no
  // explicit scene_array is the monolithic single synthetic scene → 1.
  const std::size_t num_scenes = planning.simulation.scene_array.empty()
      ? 1
      : planning.simulation.scene_array.size();

  // The solver backend name comes from the operational planning's
  // LP-matrix options (the single programmatic source of truth); the
  // master MIP and every operational SDDP run share it.  Empty ⇒ the
  // registry auto-detects (a MIP-capable default is required for the
  // integer master).
  const std::string solver_name =
      planning.options.lp_matrix_options.solver_name;

  MasterMIP master(
      candidates, num_scenes, base_capacity, solver_name, opts.solver_options);

  InvestmentMasterResult result;
  result.candidates = candidates;
  result.lower_bound = -std::numeric_limits<double>::infinity();
  result.upper_bound = std::numeric_limits<double>::infinity();

  for (int iter = 0; iter < opts.max_iterations; ++iter) {
    // ── 1. Master → proposed builds + LB ──
    auto lb = master.solve();
    if (!lb.has_value()) {
      return std::unexpected(std::move(lb.error()));
    }
    const double lower_bound = *lb;
    const auto builds = master.builds();  // copy — reused below

    // ── 2. Pin capacities into the operational SDDP and run it ──
    // Iteration 0 has no cuts yet, so the master's proposal is
    // meaningless; run the operational oracle at FULL expmod headroom to
    // seed cuts that span the capacity range.  From iteration 1 on, pin
    // `expmod = n*` so each run prices the proposal exactly (design §7.1).
    // Either way the operational copy relaxes expansion integrality (LP
    // oracle — design §7.2).
    Planning op_copy =
        make_operational_copy(planning, candidates, builds, /*pin=*/iter > 0);
    PlanningLP plp(std::move(op_copy));

    SDDPOptions sddp_opts = opts.sddp_options;
    sddp_opts.enable_api = false;
    SDDPMethod sddp(plp, sddp_opts);
    auto sddp_res = sddp.solve(opts.solver_options);
    if (!sddp_res.has_value()) {
      return std::unexpected(std::move(sddp_res.error()));
    }
    if (sddp_res->empty()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = "investment master: operational SDDP produced no "
                     "iterations",
      });
    }
    // The converged SDDP lower bound is the phase-0 expected cost of the
    // policy at this capacity.  We read the LB (phase-0 objective), not
    // the statistical UB: the phase-0 objective is the exact expected cost
    // at fixed capacity, whereas the statistical UB carries sampling
    // noise.  This is a VALID loop UB only when the run was PINNED to the
    // master's integer proposal (iter ≥ 1); the iteration-0 seeding run
    // uses full continuous-relaxed headroom, so its cost is a relaxation
    // lower bound, not a valid integer UB.
    const double operational_cost = sddp_res->back().lower_bound;
    const bool operational_cost_is_ub = (iter > 0);

    // ── 3. Add FULL-value optimality cuts, one per scene ──
    //
    // The correct master epigraph support is θ_s ≥ Φ_s(K) = the WHOLE
    // per-scene phase-0 objective (stage-0 dispatch + CAPEX + cost-to-go)
    // as a function of the build capacity K — NOT merely the phase-0 α
    // tail that `extract_capacity_cuts` isolates.  Binding θ_s to the α
    // tail alone omits the here-and-now stage-0 cost, so the master
    // under-values capacity: LB stalls below the true two-stage optimum
    // and the master builds nothing (the pre-fix defect — LB 500, gap
    // 700, build 0 on the acceptance fixture).  We build the full-value
    // cut from the live operational LP: value V_s = scene_lower_bounds[s]
    // (= get_obj_value()) and slope g_{s,j} = ∂V_s/∂K_j (the sum over all
    // (phase, block) `capacity` constraint duals — NOT the basic `capainst`
    // column reduced cost, which is structurally 0 and would flatten every
    // cut; see `SceneCapSubgradients`), both in the same folded LP space,
    // using the tested backward-pass cut convention
    // (row[K] = −g, rhs = V − Σ g·K̂).
    const auto& sim = plp.simulation();
    const auto stored = sddp.stored_cuts();

    std::size_t cuts_added = 0;
    const auto scene_lbs = sddp_res->back().scene_lower_bounds;
    const auto subg = read_scene_cap_subgradients(plp, candidates, scene_lbs);

    // Master K columns, in candidate order (all always resolvable — one
    // K_g per candidate, built in the master ctor).
    const auto& k_cols = master.k_cols();

    // Track scenes handled by the full-value cut so the α-cut fallback
    // below only fires for scenes the reduced-cost read could not resolve.
    std::vector<uint8_t> scene_full_cut(num_scenes, 0U);
    for (std::size_t s = 0; s < num_scenes; ++s) {
      if (s >= subg.value.size() || !std::isfinite(subg.value[s])) {
        continue;
      }
      // Assemble the per-candidate slope / K̂ vectors, falling back to the
      // analytic K̂ = base + expcap·build when the primal read is missing.
      std::vector<double> g(candidates.size(), 0.0);
      std::vector<double> khat(candidates.size(), 0.0);
      bool ok = true;
      for (std::size_t j = 0; j < candidates.size(); ++j) {
        const double gj = subg.subgrad[s][j];
        if (!std::isfinite(gj)) {
          ok = false;
          break;
        }
        g[j] = gj;
        const double kh = subg.khat[s][j];
        khat[j] = std::isfinite(kh)
            ? kh
            : base_capacity[j] + (candidates[j].expcap * builds[j]);
      }
      if (!ok) {
        continue;  // leave this scene to the α-cut fallback
      }
      master.add_full_value_cut(s, subg.value[s], k_cols, g, khat);
      scene_full_cut[s] = 1U;
      ++cuts_added;
    }

    // α-cut fallback for any scene the reduced-cost read could not
    // resolve (defensive: e.g. a released backend under low_memory).
    if (!subg.all_resolved) {
      const auto cap_cuts = extract_capacity_cuts(sim, stored);
      for (const auto& cut : cap_cuts) {
        std::optional<std::size_t> scene_idx;
        for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
          if (sim.uid_of(si) == cut.scene_uid) {
            scene_idx = static_cast<std::size_t>(si);
            break;
          }
        }
        if (!scene_idx.has_value() || *scene_idx >= num_scenes
            || scene_full_cut[*scene_idx] != 0U)
        {
          continue;
        }
        if (master.add_capacity_cut(candidates, *scene_idx, cut)) {
          ++cuts_added;
        }
      }
    }

    // ── 4. Bounds bookkeeping / convergence ──
    if (operational_cost_is_ub && operational_cost < result.upper_bound) {
      result.upper_bound = operational_cost;
      result.best_builds = builds;
    }
    result.lower_bound = lower_bound;
    const double gap = result.upper_bound - lower_bound;

    result.iterations.push_back(InvestmentMasterIteration {
        .iteration = iter,
        .lower_bound = lower_bound,
        .upper_bound = result.upper_bound,
        .gap = gap,
        .builds = builds,
        .cuts_added = cuts_added,
    });

    SPDLOG_INFO(
        "investment master iter {}: LB {:.6g}  UB {:.6g}  gap {:.3g}  "
        "cuts +{}",
        iter,
        lower_bound,
        result.upper_bound,
        gap,
        cuts_added);

    // Convergence certificate: the gap has closed AND the master's current
    // integer proposal is the one that ATTAINS the incumbent UB.  Requiring
    // `builds == best_builds` rules out a false positive where the pinned
    // operational LP built strictly less than the ceiling n* (a fractional
    // sub-proposal whose cost is below the cost at n*): only when the
    // proposal is operationally optimal at the current cut set do LB and UB
    // certify the SAME integer point.
    if (gap <= opts.tol && builds == result.best_builds) {
      result.converged = true;
      break;
    }
    // No new cut means the master cannot improve — the operational oracle
    // is exhausted at this proposal; stop to avoid a stall.
    if (cuts_added == 0 && iter > 0) {
      SPDLOG_WARN(
          "investment master iter {}: no new cuts — stopping (gap {:.3g})",
          iter,
          gap);
      break;
    }
  }

  if (result.best_builds.empty()) {
    result.best_builds.assign(candidates.size(), 0.0);
  }
  return result;
}

}  // namespace gtopt
