#!/usr/bin/env julia
# NREL-Sienna PowerSimulations.jl oracle driver for the sienna_to_gtopt
# golden tests.  Mirrors tools/ucjl_solve_golden.jl in spirit:
#
#   * Picks up a Sienna case via PowerSystemCaseBuilder (PSITestSystems).
#   * Wires a DecisionModel with the device + network models appropriate
#     to the variant we want to cross-check against gtopt.
#   * Solves with HiGHS (open-source, ships with the Julia package set;
#     CPLEX would mirror gtopt better but is gated behind a separate
#     install we can't assume on the agent box).
#   * Emits a compact JSON capturing the objective, per-generator
#     dispatch, line flows, reservoir storage, and battery SoC so the
#     Python test layer can re-solve the gtopt-converted JSON and
#     compare against the Julia answer.
#
# Usage:
#   julia tools/sienna_solve_golden.jl <slug> <output_golden.json>
#   julia tools/sienna_solve_golden.jl all                # solve every
#                                                           registered slug
#
# When invoked with the literal slug ``all``, the second arg is
# ignored and per-case JSONs are written to ``tools/sienna_goldens/``.

using PowerSystems
using PowerSimulations
using PowerSystemCaseBuilder
using HydroPowerSimulations
using StorageSystemsSimulations
using HiGHS
using JSON
using DataFrames
using Pkg
using Dates

const PSI = PowerSimulations
const PSY = PowerSystems

# ---------------------------------------------------------------------------
# Slug → PowerSystemCaseBuilder system name mapping.
# ---------------------------------------------------------------------------
const SLUG_TO_CASE = Dict{String, String}(
    "cascading_hydro"    => "c_sys5_hy_cascading_turbine_energy",
    "monitored_line"     => "c_sys5_ml",
    "hvdc"               => "c_sys5_dc",
    # PowerSystemCaseBuilder ships ``c_sys5_hydro_pump_energy`` (renamed
    # from the older ``c_sys5_hy_pump_energy``); fall back to the ED
    # pumped-storage system if either is unavailable.
    "pumped_storage"     => "c_sys5_hydro_pump_energy",
    "interruptible_load" => "c_sys5_il",
    "fuel_cost_ts"       => "c_sys5_re_fuel_cost",
)

# Battery degeneracy subcases b..f → batt_test_case_<l>_sys.
for sub in ("b", "c", "d", "e", "f")
    SLUG_TO_CASE["battery_degeneracy_$sub"] = "batt_test_case_$(sub)_sys"
end

# ---------------------------------------------------------------------------
# Build a DecisionModel template tuned for the requested slug.
# ---------------------------------------------------------------------------
"""
    has_component(sys, type_sym) → Bool

Convenience: check whether ``type_sym`` is a defined symbol in either
PSY or HPS *and* the system has at least one such component.  This
lets us probe optional device types (e.g., ``HydroPumpedStorage``,
``BatteryEMS``) that were removed in PSY v5 without an UndefVarError.
"""
function has_component(sys::PSY.System, type_sym::Symbol)
    T = nothing
    for mod in (PSY, HydroPowerSimulations, StorageSystemsSimulations)
        if isdefined(mod, type_sym)
            T = getfield(mod, type_sym)
            break
        end
    end
    T === nothing && return (false, nothing)
    return (!isempty(get_components(T, sys)), T)
end

function build_template(slug::AbstractString, sys::PSY.System)
    template = ProblemTemplate(NetworkModel(CopperPlatePowerModel))
    # All variants ship thermal + load.  Pick the conservative
    # ``ThermalBasicUnitCommitment`` so we get an explicit OnVariable
    # we can read back per generator.
    set_device_model!(template, ThermalStandard, ThermalBasicUnitCommitment)
    set_device_model!(template, PowerLoad, StaticPowerLoad)

    if !isempty(get_components(RenewableDispatch, sys))
        set_device_model!(template, RenewableDispatch, RenewableFullDispatch)
    end
    have, T = has_component(sys, :RenewableNonDispatch)
    if have
        set_device_model!(template, T, FixedOutput)
    end

    # Hydro — type names vary between PSY v4 (HydroDispatch /
    # HydroEnergyReservoir / HydroPumpedStorage) and PSY v5 (HydroTurbine
    # + HydroReservoir + HydroPumpStorage).  Probe + dispatch the right
    # formulation for whichever lineage is present.
    have, T = has_component(sys, :HydroDispatch)
    if have
        set_device_model!(template, T,
            HydroPowerSimulations.HydroDispatchRunOfRiver)
    end
    have, T = has_component(sys, :HydroEnergyReservoir)
    if have
        # PSY v4: HydroEnergyReservoir uses HydroDispatchRunOfRiver from
        # HydroPowerSimulations (energy-budget formulation).
        set_device_model!(template, T,
            HydroPowerSimulations.HydroDispatchRunOfRiverBudget)
    end
    have, T = has_component(sys, :HydroTurbine)
    if have
        # PSY v5 split: HydroTurbine carries the power conversion.
        set_device_model!(template, T,
            HydroPowerSimulations.HydroTurbineEnergyDispatch)
    end
    # PSY v5 uses ``HydroPumpTurbine`` (and an associated
    # ``HydroReservoir`` upper basin) for the pumped-storage fixture.
    # Older PSY versions used ``HydroPumpedStorage`` / ``HydroPumpStorage``.
    for sym in (:HydroPumpedStorage, :HydroPumpStorage, :HydroPumpTurbine)
        have, T = has_component(sys, sym)
        if have
            set_device_model!(template, T,
                HydroPowerSimulations.HydroPumpEnergyDispatch)
        end
    end

    # Storage (battery / EMS): only one dispatch model on the modern
    # SSS, applied uniformly across whichever type the case ships.
    for sym in (:EnergyReservoirStorage, :GenericBattery, :BatteryEMS)
        have, T = has_component(sys, sym)
        if have
            set_device_model!(template, T,
                StorageSystemsSimulations.StorageDispatchWithReserves)
        end
    end

    # Interruptible / controllable load — needed for c_sys5_il.
    have, T = has_component(sys, :InterruptiblePowerLoad)
    if have
        set_device_model!(template, T, PowerLoadInterruption)
    end

    # Lines: monitored_line / hvdc want non-copperplate to make sense,
    # but the gtopt converter for these emits ``use_single_bus = true``,
    # so a copperplate Sienna run is the apples-to-apples reference for
    # the dispatch totals (line-level flows on the Sienna side will
    # then be absent — we capture them as empty in the JSON).  No
    # branch device model registration needed under CopperPlate.

    return template
