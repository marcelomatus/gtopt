# -*- coding: utf-8 -*-
"""Writer for the block-to-hour map derived from indhor.csv.

Writes ``BlockHourMap/block_hour_map.parquet`` (or ``.csv``) alongside the
other plp2gtopt time-series outputs.  The output file has the same structure
as the input ``indhor.csv`` but with normalised column names and an
efficient Parquet encoding.

The JSON simulation section will reference this file under the key
``"block_hour_map"`` so that post-processing tools (e.g. ts2gtopt's
``build_hour_block_map``) can reconstruct hourly time-series from
block-granularity solver output.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from .indhor_parser import IndhorParser
from .base_writer import BaseWriter


class IndhorWriter(BaseWriter):
    """Writes the hour-to-block mapping from an :class:`IndhorParser` instance.

    Output columns (all ``int32``):
        year, month, day, hour, block

    The ``hour`` column uses the original 1-based PLP convention (1-24).
    The ``block`` column uses the original 1-based PLP block UIDs.
    """

    #: Relative subdirectory name inside the output directory.
    SUBDIR = "BlockHourMap"
    #: Stem of the output file (without extension).
    FILE_STEM = "block_hour_map"

    def __init__(
        self,
        indhor_parser: Optional[IndhorParser] = None,
        options: Optional[dict] = None,
    ) -> None:
        super().__init__(None, options)
        self.indhor_parser = indhor_parser

    def to_parquet(self, output_dir: Path) -> Optional[str]:
        """Write block_hour_map.parquet to *output_dir*.

        Returns the relative path string (suitable for the JSON
        ``block_hour_map`` key), or ``None`` when there is no data.
        """
        if (
            self.indhor_parser is None
            or self.indhor_parser.df is None
            or self.indhor_parser.df.empty
        ):
            return None

        df = self.indhor_parser.df
        output_dir.mkdir(parents=True, exist_ok=True)

        compression = self.get_compression()
        fmt = (
            self.options.get("output_format", "parquet") if self.options else "parquet"
        )

        if fmt == "csv":
            out_file = output_dir / f"{self.FILE_STEM}.csv"
            df.to_csv(out_file, index=False)
        else:
            out_file = output_dir / f"{self.FILE_STEM}.parquet"
            df.to_parquet(out_file, index=False, compression=compression)

        # Return the relative key path (no extension; gtopt resolves format)
        return f"{self.SUBDIR}/{self.FILE_STEM}"
