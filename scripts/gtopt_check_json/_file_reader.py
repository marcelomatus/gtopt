# SPDX-License-Identifier: BSD-3-Clause
"""Utilities for reading gtopt FieldSched values from Parquet/CSV files.

gtopt input files follow this schema:

- **Index columns** (any subset of): ``scenario``, ``stage``, ``block``
- **Element columns**: named ``uid:<N>`` or by element name (e.g. ``d1``)

The :func:`read_file_sched` function resolves a FieldSched file reference
to actual numeric values for a given element and (scenario, block) pair.
"""

from pathlib import Path
from typing import Any


def build_table_path(
    input_dir: str,
    class_name: str,
    field_name: str,
) -> str:
    """Build the base file path (without extension) for an input table.

    Mirrors the C++ ``build_table_path`` in ``array_index_traits.cpp``.

    If *field_name* contains ``@``, the part before ``@`` is the class
    and the part after is the field.  Otherwise *class_name* / *field_name*.
    """
    if "@" in field_name:
        parts = field_name.split("@", 1)
        return str(Path(input_dir) / parts[0] / parts[1])
    return str(Path(input_dir) / class_name / field_name)


def try_read_table(
    base_path: str,
    preferred_format: str = "parquet",
) -> Any:
    """Try to read a table from Parquet or CSV with fallback.

    Parameters
    ----------
    base_path
        Path without extension (e.g. ``input/Demand/lmax``).
    preferred_format
        ``"parquet"`` or ``"csv"``.

    Returns
    -------
    pandas.DataFrame or None
        The loaded table, or None if no file found.
    """
    import pandas as pd  # pylint: disable=import-outside-toplevel

    extensions = (
        [".parquet", ".parquet.gz", ".csv", ".csv.gz"]
        if preferred_format == "parquet"
        else [".csv", ".csv.gz", ".parquet", ".parquet.gz"]
    )

    for ext in extensions:
        fpath = Path(base_path + ext)
        if not fpath.exists():
            continue
        try:
            if ".parquet" in ext:
                return pd.read_parquet(fpath)
            return pd.read_csv(fpath)
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            continue
    return None


def resolve_file_sched_value(
    base_dir: str,
    class_name: str,
    field_name: str,
    element_uid: int | None,
    element_name: str | None,
    scenario_uid: int | None,
    block_uid: int | None,
    preferred_format: str = "parquet",
) -> float | None:
    """Resolve a single scalar from a FieldSched file reference.

    Parameters
    ----------
    base_dir
        The base directory for the case (where the JSON file lives).
    class_name
        Element class (e.g. ``"Demand"``, ``"Generator"``).
    field_name
        The FieldSched file reference string (e.g. ``"lmax"``).
    element_uid
        UID of the element to look up.
    element_name
        Name of the element (fallback column name).
    scenario_uid
        Scenario UID to filter by (or None to skip filter).
    block_uid
        Block UID to filter by (or None to skip filter).
    preferred_format
        ``"parquet"`` or ``"csv"``.

    Returns
    -------
    float or None
        The resolved value, or None if not found.
    """
    input_dir = _get_input_directory(base_dir)
    table_base = build_table_path(input_dir, class_name, field_name)
    df = try_read_table(table_base, preferred_format)
    if df is None:
        return None

    return _lookup_value(df, element_uid, element_name, scenario_uid, block_uid)


def resolve_file_sched_series(
    base_dir: str,
    class_name: str,
    field_name: str,
    element_uid: int | None,
    element_name: str | None,
    preferred_format: str = "parquet",
) -> list[float]:
    """Resolve all values from a FieldSched file reference.

    Returns all values for the element across all scenario/stage/block
    combinations in the file.
    """
    input_dir = _get_input_directory(base_dir)
    table_base = build_table_path(input_dir, class_name, field_name)
    df = try_read_table(table_base, preferred_format)
    if df is None:
        return []

    col = _find_element_column(df, element_uid, element_name)
    if col is None:
        return []

    return [float(v) for v in df[col].dropna().tolist()]


def _get_input_directory(base_dir: str) -> str:
    """Return *base_dir* as the input directory root.

    The caller should have already resolved the options.input_directory
    relative to the case file location.
    """
    return base_dir


def _find_element_column(
    df: Any,
    element_uid: int | None,
    element_name: str | None,
) -> str | None:
    """Find the column for an element by UID or name."""
    cols = list(df.columns)

    # Try uid:<N> pattern first
    if element_uid is not None:
        uid_col = f"uid:{element_uid}"
        if uid_col in cols:
            return uid_col

    # Try element name
    if element_name and element_name in cols:
        return element_name

    # Try bare UID as column name
    if element_uid is not None and str(element_uid) in cols:
        return str(element_uid)

    return None


def _lookup_value(
    df: Any,
    element_uid: int | None,
    element_name: str | None,
    scenario_uid: int | None,
    block_uid: int | None,
) -> float | None:
    """Look up a single value from a DataFrame."""
    col = _find_element_column(df, element_uid, element_name)
    if col is None:
        return None

    filtered = df
    if scenario_uid is not None and "scenario" in filtered.columns:
        filtered = filtered[filtered["scenario"] == scenario_uid]
    if block_uid is not None and "block" in filtered.columns:
        filtered = filtered[filtered["block"] == block_uid]

    if filtered.empty:
        # No exact match — return the first available value
        if not df.empty:
            return float(df[col].iloc[0])
        return None

    return float(filtered[col].iloc[0])
