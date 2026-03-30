# Prompt: Critical Analysis of gtopt Implementation

## Task

Perform a comprehensive critical analysis of the gtopt C++ codebase from two perspectives:

1. **Electric grid modeling** (using pandapower/MATPOWER as reference implementations)
2. **SDDP & Benders decomposition** (using academic literature and commercial tools as reference)

The goal is to find implementation issues, deviations from standard formulations, and areas for improvement. Produce an extensive report comparing gtopt against PLP, pandapower, and the literature.

## Research Phase (use parallel agents for all of these)

### Agent 1: SDDP/Benders Literature (web research)
Research SDDP and Benders decomposition as applied to power system expansion planning:
- Classical SDDP algorithm (Pereira & Pinto 1991): forward/backward passes, cut construction, convergence
- Benders decomposition: master/subproblem structure, optimality and feasibility cuts
- Multi-stage stochastic programming for GTEP: scenarios, stages, investment decisions
- Common pitfalls: numerical issues, cut management, regularization, convergence problems
- Key references: SDDP.jl documentation, PSR SDDP papers, SIAM Review survey papers

### Agent 2: PLP Fortran Code (codebase exploration)
Explore ~/git/plp_storage/CEN65/src/ for:
- SDDP main loop, forward/backward passes (plp-progdin.f, plp-faseprim.f, plp-fasedual.f)
- Cut generation and storage (plp-agrespd.f, addplane.f)
- LP formulation and matrix assembly (defprbpd.f, genmatpd.f, pdmatvar.f)
- Network modeling: buses, lines, Kirchhoff (genpdlin.f, genpdang.f)
- Hydro: reservoirs, turbines, spillage, filtration (genpdcen.f, genpdqemb.f, genpdver.f, genpdfil.f)
- Battery/ESS models (genpdbaterias.f, genpdess.f)
- Scenario/stage handling, scaling (ScaleObj, ScaleVol, ScaleAng, ScalePhi)
- Convergence criteria (plp-pdconvrg.f)

### Agent 3: pandapower DC-OPF (web research)
Research pandapower's DC-OPF implementation:
- Formulation: B-matrix construction (makeBdc.py), KCL/KVL constraints
- Bus, line, transformer modeling; per-unit system; reference bus handling
- Compare with MATPOWER's implementation
- Key source files on GitHub: pandapower/pypower/makeBdc.py, dcopf_solver.py, opf_setup.py
- What additional considerations does GTEP need beyond single-period DC-OPF?

### Agent 4: gtopt SDDP/LP Deep-dive (codebase exploration)
Explore /home/marce/git/gtopt for SDDP and LP implementation:
- SDDP: sddp_method.hpp/.cpp, sddp_forward_pass.cpp, sddp_iteration.cpp, sddp_aperture_pass.cpp, sddp_feasibility.cpp
- Benders cuts: benders_cut.hpp/.cpp -- cut construction, elastic filter, feasibility cuts
- LP assembly: linear_problem.hpp, system_lp.cpp -- how variables/constraints are built
- State variables: state_variable.hpp, storage_lp.hpp -- cross-phase coupling
- Cascade method: cascade_method.hpp/.cpp
- Solver interface: solver_backend.hpp, solver_registry.hpp, linear_interface.hpp
- Focus on: how duals/reduced costs are used for cuts, alpha variable bounds, convergence criteria, cut sharing modes, state propagation modes

### Agent 5: gtopt Network/Grid Modeling (codebase exploration)
Explore /home/marce/git/gtopt for electric grid modeling:
- Bus: bus.hpp, bus_lp.hpp/.cpp -- nodal balance, theta variables, reference bus
- Line: line.hpp, line_lp.hpp/.cpp -- flow variables, Kirchhoff constraints, losses, expansion
- KCL: how nodal power balance is assembled across elements
- KVL: per-line Kirchhoff constraint formulation, susceptance computation, scaling
- Single-bus vs multi-bus mode
- Generator-bus connection, demand-bus connection
- Demand curtailment mechanism
- Line expansion (capacity constraints, continuous relaxation)

### Agent 6: gtopt Generator/Hydro/Battery (codebase exploration)
Explore /home/marce/git/gtopt for component modeling:
- Generators: generator.hpp, generator_lp.cpp -- costs, bounds, loss factor
- Hydro: junction.hpp, waterway.hpp, reservoir.hpp, turbine.hpp and their LP counterparts
- Reservoir features: seepage, production factor, discharge limits
- Battery: battery.hpp, battery_lp.cpp -- charge/discharge, efficiency, daily_cycle, use_state_variable
- StorageLP template: storage_lp.hpp -- energy balance, state coupling
- Capacity expansion: capacity_object_lp.hpp/.cpp -- investment variables across stages
- Renewable profiles: generator_profile.hpp, profile_object_lp.hpp
- Demand: demand.hpp, demand_lp.cpp -- load, failure, minimum energy

