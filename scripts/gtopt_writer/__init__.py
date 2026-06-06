# SPDX-License-Identifier: BSD-3-Clause
"""Cross-converter gtopt planning-JSON writer framework (issue #507).

This package collects the shared layer the four planning-writer
converters (``plp2gtopt``, ``plexos2gtopt``, ``sddp2gtopt``,
``pp2gtopt``) plus the Excel converter (``igtopt``) gradually
migrate to.  Each phase of the unification ships one self-contained
module here; converters call into them as they are wired.

Phase status (2026-06-06):

  Phase 3: :mod:`gtopt_writer.simulation` — ``build_simulation``
           shared builder lands here.  Not yet wired into the
           converters; the per-converter ``build_simulation`` /
           inline simulation assembly stay in place until Phase 6
           shrinks them to adapters.

  Phase 4: per-entity builders under ``gtopt_writer.entities.*``
           (not yet started).

  Phase 5: :mod:`gtopt_writer.io` for JSON / Parquet write
           consolidation (not yet started).
"""

from __future__ import annotations
