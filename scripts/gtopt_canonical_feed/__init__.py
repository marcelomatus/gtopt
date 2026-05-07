# SPDX-License-Identifier: BSD-3-Clause
"""Canonical operation feed schema — the contract joining gtopt_marginal_units
(analysis) and cen2gtopt (CEN data fetcher). See
docs/scripts/gtopt_marginal_units_plan.md §3.3.3.

Producers (gtopt simulated runs, cen2gtopt) write a parquet dataset
conforming to this schema. Consumers (gtopt_marginal_units --input-kind
feed-parquet) read it. Schema is frozen at SCHEMA_VERSION until a major
version bump.
"""

from __future__ import annotations

from gtopt_canonical_feed.cells import Cells
from gtopt_canonical_feed.errors import (
    FeedHashError,
    FeedSchemaError,
    SchemaVersionError,
)
from gtopt_canonical_feed.feed_io import read_feed, verify_feed, write_feed
from gtopt_canonical_feed.manifest import Manifest
from gtopt_canonical_feed.topology import Bus, Generator, Line, Topology

SCHEMA_VERSION = "1.0.0"

__all__ = [
    "Bus",
    "Cells",
    "FeedHashError",
    "FeedSchemaError",
    "Generator",
    "Line",
    "Manifest",
    "SCHEMA_VERSION",
    "SchemaVersionError",
    "Topology",
    "read_feed",
    "verify_feed",
    "write_feed",
]