### Agent 7: PLEXOS/PCR/SPPD Commercial Tools (web research)
Research commercial power system planning tools:
- PLEXOS: GTEP modeling, decomposition methods, DC-OPF, network constraints
- PSR SDDP: algorithm implementation, cut management, integer variable handling
- PLP: role in Chilean system, relationship to SDDP
- SPPD/PCR: Chilean short-term dispatch, unit commitment, DC-OPF
- Industry practices: integer expansion within SDDP (LP relaxation, nested Benders, SDDiP, fix-and-relax)
- Cut sharing strategies across scenarios

### Agent 8: gtopt Objective/Scaling/Numerics (codebase exploration)
Explore /home/marce/git/gtopt for objective function and numerics:
- Objective assembly: cost_helper.hpp -- block_ecost, stage_ecost, discount factors, probability weighting
- CAPEX: capacity_object_lp.cpp -- how investment costs enter objective
- Scaling: scale_objective, scale_theta, energy_scale, VariableScaleMap
- LP coefficient monitoring: lp_coeff_ratio_threshold, stats_max_abs/min_abs
- Solution extraction: primal, dual, reduced costs; unscaling for output
- Monolithic vs SDDP vs cascade method selection
- Feasibility handling: demand_fail_cost, reserve_fail_cost, elastic filter
- Solver options: tolerances, algorithm selection, warm-start

### Agent 9: gtopt Data Model (codebase exploration)
Explore /home/marce/git/gtopt for input/data model:
- Planning struct: planning.hpp, system.hpp, simulation.hpp
- Time hierarchy: Block, Stage, Scenario, Phase, Scene, Aperture
- Element types: all 17 element arrays in System
- JSON parsing: json_*.hpp files, daw_json_link contracts
- FieldSched: scalar/array/file-reference mechanism for time-varying parameters
- Parquet I/O: reading Arrow tables, output via OutputContext
- Options: PlanningOptions, SolverOptions, SddpOptions, CascadeOptions

### Agent 10: PLP Network/Hydro Detail (codebase exploration)
Explore ~/git/plp_storage/CEN65/src/ focusing on comparison with gtopt:
- Network: genpdlin.f (lines, losses, HVDC), genpdang.f (angles, Kirchhoff)
- Hydro: genpdqemb.f (discharge), genpdver.f (spillage), genpdreb.f (overflow), genpdfil.f (filtration)
- Battery: genpdbaterias.f (old model), genpdess.f (new ESS model)
- SDDP cuts: addplane.f, plp-agrespd.f -- cut storage format, dual extraction
- LP assembly: genmatpd.f -- dimension computation, column/row ordering
- Demand: leedem.f, sumadem.f -- how demand enters LP as RHS
- Compare architectural differences with gtopt

### Agent 11: DC-OPF Best Practices (web research)
Research DC-OPF implementation best practices:
- B-theta vs PTDF formulation: trade-offs, when to use which
- Reference bus handling, island detection
- Susceptance calculation, per-unit vs physical units
- Transmission expansion: Big-M, disjunctive constraints, continuous relaxation
- Loss approximation: piecewise-linear, iterative methods
- Numerical conditioning: susceptance scaling, solver tolerances
- MATPOWER/pandapower as reference implementations

## Analysis Phase

After all research completes, synthesize findings into a report covering:

### Structure of the Report

1. **Executive Summary** -- Key findings, overall assessment
2. **DC-OPF / Electric Grid Modeling** -- Formulation choice, split flow variables, reference bus handling, susceptance/units, loss models, phase shifters, capacity expansion
3. **SDDP & Benders Implementation** -- Algorithm fidelity, cut construction (reduced costs vs duals), state propagation modes, elastic filter, cut sharing, convergence criteria
4. **Component Modeling Comparison** -- Thermal generators, hydro cascade, battery/ESS, demand; compare gtopt vs PLP vs pandapower vs PLEXOS
5. **Numerical Conditioning** -- Scaling strategies, coefficient monitoring, comparison with PLP
6. **Features Unique to gtopt** -- Cascade method, plugin solvers, user constraints, etc.
7. **Potential Implementation Issues** -- Numbered list with severity, area, and recommendation
8. **Comparison Summary Table** -- All tools side by side
9. **Conclusions** -- Overall assessment, most impactful improvement opportunities

