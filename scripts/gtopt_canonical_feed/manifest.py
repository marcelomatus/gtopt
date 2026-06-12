# SPDX-License-Identifier: BSD-3-Clause
"""Manifest — the JSON sidecar that records producer metadata,
schema version, and sha256 of every parquet file in the dataset.

The reader verifies hashes on every open; tampered files raise
FeedHashError.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


_MANIFEST_FILENAME = "manifest.json"


@dataclass(slots=True)
class Manifest:
    producer: str
    producer_version: str
    schema_version: str
    written_at: str
    file_hashes: dict[str, str] = field(default_factory=dict)
    row_counts: dict[str, int] = field(default_factory=dict)
    extras: dict[str, object] = field(default_factory=dict)

    @classmethod
    def make(
        cls,
        producer: str,
        producer_version: str,
        schema_version: str,
        extras: Optional[dict[str, object]] = None,
    ) -> "Manifest":
        return cls(
            producer=producer,
            producer_version=producer_version,
            schema_version=schema_version,
            written_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
            file_hashes={},
            row_counts={},
            extras=extras or {},
        )

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, sort_keys=True)

    def write(self, root: Path) -> None:
        (root / _MANIFEST_FILENAME).write_text(self.to_json(), encoding="utf-8")

    @classmethod
    def read(cls, root: Path) -> "Manifest":
        text = (root / _MANIFEST_FILENAME).read_text(encoding="utf-8")
        data = json.loads(text)
        return cls(
            producer=data["producer"],
            producer_version=data["producer_version"],
            schema_version=data["schema_version"],
            written_at=data["written_at"],
            file_hashes=dict(data.get("file_hashes", {})),
            row_counts=dict(data.get("row_counts", {})),
            extras=dict(data.get("extras", {})),
        )


def sha256_of(path: Path) -> str:
    """Stream-hash a file. Returns the hex digest."""
    digest = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()
