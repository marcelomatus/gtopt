#!/usr/bin/env julia
# UC.jl-as-oracle driver for the ucjl2gtopt converter golden tests.
#
# Reads a UC.jl benchmark JSON, solves it with UC.jl + CPLEX, and
# dumps a compact JSON of the resulting per-generator commitment
# status and dispatch values.  The Python test suite consumes the
# output to cross-check gtopt's converted-and-solved answer against
# UC.jl's authoritative solution.
#
# Usage:
#   julia tools/ucjl_solve_golden.jl <input.json> <output_golden.json>
#
# Output schema (compact JSON):
#   {
#     "objective": <Real>,
#     "horizon_h": <Int>,
#     "thermal_on":  { gen_name: [is_on per block] },
#     "thermal_mw":  { gen_name: [production_mw per block] },
#     "ucjl_version": "<UnitCommitment.jl semver>"
#   }
#
# Solver: CPLEX — same backend gtopt uses by default, so the
# cross-check compares like-for-like solver behavior.  UC.jl's own
# usage_test.jl uses HiGHS / Cbc; switching to CPLEX here removes
# solver-specific MIP-vertex differences from the comparison.

using UnitCommitment
using JuMP
using CPLEX
using JSON
using Pkg

function solve_one(input_path::AbstractString)
    instance = UnitCommitment.read(input_path)
    optimizer = optimizer_with_attributes(
        CPLEX.Optimizer,
        "CPX_PARAM_SCRIND" => 0,
    )

    # Auto-detect "initcond" fixtures where every generator is missing
    # initial_status / initial_power.  UC.jl's case118-initcond is the
    # canonical case: it ships unset initial conditions so UC.jl's
    # generate_initial_conditions! heuristic can populate them.  Without
    # this call, build_model() fails with
    # "Initial conditions for gX must be provided".
    needs_initcond = any(
        any(g.initial_status === nothing || g.initial_power === nothing
            for g in sc.thermal_units)
        for sc in instance.scenarios
    )
    if needs_initcond
        println(stderr, "  populating initial conditions via UC.jl heuristic...")
        UnitCommitment.generate_initial_conditions!(instance, optimizer)
    end

    model = UnitCommitment.build_model(
        instance = instance,
        optimizer = optimizer,
        variable_names = true,
    )
    optimize!(model)
    sol = UnitCommitment.solution(model)

    # UC.jl's ``solution()`` returns an OrderedDict whose keys have
    # changed across versions (older tests reference ``"Thermal: Is
    # on"`` / ``"Thermal: Production (MW)"``; current shows ``"Is on"``
    # / ``"Thermal production (MW)"``).  Try both layouts; pick the
    # one present.  For stochastic models the top-level dict is keyed
    # by scenario name; recurse one level in that case.
    function pick(d::AbstractDict)
        is_on = nothing
        mw = nothing
        for k in keys(d)
            ks = string(k)
            if ks == "Thermal: Is on" || ks == "Is on"
                is_on = d[k]
            elseif ks == "Thermal: Production (MW)" ||
                   ks == "Thermal production (MW)"
                mw = d[k]
            end
        end
        return is_on, mw
    end

    is_on, mw = pick(sol)
    scenario_keys = nothing
    if is_on === nothing && mw === nothing
        scenario_keys = sort(collect(map(string, keys(sol))))
        scenarios = Dict{String, Any}()
        for sk in scenario_keys
            s_is_on, s_mw = pick(sol[sk])
            scenarios[sk] = Dict("thermal_on" => s_is_on, "thermal_mw" => s_mw)
        end
        return Dict(
            "objective" => objective_value(model),
            "scenarios" => scenarios,
        )
    end

    return Dict(
        "objective" => objective_value(model),
        "thermal_on" => is_on,
        "thermal_mw" => mw,
    )
end

function main()
    if length(ARGS) != 2
        println(stderr, "usage: julia ucjl_solve_golden.jl <input.json> <output.json>")
        exit(2)
    end
    input_path = ARGS[1]
    output_path = ARGS[2]
    println(stderr, "Solving $input_path with UC.jl + CPLEX...")
    out = solve_one(input_path)
    open(output_path, "w") do io
        JSON.print(io, out, 2)
    end
    println(stderr, "Wrote $output_path (obj = $(out["objective"]))")
end

main()
