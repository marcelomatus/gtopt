# Bash completion for gtopt and related tools
# Source this file from your ~/.bashrc or ~/.bash_completion:
#   source /path/to/gtopt/tools/gtopt-completion.bash

# ---------------------------------------------------------------------------
# Helper: complete files with given extensions
# ---------------------------------------------------------------------------
_gtopt_filedir_ext()
{
    local IFS=$'\n'
    COMPREPLY+=( $(compgen -f -X "!*.@($1)" -- "$cur") )
    COMPREPLY+=( $(compgen -d -- "$cur") )
}

# ---------------------------------------------------------------------------
# gtopt  (C++ standalone binary)
# ---------------------------------------------------------------------------
_gtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --solver)
            COMPREPLY=( $(compgen -W "clp cbc cplex highs" -- "$cur") )
            return ;;
        --check-solvers)
            COMPREPLY=( $(compgen -W "clp cbc cplex highs" -- "$cur") )
            return ;;
        --low-memory)
            COMPREPLY=( $(compgen -W "off snapshot compress" -- "$cur") )
            return ;;
        --output-format|-f|--input-format|-F)
            COMPREPLY=( $(compgen -W "parquet csv" -- "$cur") )
            return ;;
        --output-compression|-C|--lp-compression)
            COMPREPLY=( $(compgen -W "zstd gzip lz4 snappy uncompressed none brotli bzip2 xz lzo auto" -- "$cur") )
            return ;;
        --sddp-elastic-mode)
            COMPREPLY=( $(compgen -W "none forward backward both" -- "$cur") )
            return ;;
        --algorithm|-a)
            COMPREPLY=( $(compgen -W "default primal dual barrier" -- "$cur") )
            return ;;
        --system-file|-s|--lp-file|-l|--json-file|-j|--trace-log|-T|\
        --cut-directory|--log-directory)
            _filedir
            return ;;
        --input-directory|-D|--output-directory|-d)
            _filedir -d
            return ;;
        --sddp-num-apertures|--matrix-eps|-e|--threads|-t|\
        --sddp-max-iterations|--sddp-min-iterations|\
        --sddp-convergence-tol|--sddp-elastic-penalty|--lp-coeff-ratio|\
        --memory-limit|--cpu-factor)
            return ;;
        --build-mode)
            COMPREPLY=( $(compgen -W "serial scene-parallel full-parallel direct-parallel" -- "$cur") )
            return ;;
        --set)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        local opts="--help -h --version -V --solvers --check-solvers
            --solver --verbose -v --quiet -q
            --system-file -s --set
            --input-directory -D --input-format -F
            --output-directory -d --output-format -f --output-compression -C
            --lp-file -l --matrix-eps -e
            --lp-only -c --lp-debug --lp-compression --lp-coeff-ratio
            --json-file -j
            --stats -S --trace-log -T
            --algorithm -a --threads -t
            --use-single-bus -b --use-kirchhoff -k
            --sddp-num-apertures --sddp-max-iterations --sddp-min-iterations
            --sddp-convergence-tol --sddp-elastic-penalty
            --sddp-elastic-mode
            --cut-directory --log-directory
            --low-memory --memory-limit --cpu-factor --build-mode
            --recover"
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    else
        _filedir
    fi
}
complete -F _gtopt gtopt

# ---------------------------------------------------------------------------
# cvs2parquet
# ---------------------------------------------------------------------------
_cvs2parquet()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -o|--output)
            _gtopt_filedir_ext "parquet"
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--output -o --schema --verbose -v --version -V --no-color --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "csv|CSV|tsv|TSV"
    fi
}
complete -F _cvs2parquet cvs2parquet

# ---------------------------------------------------------------------------
# gtopt_check_json
# ---------------------------------------------------------------------------
_gtopt_check_json()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --config)
            _filedir
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
        --ai-provider)
            return ;;
        --ai-model|--ai-prompt|--ai-key)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--info --config --init-config --no-color
            --show-simulation --show-config --quiet -q
            --log-level -l --version -V
            --ai --no-ai --ai-provider --ai-model --ai-prompt --ai-key
            --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "json"
    fi
}
complete -F _gtopt_check_json gtopt_check_json

