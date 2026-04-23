# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_results_summary — KPI aggregator for gtopt output files.

Provides a library and CLI for producing a compact JSON summary of the
key performance indicators of a solved gtopt optimization case:

- Objective value (total cost)
- Total generation (MWh) broken down by technology where possible
- Total served and unserved demand (MWh)
- Min / max / mean LMP ($/MWh)
- Peak generation block
- Solver status

The summary is designed to power the dashboard KPI cards of the GUI Plus
frontend, and can also be consumed programmatically.
"""

from .main import main  # noqa: F401
from .summary import (  # noqa: F401
    summarize_output_dict,
    summarize_results,
)