end

# ---------------------------------------------------------------------------
# Convert a long-form (DateTime, name, value) DataFrame from
# read_variables / read_expressions to ``{name => [v per timestep]}``.
# ---------------------------------------------------------------------------
function long_df_to_dict(df::DataFrame; value_col::Symbol = :value)
    out = Dict{String, Vector{Float64}}()
    if !("name" in names(df)) || !(string(value_col) in names(df))
        return out
    end
    sort!(df, [:DateTime, :name])
    for sub in groupby(df, :name)
        nm = string(first(sub.name))
        out[nm] = Vector{Float64}(sub[!, value_col])
    end
    return out
end

# Wide DataFrames (rows = time, columns = unit names, plus DateTime) —
# used by some variable types in newer PSI versions.  We coerce both
# shapes into the long_df_to_dict output.
function wide_df_to_dict(df::DataFrame)
    out = Dict{String, Vector{Float64}}()
    for col in names(df)
        col == "DateTime" && continue
        out[col] = Vector{Float64}(df[!, col])
    end
    return out
end

function df_to_dict(df::DataFrame)
    cols = names(df)
    if "name" in cols && "value" in cols
        return long_df_to_dict(df)
    elseif "DateTime" in cols
        return wide_df_to_dict(df)
    else
        # Unknown shape — return empty (caller will silently skip).
        return Dict{String, Vector{Float64}}()
    end
end

# ---------------------------------------------------------------------------
# Pluck a variable from the read_variables() Dict by suffix-match on the
# variable key (e.g. "ActivePowerVariable__ThermalStandard").
# ---------------------------------------------------------------------------
function pluck(varmap::AbstractDict, want::AbstractString)
    for (k, v) in varmap
        if occursin(want, string(k))
            return df_to_dict(v)
        end
    end
    return Dict{String, Vector{Float64}}()
end

function pluck_all(varmap::AbstractDict, want::AbstractString)
    merged = Dict{String, Vector{Float64}}()
    for (k, v) in varmap
        if occursin(want, string(k))
            for (nm, vec) in df_to_dict(v)
                # Disambiguate equal names from different device types by
                # tagging the device-type suffix when there's a collision.
                key = haskey(merged, nm) ? "$nm@$(string(k))" : nm
                merged[key] = vec
            end
        end
    end
    return merged
end