# ---------------------------------------------------------------------------
# gtopt_check_lp
# ---------------------------------------------------------------------------
_gtopt_check_lp()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --solver)
            COMPREPLY=( $(compgen -W "all auto cplex highs coinor neos" -- "$cur") )
            return ;;
        --algo)
            COMPREPLY=( $(compgen -W "default primal dual barrier" -- "$cur") )
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
        --config|--output)
            _filedir
            return ;;
        --email|--solver-url|--timeout|--optimal-eps|--feasible-eps|--barrier-eps)
            return ;;
        --ai-provider|--ai-model|--ai-prompt|--ai-key)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--last --analyze-only --quiet -q --no-neos
            --solver --algo --optimal-eps --feasible-eps --barrier-eps
            --email --solver-url --timeout --output --no-color --full
            --verbose -v --log-level -l --config --init-config
            --no-setup --show-config --benchmark
            --ai --no-ai --ai-provider --ai-model --ai-prompt --ai-key
            --version --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "lp|lp.gz|lp.zst|lp.lz4"
    fi
}
complete -F _gtopt_check_lp gtopt_check_lp

# ---------------------------------------------------------------------------
# gtopt_check_solvers
# ---------------------------------------------------------------------------
_gtopt_check_solvers()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -s|--solver)
            COMPREPLY=( $(compgen -W "clp cbc cplex highs" -- "$cur") )
            return ;;
        --gtopt-bin)
            _filedir
            return ;;
        -t|--test|--timeout)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--list -l --solver -s --test -t
            --gtopt-bin --timeout --no-color --verbose -v
            --version -V --help -h" -- "$cur") )
    fi
}
complete -F _gtopt_check_solvers gtopt_check_solvers

# ---------------------------------------------------------------------------
# gtopt_check_output
# ---------------------------------------------------------------------------
_gtopt_check_output()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -r|--results-dir)
            _filedir -d
            return ;;
        -j|--json-file|--config)
            _filedir
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--results-dir -r --json-file -j
            --quiet -q --no-color --config --init-config
            --log-level -l --version -V --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _gtopt_check_output gtopt_check_output

# ---------------------------------------------------------------------------
# gtopt_compare
# ---------------------------------------------------------------------------
_gtopt_compare()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --case)
            COMPREPLY=( $(compgen -W "bat_4b_24 ieee30b ieee_4b_ori ieee_57b plp plp_bat_4b_24 s1b" -- "$cur") )
            return ;;
        --gtopt-output|--plp-output)
            _filedir -d
            return ;;
        --pandapower-file|--save-pandapower-file)
            _gtopt_filedir_ext "json"
            return ;;
        --tol|--tol-lmp)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--case --gtopt-output --plp-output
            --pandapower-file --save-pandapower-file
            --tol --tol-lmp --log-level -l --no-color
            --version -V --help -h" -- "$cur") )
    fi
}
complete -F _gtopt_compare gtopt_compare

# ---------------------------------------------------------------------------
# gtopt_compress_lp
# ---------------------------------------------------------------------------
_gtopt_compress_lp()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --codec|--compressor)
            COMPREPLY=( $(compgen -W "auto gzip zstd lz4" -- "$cur") )
            return ;;
        --color)
            COMPREPLY=( $(compgen -W "auto always never" -- "$cur") )
            return ;;
        --config)
            _filedir
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--init-config --list-tools --quiet
            --codec --compressor --config --color
            --show-config --log-level -l --version --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "lp"
    fi
}
complete -F _gtopt_compress_lp gtopt_compress_lp

