#!/usr/bin/env julia
# UC.jl-as-oracle driver for the ucjl2gtopt converter golden tests.
#
# Reads a UC.jl benchmark JSON, solves it with UC.jl + HiGHS, and
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
# Solver: HiGHS (matches what UC.jl's own usage_test.jl uses, so
# pinned golden values from UC.jl's test suite are reproducible
# bit-for-bit when fed back through this driver).

using UnitCommitment
using JuMP
using HiGHS
using JSON
using Pkg

function solve_one(input_path::AbstractString)
    instance = UnitCommitment.read(input_path)
    model = UnitCommitment.build_model(
        instance = instance,
        optimizer = optimizer_with_attributes(
            HiGHS.Optimizer,
            "log_to_console" => false,
        ),
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
    println(stderr, "Solving $input_path with UC.jl + HiGHS...")
    out = solve_one(input_path)
    open(output_path, "w") do io
        JSON.print(io, out, 2)
    end
    println(stderr, "Wrote $output_path (obj = $(out["objective"]))")
end

main()