# ---------------------------------------------------------------------------
# Solve one case and return the golden-payload Dict.
# ---------------------------------------------------------------------------
function solve_one(slug::AbstractString, case_name::AbstractString)
    println(stderr, "  building system  $case_name ...")
    sys = build_system(PSITestSystems, case_name)

    println(stderr, "  building template for $slug ...")
    template = build_template(slug, sys)

    println(stderr, "  building DecisionModel ...")
    problem = DecisionModel(template, sys;
        optimizer = optimizer_with_attributes(HiGHS.Optimizer,
            "output_flag" => false),
        name = slug,
    )

    payload = Dict{String, Any}(
        "case" => case_name,
        "slug" => slug,
        "psi_version" => string(pkgversion(PowerSimulations)),
        "psy_version" => string(pkgversion(PowerSystems)),
        "psc_version" => string(pkgversion(PowerSystemCaseBuilder)),
        "highs_version" => string(pkgversion(HiGHS)),
    )

    mktempdir() do dir
        build_status = build!(problem; output_dir = dir)
        if build_status != PSI.ModelBuildStatus.BUILT
            payload["status"] = "build_failed"
            payload["status_detail"] = string(build_status)
            return
        end
        solve_status = solve!(problem)
        if solve_status != PSI.RunStatus.SUCCESSFULLY_FINALIZED
            payload["status"] = "solve_failed"
            payload["status_detail"] = string(solve_status)
            return
        end

        results = OptimizationProblemResults(problem)
        payload["status"] = "ok"
        payload["objective"] = get_objective_value(results)

        vars = read_variables(results)
        params = try
            read_parameters(results)
        catch
            Dict{Symbol, DataFrame}()
        end

        # Gen dispatch — ActivePowerVariable__* spans Thermal / Renewable /
        # Hydro / Storage discharge.  pluck_all merges across types.
        payload["gen_dispatch_mw"] =
            pluck_all(vars, "ActivePowerVariable")
        # On/off thermal commitment status.
        payload["thermal_on"] = pluck(vars, "OnVariable__ThermalStandard")
        # Storage SoC: EnergyVariable__* in StorageSystemsSimulations.
        payload["storage_soc"] = pluck_all(vars, "EnergyVariable")
        # Reservoir storage (HydroEnergyReservoir / HydroReservoir).
        # PSY v5's HydroReservoir exposes the volume as
        # ``HydroReservoirVolumeVariable`` from HydroPowerSimulations;
        # PSY v4's HydroEnergyReservoir exposes it as
        # ``EnergyVariable__HydroEnergyReservoir``.  Merge both shapes.
        hydro_state = pluck_all(vars, "EnergyVariable__HydroEnergyReservoir")
        for (k, v) in pluck_all(vars, "HydroReservoirVolumeVariable")
            hydro_state[k] = v
        end
        for (k, v) in pluck_all(vars, "WaterVolumeVariable")
            hydro_state[k] = v
        end
        payload["hydro_storage"] = hydro_state

        # Charge / pump powers (battery, pumped hydro).
        payload["storage_charge_mw"] =
            pluck_all(vars, "ActivePowerInVariable")
        payload["storage_discharge_mw"] =
            pluck_all(vars, "ActivePowerOutVariable")

        # Line flows (when network model isn't copperplate; under
        # CopperPlate we end up with an empty dict — fine).
        payload["line_flow_mw"] = pluck_all(vars, "FlowActivePowerVariable")

        # Records useful for debugging the Python cross-check.
        payload["variable_keys"] = sort([string(k) for k in keys(vars)])

        # Capture the time-resolution metadata the gtopt JSON uses to
        # line up its block array.
        time_steps = nothing
        for (_, df) in vars
            if "DateTime" in names(df) && nrow(df) > 0
                # Number of distinct DateTime values.
                time_steps = length(unique(df.DateTime))
                break
            end
        end
        payload["horizon_hours"] = time_steps === nothing ? 0 : time_steps
    end

    return payload
end

# ---------------------------------------------------------------------------
# Pretty-format the payload size for the stderr summary line.
# ---------------------------------------------------------------------------
function summarise(payload::AbstractDict)
    obj = get(payload, "objective", missing)
    status = get(payload, "status", "unknown")
    return "status=$status obj=$obj"
end

# ---------------------------------------------------------------------------
# main: dispatch on argv.
# ---------------------------------------------------------------------------
function main()
    if length(ARGS) < 1
        println(stderr,
            "usage: julia sienna_solve_golden.jl <slug|all> [output.json]")
        exit(2)
    end
    slug = ARGS[1]

    if slug == "all"
        repo_root = abspath(joinpath(@__DIR__, ".."))
        out_dir = joinpath(repo_root, "tools", "sienna_goldens")
        mkpath(out_dir)
        ok = 0
        fail = 0
        for (s, case) in sort(collect(SLUG_TO_CASE))
            println(stderr, "[$s → $case]")
            try
                payload = solve_one(s, case)
                out = joinpath(out_dir, "$s.json")
                open(out, "w") do io
                    JSON.print(io, payload, 2)
                end
                println(stderr, "  wrote $out  ", summarise(payload))
                ok += 1
            catch err
                println(stderr, "  FAILED: ", err)
                # Persist a stub so the Python test layer can mark XFAIL
                # without conflating "build error" with "missing file".
                out = joinpath(out_dir, "$s.json")
                open(out, "w") do io
                    JSON.print(io, Dict(
                        "slug" => s, "case" => case,
                        "status" => "exception",
                        "error" => string(err),
                    ), 2)
                end
                fail += 1
            end
        end
        println(stderr, "Done.  ok=$ok  fail=$fail  cases=$(length(SLUG_TO_CASE))")
        return
    end

    if !haskey(SLUG_TO_CASE, slug)
        println(stderr, "unknown slug $slug; choices: ",
            sort(collect(keys(SLUG_TO_CASE))))
        exit(2)
    end
    out_path = length(ARGS) >= 2 ? ARGS[2] :
        joinpath(@__DIR__, "sienna_goldens", "$slug.json")
    mkpath(dirname(out_path))
    payload = solve_one(slug, SLUG_TO_CASE[slug])
    open(out_path, "w") do io
        JSON.print(io, payload, 2)
    end
    println(stderr, "Wrote $out_path  ", summarise(payload))
end

main()
