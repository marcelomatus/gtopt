# SPDX-License-Identifier: BSD-3-Clause
"""Helper to generate an AI-readable mermaid diagram from a planning dict."""

import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any


def get_mermaid_summary(planning: dict[str, Any]) -> str:
    """Try to produce a mermaid diagram using gtopt_diagram.

    Falls back to a simple text summary if gtopt_diagram is not available.
    """
    # Try calling gtopt_diagram via subprocess
    try:
        with tempfile.NamedTemporaryFile(suffix=".json", mode="w", delete=False) as tmp:
            json.dump(planning, tmp)
            tmp_path = tmp.name

        result = subprocess.run(
            [
                "gtopt_diagram",
                tmp_path,
                "--format",
                "mermaid",
                "-o",
                "/dev/stdout",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        Path(tmp_path).unlink(missing_ok=True)

        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        pass

    # Fallback: simple text summary
    return _text_summary(planning)


def _text_summary(planning: dict[str, Any]) -> str:
    """Generate a simple text summary of the network topology."""
    sys = planning.get("system", {})
    lines_out: list[str] = []

    buses = sys.get("bus_array", [])
    for bus in buses:
        name = bus.get("name", str(bus.get("uid", "?")))
        lines_out.append(f"Bus: {name}")

    for gen in sys.get("generator_array", []):
        name = gen.get("name", str(gen.get("uid", "?")))
        bus = gen.get("bus", "?")
        pmax = gen.get("pmax", "?")
        gcost = gen.get("gcost", "?")
        lines_out.append(f"Generator: {name} at bus {bus}, pmax={pmax}, gcost={gcost}")

    for dem in sys.get("demand_array", []):
        name = dem.get("name", str(dem.get("uid", "?")))
        bus = dem.get("bus", "?")
        lines_out.append(f"Demand: {name} at bus {bus}")

    for line in sys.get("line_array", []):
        name = line.get("name", str(line.get("uid", "?")))
        bus_a = line.get("bus_a", "?")
        bus_b = line.get("bus_b", "?")
        lines_out.append(f"Line: {name} from {bus_a} to {bus_b}")

    return "\n".join(lines_out)
