# -*- coding: utf-8 -*-

"""Utility functions for parsing index range and stages-phase specifications.

All user-visible indices follow the Fortran convention (1-based).
"""

from __future__ import annotations

from typing import List


def parse_index_range(spec: str) -> List[int]:
    """Parse a comma/range index specification using 1-based (Fortran) indices.

    Accepted formats (1-based):
      ``"1"``              → ``[1]``
      ``"1,2,3"``          → ``[1, 2, 3]``
      ``"5-10"``           → ``[5, 6, 7, 8, 9, 10]``
      ``"1,2,5-10,11"``    → ``[1, 2, 5, 6, 7, 8, 9, 10, 11]``

    Args:
        spec: Index-range specification string.

    Returns:
        Sorted, deduplicated list of 1-based integer indices.

    Raises:
        ValueError: If the specification is empty, contains non-integer tokens,
                    or a range has start > end.
    """
    if not spec or not spec.strip():
        raise ValueError(f"Empty index specification: {spec!r}")

    result: List[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            tokens = part.split("-", 1)
            try:
                start = int(tokens[0])
                end = int(tokens[1])
            except ValueError as exc:
                raise ValueError(
                    f"Invalid range {part!r} in specification {spec!r}"
                ) from exc
            if start > end:
                raise ValueError(
                    f"Range start {start} > end {end} in specification {spec!r}"
                )
            result.extend(range(start, end + 1))
        else:
            try:
                result.append(int(part))
            except ValueError as exc:
                raise ValueError(
                    f"Invalid index {part!r} in specification {spec!r}"
                ) from exc

    # Sort and deduplicate while preserving first occurrence order for ranges
    seen: set[int] = set()
    unique: List[int] = []
    for v in result:
        if v not in seen:
            seen.add(v)
            unique.append(v)
    return unique


def parse_stages_phase(spec: str, num_stages: int) -> List[List[int]]:
    """Parse a stages-per-phase specification string.

    Each comma-separated token defines one gtopt phase; the value is either a
    single Fortran (1-based) PLP-stage index or a *range* ``N:M`` meaning
    stages N through M (inclusive).  The trailing token ``...`` is a wildcard
    that auto-expands: the next stage after the last explicitly listed stage is
    used for each remaining phase until all stages are exhausted.

    Examples (12 stages total):
      ``"1:4,5,6,7,8,9,10,..."``
          → phase 1 = [1,2,3,4], phase 2 = [5], …, phase 7 = [10],
            phase 8 = [11], phase 9 = [12]

      ``"1:3,4:6,7:12"``
          → phase 1 = [1,2,3], phase 2 = [4,5,6], phase 3 = [7,…,12]

      ``"1,2,3"``
          → phase 1 = [1], phase 2 = [2], phase 3 = [3]
            (remaining stages, if any, are not included)

    Args:
        spec: The stages-phase specification string.
        num_stages: Total number of PLP stages (used for ``...`` expansion and
                    range validation).

    Returns:
        List of phases; each phase is a list of 1-based PLP-stage indices.

    Raises:
        ValueError: For malformed tokens or out-of-range stage numbers.
    """
    if not spec or not spec.strip():
        raise ValueError(f"Empty stages-phase specification: {spec!r}")

    tokens = [t.strip() for t in spec.split(",") if t.strip()]

    has_ellipsis = tokens and tokens[-1] == "..."
    if has_ellipsis:
        tokens = tokens[:-1]

    phases: List[List[int]] = []

    def _parse_token(tok: str) -> List[int]:
        """Parse a single token of the form ``N`` or ``N:M``."""
        if ":" in tok:
            parts = tok.split(":", 1)
            try:
                s, e = int(parts[0]), int(parts[1])
            except ValueError as exc:
                raise ValueError(
                    f"Invalid stage range {tok!r} in {spec!r}"
                ) from exc
            if s > e:
                raise ValueError(
                    f"Stage range start {s} > end {e} in {spec!r}"
                )
            if s < 1 or e > num_stages:
                raise ValueError(
                    f"Stage range {tok!r} out of bounds (1–{num_stages}) in {spec!r}"
                )
            return list(range(s, e + 1))
        try:
            v = int(tok)
        except ValueError as exc:
            raise ValueError(f"Invalid stage token {tok!r} in {spec!r}") from exc
        if v < 1 or v > num_stages:
            raise ValueError(
                f"Stage index {v} out of bounds (1–{num_stages}) in {spec!r}"
            )
        return [v]

    for tok in tokens:
        phases.append(_parse_token(tok))

    if has_ellipsis:
        # Determine the next stage to assign after the last explicit token.
        if phases:
            last_stage = phases[-1][-1]
        else:
            last_stage = 0
        next_stage = last_stage + 1
        while next_stage <= num_stages:
            phases.append([next_stage])
            next_stage += 1

    return phases
