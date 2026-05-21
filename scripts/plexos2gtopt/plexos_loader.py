"""Bundle locator + extractor for CEN PCP PLEXOS archives.

A CEN PCP "bundle" can arrive in any of these shapes:

* a directory already containing ``DBSEN_PRGDIARIO.xml`` + ``*.csv``;
* a ``DATOS{date}.zip`` archive (inner zip of the outer wrapper);
* a ``DATOS{date}.zip.xz`` (the on-disk form vendored under
  ``support/plexos/``);
* a ``PLEXOS{date}.zip`` outer wrapper (auto-unwrapped to its two
  inner zips).

This loader normalises all of them to an extracted directory and
exposes a :class:`PlexosBundle` view with stable accessor methods.

The companion to ``sddp2gtopt.psrclasses_loader`` for the PLEXOS
dialect. Symmetric design: open-with, ``.first(...)`` /
``.count(...)`` semantics, no implicit caching.
"""

from __future__ import annotations

import logging
import re
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path


logger = logging.getLogger(__name__)


#: Top-level XML object database shipped inside ``DATOS{date}.zip``.
DBSEN_FILENAME = "DBSEN_PRGDIARIO.xml"

#: Optional run-parameters XML in the same zip.
PLEXOS_PARAM_FILENAME = "PLEXOS_Param.xml"

#: Match CEN's outer wrapper convention.
_OUTER_RE = re.compile(r"^PLEXOS(\d{8})\.zip$", re.IGNORECASE)
_INNER_DATOS_RE = re.compile(r"^DATOS(\d{8})\.zip$", re.IGNORECASE)


@dataclass
class PlexosBundle:
    """An extracted PLEXOS bundle on disk.

    Attributes:
        root: Filesystem path that contains ``DBSEN_PRGDIARIO.xml``
            and the per-class CSVs.
        source: Original archive / directory path the bundle was
            located from (for diagnostics).
        _cleanup: Optional ``TemporaryDirectory`` whose lifetime owns
            the extracted files; ``None`` when the bundle was already
            a directory on disk.
    """

    root: Path
    source: Path
    _cleanup: tempfile.TemporaryDirectory[str] | None = None
    # Horizon length in days for the converted simulation.  ``1``
    # (default) keeps the legacy day-1 PCP behaviour; ``7``
    # reconstructs the full CEN PCP forward-look week as 168 hourly
    # blocks in a single stage.  Extractors that read time-varying
    # CSVs (Nod_Load, Lin_Units, Lin_Max/MinRating, Gen_Rating,
    # Hydro_WaterFlows, …) honour this via :func:`plexos_csv.read_wide`
    # / :func:`read_long`'s ``n_days`` parameter.
    n_days: int = 1

    @property
    def xml_path(self) -> Path:
        """Path to ``DBSEN_PRGDIARIO.xml`` inside this bundle."""
        return self.root / DBSEN_FILENAME

    @property
    def param_xml_path(self) -> Path:
        """Path to ``PLEXOS_Param.xml`` inside this bundle."""
        return self.root / PLEXOS_PARAM_FILENAME

    def csv_paths(self) -> list[Path]:
        """All per-class CSV files at the bundle root.

        Sub-directories (``CFdata/CPF/...``) are intentionally excluded
        from this top-level listing — they are read by name on demand.
        """
        return sorted(p for p in self.root.iterdir() if p.suffix.lower() == ".csv")

    def csv(self, name: str) -> Path:
        """Resolve a CSV by basename; raises if absent.

        Args:
            name: Basename (e.g. ``"Gen_Rating.csv"``).
        """
        path = self.root / name
        if not path.is_file():
            raise FileNotFoundError(f"{name} not found in bundle {self.source}")
        return path

    def has(self, name: str) -> bool:
        """Return ``True`` when ``name`` exists at the bundle root."""
        return (self.root / name).is_file()

    def close(self) -> None:
        """Release the extraction tempdir (no-op for directory inputs)."""
        if self._cleanup is not None:
            self._cleanup.cleanup()
            self._cleanup = None

    def __enter__(self) -> PlexosBundle:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def _unwrap_outer(zip_path: Path, dest: Path) -> Path:
    """Extract a ``PLEXOS*.zip`` outer wrapper; return the DATOS zip path."""
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dest)
    inner_datos = sorted(dest.glob("DATOS*.zip"))
    if not inner_datos:
        raise ValueError(
            f"{zip_path}: outer wrapper missing the expected DATOS*.zip payload"
        )
    return inner_datos[0]


