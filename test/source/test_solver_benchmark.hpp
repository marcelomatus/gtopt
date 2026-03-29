/**
 * @file      test_solver_benchmark.hpp
 * @brief     Benchmark solver backends with different algorithms and threads
 * @date      2026-03-27
 * @copyright BSD-3-Clause
 *
 * Builds a realistic power-system dispatch LP (multiple buses, generators,
 * lines, and demand constraints) and solves it with every available solver
 * using dual and barrier algorithms with threads = 0, 2, 4.
 */

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── LP builder ─────────────────────────────────────────────────────────────

/// Build a realistic power-system dispatch LP.
///
/// Topology: N_BUS buses in a ring, each with N_GEN generators.
/// Each bus has a demand constraint (supply - flow_in + flow_out >= demand).
/// Generators have capacity bounds and linear cost.
/// Lines have capacity limits in both directions.
/// An "alpha" future-cost variable is added (typical of SDDP sub-problems).
///
/// Returns the FlatLinearProblem ready to load into a LinearInterface.
auto build_dispatch_lp(int n_bus = 20, int n_gen_per_bus = 5, int n_blocks = 4)
    -> FlatLinearProblem
{
  LinearProblem lp("benchmark_dispatch");

  // ── Generators: gen[bus][gen][block] ──
  // cost = 10 + 3*bus + 7*gen (spread of marginal costs)
  struct GenCol
  {
    int bus;
    int gen;
    int block;
    ColIndex col;
  };
  std::vector<GenCol> gen_cols;

  for (int b = 0; b < n_bus; ++b) {
    for (int g = 0; g < n_gen_per_bus; ++g) {
      const double cost = 10.0 + 3.0 * b + 7.0 * g;
      const double cap = 100.0 + 20.0 * g;
      for (int blk = 0; blk < n_blocks; ++blk) {
        auto name = std::format("gen_b{}_g{}_t{}", b, g, blk);
        auto ci = lp.add_col({
            .name = std::move(name),
            .lowb = 0.0,
            .uppb = cap,
            .cost = cost,
        });
        gen_cols.push_back({
            .bus = b,
            .gen = g,
            .block = blk,
            .col = ci,
        });
      }
    }
  }

  // ── Lines: flow[line][block] (ring topology: bus i -> bus (i+1) % n_bus) ──
  struct FlowCol
  {
    int from;
    int to;
    int block;
    ColIndex col;
  };
  std::vector<FlowCol> flow_cols;

  for (int i = 0; i < n_bus; ++i) {
    const int j = (i + 1) % n_bus;
    const double line_cap = 200.0 + 50.0 * (i % 3);
    for (int blk = 0; blk < n_blocks; ++blk) {
      auto name = std::format("flow_{}_{}_{}", i, j, blk);
      auto ci = lp.add_col({
          .name = std::move(name),
          .lowb = -line_cap,
          .uppb = line_cap,
          .cost = 0.5,
      });
      flow_cols.push_back({
          .from = i,
          .to = j,
          .block = blk,
          .col = ci,
      });
    }
  }

  // ── Demand fail (unserved energy) per bus per block ──
  struct FailCol
  {
    int bus;
    int block;
    ColIndex col;
  };
  std::vector<FailCol> fail_cols;

  for (int b = 0; b < n_bus; ++b) {
    for (int blk = 0; blk < n_blocks; ++blk) {
      auto name = std::format("dfail_b{}_t{}", b, blk);
      auto ci = lp.add_col({
          .name = std::move(name),
          .lowb = 0.0,
          .uppb = 1e6,
          .cost = 1000.0,
      });
      fail_cols.push_back({
          .bus = b,
          .block = blk,
          .col = ci,
      });
    }
  }

  // ── Alpha (future cost variable, typical in SDDP) ──
  auto alpha_col = lp.add_col({
      .name = "alpha",
      .lowb = 0.0,
      .uppb = 1e12,
      .cost = 1.0,
  });

  // ── Demand balance: sum(gen) - flow_out + flow_in + dfail >= demand ──
  for (int b = 0; b < n_bus; ++b) {
    for (int blk = 0; blk < n_blocks; ++blk) {
      const double demand = 150.0 + 30.0 * b + 10.0 * blk;
      auto rname = std::format("demand_b{}_t{}", b, blk);
      auto ri = lp.add_row({
          .name = std::move(rname),
          .lowb = demand,
          .uppb = SparseRow::DblMax,
      });

      // Generators at this bus
      for (const auto& gc : gen_cols) {
        if (gc.bus == b && gc.block == blk) {
          lp.set_coeff(ri, gc.col, 1.0);
        }
      }

      // Flows: outgoing (-1), incoming (+1)
      for (const auto& fc : flow_cols) {
        if (fc.block == blk) {
          if (fc.from == b) {
            lp.set_coeff(ri, fc.col, -1.0);
          }
          if (fc.to == b) {
            lp.set_coeff(ri, fc.col, 1.0);
          }
        }
      }

      // Demand fail
      for (const auto& dc : fail_cols) {
        if (dc.bus == b && dc.block == blk) {
          lp.set_coeff(ri, dc.col, 1.0);
        }
      }
    }
  }

  // ── Ramping constraints between consecutive blocks ──
  for (int b = 0; b < n_bus; ++b) {
    for (int g = 0; g < n_gen_per_bus; ++g) {
      const double ramp = 50.0 + 10.0 * g;
      for (int blk = 1; blk < n_blocks; ++blk) {
        auto rname = std::format("ramp_b{}_g{}_t{}", b, g, blk);
        auto ri = lp.add_row({
            .name = std::move(rname),
            .lowb = -ramp,
            .uppb = ramp,
        });

        // Find gen cols for (b, g, blk) and (b, g, blk-1)
        for (const auto& gc : gen_cols) {
          if (gc.bus == b && gc.gen == g) {
            if (gc.block == blk) {
              lp.set_coeff(ri, gc.col, 1.0);
            }
            if (gc.block == blk - 1) {
              lp.set_coeff(ri, gc.col, -1.0);
            }
          }
        }
      }
    }
  }

  // ── A few Benders optimality cuts on alpha ──
  for (int k = 0; k < 10; ++k) {
    auto rname = std::format("cut_{}", k);
    const double rhs = 5000.0 + 500.0 * k;
    auto ri = lp.add_row({
        .name = std::move(rname),
        .lowb = rhs,
        .uppb = SparseRow::DblMax,
    });

    // alpha >= rhs - sum_i(pi_i * gen_i_0)  →  alpha + pi*gen >= rhs
    lp.set_coeff(ri, alpha_col, 1.0);
    for (int b = 0; b < n_bus; ++b) {
      const double pi = 0.5 + 0.1 * ((b + k) % 5);
      for (const auto& gc : gen_cols) {
        if (gc.bus == b && gc.block == 0 && gc.gen == 0) {
          lp.set_coeff(ri, gc.col, pi);
        }
      }
    }
  }

  LpBuildOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  return lp.lp_build(opts);
}

