# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_timeseries_export — export gtopt results to a formatted Excel workbook.

Provides a library and CLI for converting the time-series output of a
solved gtopt optimization case into a single ``.xlsx`` workbook with:

- One sheet per output category (Generation, Load, Flows, SoC, LMPs, ...)
- Auto-width columns and a bold header row
- An Overview sheet with KPI values from gtopt_results_summary
"""

from .exporter import export_to_excel, export_from_dict  # noqa: F401
from .main import main  # noqa: F401
