# SPDX-License-Identifier: BSD-3-Clause
"""Helper to run pandapower diagnostics on a gtopt planning case."""

import json
import math
import subprocess
import tempfile
from pathlib import Path
from typing import Any

# Sentinel value for unconstrained line thermal limits (matches gtopt2pp.convert)
_TMAX_UNLIMITED = 9999.0
# Default voltage base (kV) used as fallback when a bus has no voltage data
_DEFAULT_KV = 110.0
# Relative voltage-level difference threshold above which a line is promoted
# to a transformer (must match the constant in gtopt2pp.convert)
_VOLTAGE_THRESHOLD = 0.05


def get_pandapower_diagnostics(planning: dict[str, Any]) -> str:
    """Try to convert the case to pandapower and run diagnostics.

    Uses gtopt2pp internally if available.  Falls back to direct
    pandapower conversion when possible.
    """
    # Resolve first scenario/block UID (gtopt2pp expects UIDs, not 0-based indices)
    sim = planning.get("simulation", {})
    scenarios = sim.get("scenario_array", [{"uid": 1}])
    blocks = sim.get("block_array", [{"uid": 1}])
    first_scenario_uid = str(scenarios[0].get("uid", 1))
    first_block_uid = str(blocks[0].get("uid", 1))

    try:
        with tempfile.NamedTemporaryFile(suffix=".json", mode="w", delete=False) as tmp:
            json.dump(planning, tmp)
            tmp_path = tmp.name

        out_path = tmp_path + ".pp.json"
        result = subprocess.run(
            [
                "gtopt2pp",
                tmp_path,
                "--scenario",
                first_scenario_uid,
                "--block",
                first_block_uid,
                "-o",
                out_path,
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        Path(tmp_path).unlink(missing_ok=True)

        if result.returncode == 0 and Path(out_path).exists():
            report = _run_pp_diagnostics(out_path)
            Path(out_path).unlink(missing_ok=True)
            return report
        Path(out_path).unlink(missing_ok=True)
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        pass

    # Fallback: try direct pandapower conversion
    return _try_direct_diagnostics(planning)


def _run_pp_diagnostics(pp_json_path: str) -> str:
    """Load a pandapower JSON network and run diagnostic checks."""
    try:
        import pandapower as pp  # pylint: disable=import-outside-toplevel

        net = pp.from_json(pp_json_path)
        diag = pp.diagnostic(net, report_style=None)
        if not diag:
            return "pandapower diagnostic: no issues found"
        lines: list[str] = []
        for check_name, result in diag.items():
            lines.append(f"  {check_name}: {result}")
        return "\n".join(lines)
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        return f"pandapower diagnostic failed: {exc}"


def _try_direct_diagnostics(planning: dict[str, Any]) -> str:
    """Attempt a minimal pandapower network build for diagnostics."""
    try:
        import pandapower as pp  # pylint: disable=import-outside-toplevel

        sys = planning.get("system", {})
        net = pp.create_empty_network(name=sys.get("name", "gtopt_check"))

        # Build buses
        bus_map: dict[Any, int] = {}
        for bus in sys.get("bus_array", []):
            uid = bus.get("uid")
            name = bus.get("name", str(uid))
            voltage = bus.get("voltage", 110.0)
            pp_idx = pp.create_bus(net, vn_kv=voltage, name=name)
            bus_map[uid] = pp_idx
            if name:
                bus_map[name] = pp_idx

        # Build lines (simplified)
        for line in sys.get("line_array", []):
            a_ref = line.get("bus_a")
            b_ref = line.get("bus_b")
            a_idx = bus_map.get(a_ref)
            b_idx = bus_map.get(b_ref)
            if a_idx is not None and b_idx is not None:
                tmax = 250.0
                tmax_val = line.get("tmax_ab")
                if isinstance(tmax_val, (int, float)):
                    tmax = float(tmax_val)

                kv_a = float(net.bus.at[a_idx, "vn_kv"])
                kv_b = float(net.bus.at[b_idx, "vn_kv"])
                kv_max = max(kv_a, kv_b)

                if kv_max > 0 and abs(kv_a - kv_b) / kv_max > _VOLTAGE_THRESHOLD:
                    # Different voltage levels: model as transformer to avoid
                    # "different_voltage_levels_connected" diagnostics.
                    sn_mva = tmax / math.sqrt(3) if tmax < _TMAX_UNLIMITED else 100.0
                    hv_bus = a_idx if kv_a >= kv_b else b_idx
                    lv_bus = b_idx if kv_a >= kv_b else a_idx
                    hv_kv = max(kv_a, kv_b)
                    lv_kv = min(kv_a, kv_b)
                    pp.create_transformer_from_parameters(
                        net,
                        hv_bus=hv_bus,
                        lv_bus=lv_bus,
                        sn_mva=sn_mva,
                        vn_hv_kv=hv_kv,
                        vn_lv_kv=lv_kv,
                        vkr_percent=0.0,
                        vk_percent=5.0,
                        pfe_kw=0.0,
                        i0_percent=0.0,
                        name=line.get("name", str(line.get("uid", ""))),
                    )
                else:
                    pp.create_line_from_parameters(
                        net,
                        from_bus=a_idx,
                        to_bus=b_idx,
                        length_km=1.0,
                        r_ohm_per_km=0.01,
                        x_ohm_per_km=0.1,
                        c_nf_per_km=0,
                        max_i_ka=tmax / max(kv_a, _DEFAULT_KV),
                        name=line.get("name", str(line.get("uid", ""))),
                    )

        diag = pp.diagnostic(net, report_style=None)
        if not diag:
            return "pandapower diagnostic: no issues found"
        lines_out: list[str] = []
        for check_name, result in diag.items():
            lines_out.append(f"  {check_name}: {result}")
        return "\n".join(lines_out)
    except ImportError:
        return ""
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        return f"pandapower diagnostic failed: {exc}"