def _extract_xz_zip(xz_path: Path, dest_dir: Path) -> Path:
    """Decompress ``*.zip.xz`` to ``dest_dir`` and return the resulting zip path."""
    try:
        import lzma  # pylint: disable=import-outside-toplevel
    except ImportError as exc:  # pragma: no cover — stdlib on all supported versions
        raise RuntimeError("lzma module unavailable; cannot extract .zip.xz") from exc

    out_zip = dest_dir / xz_path.name.removesuffix(".xz")
    with lzma.open(xz_path, "rb") as src, out_zip.open("wb") as dst:
        # 1 MiB chunks keep peak memory bounded for the 18 MB PCP files.
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            dst.write(chunk)
    return out_zip


def _extract_zip(zip_path: Path, dest_dir: Path) -> Path:
    """Extract a ``DATOS*.zip`` into ``dest_dir`` and return its path."""
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(dest_dir)
    return dest_dir


def locate_bundle(input_path: Path) -> PlexosBundle:
    """Locate or extract a PLEXOS bundle at ``input_path``.

    Accepts:
      * a directory already containing ``DBSEN_PRGDIARIO.xml``;
      * a ``DATOS{date}.zip`` archive;
      * a ``DATOS{date}.zip.xz`` archive (auto-decompressed);
      * a ``PLEXOS{date}.zip`` outer wrapper (auto-unwrapped).

    Raises:
        FileNotFoundError: If ``input_path`` does not exist or does not
            contain the expected XML schema.
        ValueError: If the archive layout is not one of the supported
            shapes.
    """
    if not input_path.exists():
        raise FileNotFoundError(f"PLEXOS bundle path not found: {input_path}")

    if input_path.is_dir():
        if not (input_path / DBSEN_FILENAME).is_file():
            raise FileNotFoundError(
                f"{input_path}: directory does not contain {DBSEN_FILENAME}"
            )
        return PlexosBundle(root=input_path, source=input_path)

    # The tempdir lifetime is owned by the returned PlexosBundle (released
    # via ``.close()`` / context-manager exit), so a ``with`` block here
    # would tear it down before the caller has a chance to read anything.
    # pylint: disable-next=consider-using-with
    tmp = tempfile.TemporaryDirectory(prefix="plexos2gtopt-")
    work = Path(tmp.name)

    # Decompress .zip.xz first if applicable.
    if input_path.suffix.lower() == ".xz" and input_path.stem.lower().endswith(".zip"):
        zip_path = _extract_xz_zip(input_path, work)
    elif input_path.suffix.lower() == ".zip":
        zip_path = input_path
    else:
        tmp.cleanup()
        raise ValueError(
            f"{input_path}: unsupported archive type (expected .zip or .zip.xz)"
        )

    # Unwrap PLEXOS outer wrapper to its DATOS inner zip when applicable.
    if _OUTER_RE.match(zip_path.name):
        unwrap_dir = work / "_outer"
        unwrap_dir.mkdir()
        inner_zip = _unwrap_outer(zip_path, unwrap_dir)
        extract_dir = work / "_inner"
        extract_dir.mkdir()
        _extract_zip(inner_zip, extract_dir)
    elif _INNER_DATOS_RE.match(zip_path.name):
        extract_dir = work / "_inner"
        extract_dir.mkdir()
        _extract_zip(zip_path, extract_dir)
    else:
        # Unknown name — still try to extract; reject if no XML lands.
        extract_dir = work / "_inner"
        extract_dir.mkdir()
        _extract_zip(zip_path, extract_dir)

    if not (extract_dir / DBSEN_FILENAME).is_file():
        tmp.cleanup()
        raise FileNotFoundError(
            f"{input_path}: extracted bundle missing {DBSEN_FILENAME}"
        )

    logger.info("extracted PLEXOS bundle %s -> %s", input_path, extract_dir)
    return PlexosBundle(root=extract_dir, source=input_path, _cleanup=tmp)
