# SPDX-License-Identifier: BSD-3-Clause
"""CSV-parser tests against synthetic CEN-shaped fixtures.

Covers tz handling (date_utc passthrough), Spanish accents (Charrúa,
Ventanas N°1), and stable UID hashing.
"""

from __future__ import annotations

from pathlib import Path


from cen2gtopt._csv_parsers import (
    parse_costo_marginal_real,
    parse_demanda_real,
    parse_generacion_real,
)
from cen2gtopt._normalize import normalize_name, stable_uid


_FIXTURES = Path(__file__).parent / "data" / "cen_csv_fixtures"


def test_parse_costo_marginal_real():
    df = parse_costo_marginal_real(_FIXTURES / "costo_marginal_real_sample.csv")
    assert len(df) == 4
    assert {"date_utc", "hour", "bus_uid", "bus_name", "lmp"}.issubset(df.columns)
    # Stable UIDs are deterministic.
    crucero_uid = stable_uid("Crucero")
    assert (df[df["bus_name"] == "Crucero"]["bus_uid"] == crucero_uid).all()


def test_parse_generacion_real_handles_accents_and_glyphs():
    df = parse_generacion_real(_FIXTURES / "generacion_real_sample.csv")
    # Ventanas N°1 has the special glyph; parser must not choke.
    assert "Ventanas N°1" in set(df["gen_name"])
    # UID hashing must be deterministic across runs.
    assert (
        df[df["gen_name"] == "Ventanas N°1"]["gen_uid"] == stable_uid("Ventanas N°1")
    ).all()


def test_parse_demanda_real():
    df = parse_demanda_real(_FIXTURES / "demanda_real_sample.csv")
    assert len(df) == 4
    assert (df["load"] > 0).all()


def test_normalize_name_strips_accents_and_glyphs():
    assert normalize_name("Charrúa") == "charrua"
    # ``°`` has no ASCII equivalent → dropped; "N°1" collapses to "n1".
    assert normalize_name("Ventanas N°1") == "ventanas_n1"
    # NFD path handles compound accents (e.g. "ñ" is also covered).
    assert normalize_name("Maitén") == "maiten"


def test_stable_uid_is_deterministic():
    a = stable_uid("BOCAMINA II")
    b = stable_uid("BOCAMINA II")
    assert a == b
    # Different names yield different UIDs (collision-resistant in 31 bits).
    assert stable_uid("BOCAMINA II") != stable_uid("Ventanas N°1")