### Key Questions to Answer

- Is the DC-OPF formulation correct and consistent with pandapower/MATPOWER?
- Does the SDDP algorithm faithfully implement Pereira-Pinto (1991)?
- Are Benders cuts constructed correctly (reduced costs vs duals)?
- Are there numerical issues (scaling, conditioning, tolerance)?
- What PLP features are missing in gtopt?
- What are the most impactful potential bugs or design issues?
- How does gtopt compare to commercial tools (PLEXOS, PSR SDDP)?

---

## Prompt Improvements (Proposed for Future Runs)

### Issues Found in This Run

1. **Agent 2 (PLP Fortran) scope too broad**: Covered SDDP + network + hydro +
   battery + LP assembly + convergence in a single agent. This created a very
   long-running agent (~160s). Better to split into:
   - Agent 2a: PLP SDDP loop & cut generation (plp-progdin.f, plp-agrespd.f,
     addplane.f, plp-pdconvrg.f)
   - Agent 2b: PLP LP assembly & network (genmatpd.f, genpdlin.f, genpdang.f,
     defprbpd.f, pdmatvar.f)
   - Agent 2c: PLP hydro & components (genpdcen.f, genpdqemb.f, genpdver.f,
     genpdbaterias.f, genpdess.f)

2. **Agent 10 (PLP network/hydro detail) overlaps with Agent 2**: Both explored
   the same PLP source files. Agent 10 should be merged into Agent 2 or
   refocused on a comparison-only role.

3. **No agent dedicated to testing/validation**: The analysis would benefit from
   an agent that runs gtopt's test suite and validates findings against actual
   test cases (e.g., IEEE 9-bus with known LMPs).

4. **Missing verification agent**: A final agent should verify top findings
   from other agents by reading the actual source code and confirming or
   refuting claims. Several findings in the first version were corrected in v2
   (e.g., `scale_alpha` exists but was initially reported as missing).

### Proposed Improved Agent Structure (13 agents)

```
Research Phase:
  Agent 1:  SDDP/Benders literature (web) — unchanged
  Agent 2:  pandapower/MATPOWER DC-OPF (web) — unchanged
  Agent 3:  DC-OPF best practices (web) — unchanged
  Agent 4:  Commercial tools (PLEXOS/PSR/PLP/SPPD) (web) — unchanged

Codebase Phase:
  Agent 5:  gtopt SDDP deep-dive (explore) — unchanged
  Agent 6:  gtopt network/grid modeling (explore) — unchanged
  Agent 7:  gtopt generator/hydro/battery (explore) — unchanged
  Agent 8:  gtopt objective/scaling/numerics (explore) — unchanged
  Agent 9:  gtopt data model (explore) — unchanged
  Agent 10: PLP SDDP & LP assembly (explore) — merged from Agents 2+10
  Agent 11: PLP hydro & components (explore) — split from Agent 2

Verification Phase:
  Agent 12: Cross-verify top findings — reads specific source lines to
            confirm/refute claims from other agents
  Agent 13: Test validation — runs test suite, checks IEEE benchmark
            results against known values
```

### Improved Analysis Phase Instructions

Add these to the synthesis instructions:

1. **Track finding status**: Mark each finding as `Confirmed`, `Likely`,
   `Unverified`, or `Refuted` based on direct source evidence.

2. **Previous report delta**: When a previous report exists, explicitly note
   what changed (new findings, corrected findings, resolved findings).

3. **Severity criteria**: Define severity levels:
   - **Critical**: Produces incorrect results silently
   - **High**: Produces suboptimal results or fails for common inputs
   - **Medium**: Impacts specific use cases or degrades performance
   - **Low**: Missing feature or suboptimal design choice

4. **Actionable recommendations**: Each finding should have a specific
   recommendation with estimated effort (1-line fix, small PR, major feature).

### Improved Key Questions

Add these questions:

- Are probability weights correctly handled across the full SDDP pipeline
  (LP objective → cut construction → cut sharing → convergence bounds)?
- What are the default scaling parameters and are they adequate for both
  small IEEE benchmarks and large national systems?
- Which PLP features are available via `user_constraints` vs truly missing?
- What is the condition number of the LP for each IEEE test case?
- How does the cascade method compare to nested Benders in the literature?