# ---------------------------------------------------------------------------
# gtopt_diagram
# ---------------------------------------------------------------------------
_gtopt_diagram()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -t|--diagram-type)
            COMPREPLY=( $(compgen -W "topology planning" -- "$cur") )
            return ;;
        -f|--format)
            COMPREPLY=( $(compgen -W "dot png svg pdf mermaid html" -- "$cur") )
            return ;;
        -o|--output)
            _filedir
            return ;;
        -s|--subsystem)
            COMPREPLY=( $(compgen -W "full electrical hydro" -- "$cur") )
            return ;;
        -L|--layout)
            COMPREPLY=( $(compgen -W "dot neato fdp sfdp circo twopi" -- "$cur") )
            return ;;
        -d|--direction)
            COMPREPLY=( $(compgen -W "LR TD BT RL" -- "$cur") )
            return ;;
        -a|--aggregate)
            COMPREPLY=( $(compgen -W "auto none bus type global" -- "$cur") )
            return ;;
        --palette)
            COMPREPLY=( $(compgen -W "default colorblind" -- "$cur") )
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
        -g|--top-gens|--focus-hops|--max-nodes|--voltage-threshold|\
        --scenario|--stage|--block)
            return ;;
        --filter-type)
            COMPREPLY=( $(compgen -W "hydro solar wind thermal battery" -- "$cur") )
            return ;;
        --focus-bus|--focus-generator|--focus-area)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--diagram-type -t --format -f --output -o
            --subsystem -s --layout -L --direction -d --clusters
            --aggregate -a --no-generators --top-gens -g
            --filter-type --focus-bus --focus-generator --focus-area
            --focus-hops --max-nodes --voltage-threshold --hide-isolated
            --compact --show --palette --scenario --stage --block
            --log-level -l --version -V --no-color
            --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "json"
    fi
}
complete -F _gtopt_diagram gtopt_diagram

# ---------------------------------------------------------------------------
# gtopt_field_extractor
# ---------------------------------------------------------------------------
_gtopt_field_extractor()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -o|--output)
            _filedir
            return ;;
        -f|--format)
            COMPREPLY=( $(compgen -W "md html" -- "$cur") )
            return ;;
        -e|--elements)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--output -o --format -f --elements -e
            --no-color --log-level -l --version -V --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _gtopt_field_extractor gtopt_field_extractor

# ---------------------------------------------------------------------------
# igtopt
# ---------------------------------------------------------------------------
_igtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -j|--json-file)
            _gtopt_filedir_ext "json"
            return ;;
        -d|--input-directory|--header-dir)
            _filedir -d
            return ;;
        -f|--input-format)
            COMPREPLY=( $(compgen -W "csv parquet" -- "$cur") )
            return ;;
        -c|--compression)
            COMPREPLY=( $(compgen -W "zstd gzip snappy lz4 none" -- "$cur") )
            return ;;
        --compression-level)
            return ;;
        -n|--name)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--json-file -j --input-directory -d
            --input-format -f --name -n --compression -c
            --compression-level
            --pretty -p --skip-nulls -N
            --parse-unexpected-sheets -U --zip -z
            --validate --ignore-errors
            --log-level -l --version -V
            --make-template -T --header-dir --list-sheets
            --no-color --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "xlsx|xls|ods"
    fi
}
complete -F _igtopt igtopt