// ─── Benchmark runner ───────────────────────────────────────────────────────

struct BenchmarkResult
{
  std::string solver;
  std::string algorithm;
  int threads;
  double solve_time_ms;
  double obj_value;
  bool optimal;
};

auto run_benchmark(const std::string& solver_name,
                   const FlatLinearProblem& flat_lp,
                   LPAlgo algo,
                   int threads) -> BenchmarkResult
{
  LinearInterface li(solver_name, flat_lp);

  SolverOptions opts;
  opts.algorithm = algo;
  opts.threads = threads;
  opts.log_level = 0;

  const auto t0 = std::chrono::steady_clock::now();
  auto result = li.initial_solve(opts);
  const auto t1 = std::chrono::steady_clock::now();

  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  return BenchmarkResult {
      .solver = solver_name,
      .algorithm = std::string(enum_name(algo)),
      .threads = threads,
      .solve_time_ms = ms,
      .obj_value = li.is_optimal() ? li.get_obj_value() : 0.0,
      .optimal = li.is_optimal(),
  };
}

void print_benchmark_table(const std::vector<BenchmarkResult>& results)
{
  MESSAGE("");
  MESSAGE(std::format("{:<10} {:<10} {:>7} {:>12} {:>14} {:>7}",
                      "Solver",
                      "Algorithm",
                      "Threads",
                      "Time(ms)",
                      "Objective",
                      "Status"));
  MESSAGE(std::string(62, '-'));
  for (const auto& r : results) {
    MESSAGE(std::format("{:<10} {:<10} {:>7} {:>12.2f} {:>14.2f} {:>7}",
                        r.solver,
                        r.algorithm,
                        r.threads,
                        r.solve_time_ms,
                        r.obj_value,
                        r.optimal ? "OK" : "FAIL"));
  }
  MESSAGE("");
}

