"""Transparent reading of optionally compressed and/or split files.

The implementation now lives in
:mod:`gtopt_shared.compressed_open` so every converter (plp2gtopt,
sddp2gtopt, psse2gtopt, …) shares one transparent ``.xz`` / ``.gz`` /
``.zst`` / ``.lz4`` reader with variant + split-file probing.  This
module re-exports the public API so the many existing
``from plp2gtopt.compressed_open import …`` call sites keep working.
"""

from gtopt_shared.compressed_open import (  # noqa: F401
    compressed_open,
    find_compressed_path,
    resolve_compressed_path,
)

__all__ = [
    "compressed_open",
    "find_compressed_path",
    "resolve_compressed_path",
]
