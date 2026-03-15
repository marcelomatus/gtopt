# SPDX-License-Identifier: BSD-3-Clause
"""Helper to run pandapower diagnostics on a gtopt planning case."""

import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any


def get_pandapower_diagnostics(planning: dict[str, Any]) -> str:
    """Try to convert the case to pandapower and run diagnostics.

    Uses gtopt2pp internally if available.  Falls back to direct
    pandapower conversion when possible.
    """
    # Try calling gtopt2pp via subprocess
    try:
        with tempfile.NamedTemporaryFile(
            suffix=".json", mode="w", delete=False
        ) as tmp:
            json.dump(planning, tmp)
            tmp_path = tmp.name

        out_path = tmp_path + ".pp.json"
        result = subprocess.run(
            [
                "gtopt2pp",
                tmp_path,
                "--scenario",
                "0",
                "--block",
                "0",
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
        net = pp.create_empty_network(
            name=sys.get("name", "gtopt_check")
        )

        # Build buses
        bus_map: dict[Any, int] = {}
        for bus in sys.get("bus_array", []):
            uid = bus.get("uid")
            name = bus.get("name", str(uid))
            voltage = bus.get("voltage", 110.0)
            pp_idx = pp.create_bus(
                net, vn_kv=voltage, name=name
            )
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
                pp.create_line_from_parameters(
                    net,
                    from_bus=a_idx,
                    to_bus=b_idx,
                    length_km=1.0,
                    r_ohm_per_km=0.01,
                    x_ohm_per_km=0.1,
                    c_nf_per_km=0,
                    max_i_ka=tmax / 110.0,
                    name=line.get(
                        "name", str(line.get("uid", ""))
                    ),
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
