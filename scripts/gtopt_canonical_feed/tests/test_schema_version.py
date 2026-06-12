# SPDX-License-Identifier: BSD-3-Clause
"""SchemaVersionError fires when the manifest's version doesn't match
the reader's expected_schema_version."""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import SCHEMA_VERSION, read_feed
from gtopt_canonical_feed.errors import SchemaVersionError


def test_schema_version_match(gold_feed_dir):
    # Match → no exception.
    topology, _cells, manifest = read_feed(
        gold_feed_dir, expected_schema_version=SCHEMA_VERSION
    )
    assert manifest.schema_version == SCHEMA_VERSION
    assert len(topology.buses) == 3


def test_schema_version_mismatch(gold_feed_dir):
    with pytest.raises(SchemaVersionError, match="schema_version"):
        read_feed(gold_feed_dir, expected_schema_version="999.0.0")


def test_no_expected_version_skips_check(gold_feed_dir):
    # When expected_schema_version is None, any version is accepted.
    topology, _cells, _manifest = read_feed(gold_feed_dir, expected_schema_version=None)
    assert len(topology.buses) == 3
