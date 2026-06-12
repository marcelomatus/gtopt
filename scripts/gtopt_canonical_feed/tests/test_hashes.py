# SPDX-License-Identifier: BSD-3-Clause
"""Hash-tampering test: mutate a parquet file after write, expect
FeedHashError on read."""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import read_feed, verify_feed
from gtopt_canonical_feed.errors import FeedHashError


def test_clean_feed_passes_verification(gold_feed_dir):
    manifest = verify_feed(gold_feed_dir)
    assert "cells/dispatch.parquet" in manifest.file_hashes


def test_tampered_file_raises(gold_feed_dir):
    target = gold_feed_dir / "cells/dispatch.parquet"
    # Append a single byte — corrupts the file but keeps the path.
    with target.open("ab") as fp:
        fp.write(b"\x00")

    with pytest.raises(FeedHashError, match="sha256 mismatch"):
        read_feed(gold_feed_dir)


def test_missing_file_raises(gold_feed_dir):
    target = gold_feed_dir / "cells/dispatch.parquet"
    target.unlink()
    with pytest.raises(FeedHashError, match="missing file"):
        read_feed(gold_feed_dir)


def test_skip_verify_hashes(gold_feed_dir):
    target = gold_feed_dir / "cells/dispatch.parquet"
    with target.open("ab") as fp:
        fp.write(b"\x00")
    # With verify_hashes=False, the hash mismatch is silently ignored;
    # parquet itself may still raise on a corrupt file, so we only check
    # that the hash check doesn't fire pre-emptively.
    # With verify_hashes=False the hash check must NOT fire.
    # Downstream parquet parsing may still raise on a corrupt file —
    # that's acceptable here; we are only asserting the hash check
    # is not what fails.
    try:
        read_feed(gold_feed_dir, verify_hashes=False)
    except FeedHashError:  # pragma: no cover
        pytest.fail("FeedHashError fired despite verify_hashes=False")
    except (OSError, ValueError, RuntimeError):
        # Parquet parser fails on the corrupt body — fine for v1.
        pass