void run_solver_benchmark(const FlatLinearProblem& flat_lp,
                          const std::string& label)
{
  const auto& reg = SolverRegistry::instance();
  const auto available = reg.available_solvers();

  if (available.empty()) {
    MESSAGE("No solver plugins available — skipping " << label);
    return;
  }

  MESSAGE(label << ": " << flat_lp.ncols << " cols, " << flat_lp.nrows
                << " rows, " << flat_lp.matval.size() << " non-zeros");

  constexpr auto algos = std::to_array<LPAlgo>({
      LPAlgo::dual,
      LPAlgo::barrier,
  });
  constexpr auto thread_counts = std::to_array<int>({0, 2, 4});

  std::vector<BenchmarkResult> results;
  double reference_obj = 0.0;
  bool have_reference = false;

  for (const auto& solver : available) {
    for (auto algo : algos) {
      for (int thr : thread_counts) {
        CAPTURE(solver);
        CAPTURE(enum_name(algo));
        CAPTURE(thr);

        auto br = run_benchmark(solver, flat_lp, algo, thr);

        CHECK(br.optimal);

        if (br.optimal && !have_reference) {
          reference_obj = br.obj_value;
          have_reference = true;
        }

        if (br.optimal && have_reference) {
          CHECK(br.obj_value == doctest::Approx(reference_obj).epsilon(1e-4));
        }

        results.push_back(std::move(br));
      }
    }
  }

  print_benchmark_table(results);
}

}  // namespace

// ─── Small LP benchmark ────────────────────────────────────────────────────

TEST_CASE("Solver benchmark: small dispatch LP")  // NOLINT
{
  const auto flat_lp = build_dispatch_lp(/*n_bus=*/20,
                                         /*n_gen_per_bus=*/5,
                                         /*n_blocks=*/4);
  run_solver_benchmark(flat_lp, "Small LP");
}

// ─── Large LP benchmark ────────────────────────────────────────────────────

TEST_CASE("Solver benchmark: large dispatch LP")  // NOLINT
{
  const auto flat_lp = build_dispatch_lp(/*n_bus=*/200,
                                         /*n_gen_per_bus=*/20,
                                         /*n_blocks=*/12);
  run_solver_benchmark(flat_lp, "Large LP");
}

// ─── Large-scale resolve benchmark ──────────────────────────────────────────

TEST_CASE("Solver benchmark: resolve with basis reuse")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto available = reg.available_solvers();

  if (available.empty()) {
    MESSAGE("No solver plugins available — skipping resolve benchmark");
    return;
  }

  const auto flat_lp = build_dispatch_lp(/*n_bus=*/200,
                                         /*n_gen_per_bus=*/20,
                                         /*n_blocks=*/12);

  MESSAGE("Resolve LP: " << flat_lp.ncols << " cols, " << flat_lp.nrows
                         << " rows");

  struct ResolveResult
  {
    std::string solver;
    double initial_ms;
    double resolve_ms;
    double speedup;
    bool both_optimal;
  };

  std::vector<ResolveResult> results;

  for (const auto& solver : available) {
    CAPTURE(solver);

    LinearInterface li(solver, flat_lp);
    SolverOptions init_opts;
    init_opts.algorithm = LPAlgo::dual;
    init_opts.threads = 0;

    const auto t0 = std::chrono::steady_clock::now();
    auto r1 = li.initial_solve(init_opts);
    const auto t1 = std::chrono::steady_clock::now();
    const double initial_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!r1.has_value() || !li.is_optimal()) {
      MESSAGE("  [SKIP] " << solver << " initial_solve failed");
      continue;
    }

    // Perturb bounds (simulates adding a Benders cut)
    if (li.get_numrows() > 10) {
      for (int i = 0; i < 5; ++i) {
        li.set_row_upp(RowIndex {i}, 1e6);
      }
    }

    SolverOptions resolve_opts;
    resolve_opts.reuse_basis = true;
    resolve_opts.threads = 0;

    const auto t2 = std::chrono::steady_clock::now();
    auto r2 = li.resolve(resolve_opts);
    const auto t3 = std::chrono::steady_clock::now();
    const double resolve_ms =
        std::chrono::duration<double, std::milli>(t3 - t2).count();

    CHECK(r2.has_value());
    CHECK(li.is_optimal());

    const double speedup = resolve_ms > 0.0 ? initial_ms / resolve_ms : 0.0;

    results.push_back(ResolveResult {
        .solver = solver,
        .initial_ms = initial_ms,
        .resolve_ms = resolve_ms,
        .speedup = speedup,
        .both_optimal = li.is_optimal(),
    });
  }

  MESSAGE("");
  MESSAGE(std::format("{:<10} {:>12} {:>12} {:>10} {:>7}",
                      "Solver",
                      "Initial(ms)",
                      "Resolve(ms)",
                      "Speedup",
                      "Status"));
  MESSAGE(std::string(53, '-'));
  for (const auto& r : results) {
    MESSAGE(std::format("{:<10} {:>12.2f} {:>12.2f} {:>9.1f}x {:>7}",
                        r.solver,
                        r.initial_ms,
                        r.resolve_ms,
                        r.speedup,
                        r.both_optimal ? "OK" : "FAIL"));
  }
  MESSAGE("");
}
