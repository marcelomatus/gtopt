# SPDX-License-Identifier: BSD-3-Clause
"""gtopt2pbi — make gtopt Parquet field files Power BI ready.

Reshapes the wide ``uid:N`` field Parquet/CSV files in a gtopt case **in
place** to the tidy ``[<index cols>, uid, value]`` long shape and re-encodes
them with a single, universally readable codec (default ``zstd``, replacing
legacy ``lz4`` whose deprecated Hadoop-framed variant Power BI handles
unreliably).  The result opens natively in Power BI / Power Query with no
unpivot step.

Only wide field tables are touched.  Structural tables (block/stage
definitions), SDDP cut files, already-long tables, and hive-partitioned solve
**output** are left exactly as-is.  gtopt auto-detects the layout on read, so
converting a case's input files is transparent to a later solve.

This is the standalone counterpart to the final pass ``plp2gtopt`` runs during
a fresh conversion (:func:`plp2gtopt.base_writer.convert_tree_to_long`); use it
to upgrade files that already exist on disk without re-running the conversion.
"""

from .main import main, relayout_tree  # noqa: F401