# ---------------------------------------------------------------------------
# plp2gtopt
# ---------------------------------------------------------------------------
_plp2gtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -i|--input-dir|-o|--output-dir|--aperture-directory|-A)
            _filedir -d
            return ;;
        -f|--output-file|--variable-scales-file|-X|--log)
            _filedir
            return ;;
        -x|--excel-file)
            _gtopt_filedir_ext "xlsx"
            return ;;
        -F|--output-format|-I|--input-format)
            COMPREPLY=( $(compgen -W "parquet csv" -- "$cur") )
            return ;;
        -c|--compression)
            COMPREPLY=( $(compgen -W "zstd gzip snappy lz4 none" -- "$cur") )
            return ;;
        -S|--solver)
            COMPREPLY=( $(compgen -W "sddp mono monolithic" -- "$cur") )
            return ;;
        --boundary-cuts-mode)
            COMPREPLY=( $(compgen -W "noload separated combined" -- "$cur") )
            return ;;
        --cut-sharing-mode)
            COMPREPLY=( $(compgen -W "none expected accumulate max" -- "$cur") )
            return ;;
        --rsv-scale-mode)
            COMPREPLY=( $(compgen -W "plp auto" -- "$cur") )
            return ;;
        --pasada-mode)
            COMPREPLY=( $(compgen -W "auto hydro flow-turbine profile" -- "$cur") )
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
        -s|--last-stage|-d|--discount-rate|-m|--management-factor|-t|--last-time|\
        -n|--name|--sys-version|--compression-level|--demand-fail-cost|\
        --reserve-fail-cost|--scale-objective|--scale-theta|--scale-alpha|\
        -y|--hydrologies|--soft-emin-cost|\
        -p|--probability-factors|-a|--num-apertures|--boundary-max-iterations|\
        -g|--stages-phase|--rsv-energy-scale|--bat-energy-scale|\
        --stationary-tol|--stationary-window|--tech-overrides)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--input-dir -i --output-dir -o --output-file -f
            --last-stage -s --discount-rate -d --management-factor -m
            --last-time -t --name -n --sys-version
            --output-format -F --input-format -I --compression -c
            --compression-level --zip -z --excel-output -E --excel-file -x
            --demand-fail-cost --reserve-fail-cost --scale-objective
            --scale-theta --scale-alpha
            --use-single-bus -b --use-kirchhoff -k --no-use-kirchhoff
            --use-line-losses -L
            --hydrologies -y --first-scenario --show-simulation
            --probability-factors -p
            --solver -S --num-apertures -a --aperture-directory -A
            --cut-sharing-mode --boundary-cuts-mode
            --boundary-max-iterations --no-boundary-cuts --hot-start-cuts
            --stationary-tol --stationary-window
            --info --validate
            --variable-scales-template --variable-scales-file -X
            --auto-rsv-energy-scale --no-auto-rsv-energy-scale
            --auto-bat-energy-scale --no-auto-bat-energy-scale
            --rsv-scale-mode --rsv-energy-scale --bat-energy-scale
            --clamp-battery-efficiency --no-clamp-battery-efficiency
            --soft-emin-cost
            --embed-reservoir-constraints --no-embed-reservoir-constraints
            --pasada-mode
            --tech-detect --no-tech-detect --tech-overrides --tech-list
            --stages-phase -g
            --expand-water-rights --no-expand-water-rights
            --expand-lng --no-expand-lng
            --expand-ror --no-expand-ror
            --check --no-check --init-config
            --log --log-level -l --no-color --version -V --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _plp2gtopt plp2gtopt

# ---------------------------------------------------------------------------
# plp_compress_case
# ---------------------------------------------------------------------------
_plp_compress_case()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --codec)
            COMPREPLY=( $(compgen -W "xz gzip zstd lz4" -- "$cur") )
            return ;;
        --codec-args)
            return ;;
        --color)
            COMPREPLY=( $(compgen -W "auto always never" -- "$cur") )
            return ;;
        --config)
            _filedir
            return ;;
        --split-mb)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--decompress -d --codec --codec-args
            --split-mb --color --config --show-config
            --log-level -l --version --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _plp_compress_case plp_compress_case

# ---------------------------------------------------------------------------
# pp2gtopt
# ---------------------------------------------------------------------------
_pp2gtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -f|--file)
            _filedir
            return ;;
        -o|--output)
            _gtopt_filedir_ext "json"
            return ;;
        -n|--network)
            # Try to get available networks dynamically
            local networks
            networks=$(pp2gtopt --list-networks 2>/dev/null | grep -oP '^\s+\K\S+' 2>/dev/null)
            if [[ -n "$networks" ]]; then
                COMPREPLY=( $(compgen -W "$networks" -- "$cur") )
            fi
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--file -f --network -n --output -o
            --list-networks --check --no-check
            --version -V --help -h" -- "$cur") )
    fi
}
complete -F _pp2gtopt pp2gtopt

# ---------------------------------------------------------------------------
# gtopt2pp
# ---------------------------------------------------------------------------
_gtopt2pp()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -o|--output)
            _gtopt_filedir_ext "json"
            return ;;
        -s|--scenario|-b|--block)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--output -o --scenario -s --block -b
            --solve --all-blocks --check --no-check --diagnostic
            --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "json"
    fi
}
complete -F _gtopt2pp gtopt2pp

# ---------------------------------------------------------------------------
# ts2gtopt
# ---------------------------------------------------------------------------
_ts2gtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -o|--output)
            _filedir -d
            return ;;
        -H|--horizon|--output-horizon)
            _gtopt_filedir_ext "json"
            return ;;
        -P|--planning)
            _gtopt_filedir_ext "json"
            return ;;
        -f|--format)
            COMPREPLY=( $(compgen -W "parquet csv" -- "$cur") )
            return ;;
        -a|--agg)
            COMPREPLY=( $(compgen -W "mean median min max sum" -- "$cur") )
            return ;;
        -c|--compression)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
        -y|--year|-s|--stages|-b|--blocks|-i|--interval-hours|-t|--time-column)
            return ;;
        --preset)
            local presets
            presets=$(ts2gtopt --list-presets 2>/dev/null | grep -oP '^\s+\K\S+' 2>/dev/null)
            if [[ -n "$presets" ]]; then
                COMPREPLY=( $(compgen -W "$presets" -- "$cur") )
            fi
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--output -o --horizon -H --planning -P
            --year -y --stages -s --blocks -b
            --preset --list-presets --interval-hours -i
            --time-column -t --agg -a --format -f --compression -c
            --output-horizon --verify
            --log-level -l --version -V --no-color
            --help -h" -- "$cur") )
    else
        _gtopt_filedir_ext "csv|parquet"
    fi
}
complete -F _ts2gtopt ts2gtopt

# ---------------------------------------------------------------------------
# run_gtopt
# ---------------------------------------------------------------------------
_run_gtopt()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        -C|--compression)
            COMPREPLY=( $(compgen -W "zstd gzip snappy lz4 none" -- "$cur") )
            return ;;
        -o|--output-dir)
            _filedir -d
            return ;;
        --export-json)
            _gtopt_filedir_ext "json"
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
        -t|--threads|--plp-args|--enable-check|--disable-check)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--threads -t --compression -C
            --output-dir -o --plp-args
            --check --no-check --strict
            --enable-check --disable-check --list-checks
            --convert-only --export-json --dry-run
            --log-level -l --version -V --no-color
            --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _run_gtopt run_gtopt

# ---------------------------------------------------------------------------
# gtopt_monitor (formerly sddp_monitor)
# ---------------------------------------------------------------------------
_gtopt_monitor()
{
    local cur prev
    _init_completion || return

    case "$prev" in
        --status-file)
            _filedir
            return ;;
        --poll)
            return ;;
        --case-dir)
            _filedir -d
            return ;;
        --get)
            return ;;
        -l|--log-level)
            COMPREPLY=( $(compgen -W "DEBUG INFO WARNING ERROR CRITICAL" -- "$cur") )
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--status-file --poll --no-gui --no-color
            --log-level -l --get --case-dir
            --version -V --help -h" -- "$cur") )
    else
        _filedir -d
    fi
}
complete -F _gtopt_monitor gtopt_monitor
complete -F _gtopt_monitor sddp_monitor

# ---------------------------------------------------------------------------
# gtopt_config  (if available as a standalone command)
# ---------------------------------------------------------------------------
_gtopt_config()
{
    local cur prev
    _init_completion || return

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "--help -h --version -V" -- "$cur") )
    fi
}
complete -F _gtopt_config gtopt_config 2>/dev/null

# vim: ft=bash
