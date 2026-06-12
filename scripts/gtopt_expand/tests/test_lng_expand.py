# -*- coding: utf-8 -*-

"""Tests for the LNG terminal expansion (lng.json → gtopt entities)."""

import json

import pytest

from gtopt_expand.cli import main as cli_main
from gtopt_expand.lng_expand import (
    _build_delivery_schedule,
    _compute_heat_rate,
    expand_lng,
    expand_lng_from_file,
    expand_terminal,
)


# ---------------------------------------------------------------------------
# Minimal fixtures
# ---------------------------------------------------------------------------
def _minimal_terminal():
    return {
        "number": 1,
        "name": "GNL_Quintero",
        "vmax": 150000.0,
        "vini": 80000.0,
        "cgnl": 5.0,
        "cver": 100.0,
        "creg": 2.5,
        "calm": 0.01,
        "gnlren": 0.90,
        "generators": [
            {"name": "NEHUENCO_1", "efficiency": 0.40},
        ],
        "deliveries": [
            {"stage": 1, "volume": 50000},
            {"stage": 3, "volume": 50000},
        ],
    }


def _minimal_config():
    return {"terminals": [_minimal_terminal()]}


# ---------------------------------------------------------------------------
# Heat rate computation
# ---------------------------------------------------------------------------
class TestHeatRate:
    def test_basic(self):
        # heat_rate = 1 / (0.90 × 0.40 × 3.6) = 1 / 1.296 ≈ 0.7716
        hr = _compute_heat_rate(0.90, 0.40)
        assert hr == pytest.approx(1.0 / (0.90 * 0.40 * 3.6))

    def test_zero_efficiency_raises(self):
        with pytest.raises(ValueError, match="must be > 0"):
            _compute_heat_rate(0.90, 0.0)

    def test_negative_gnlren_raises(self):
        with pytest.raises(ValueError, match="must be > 0"):
            _compute_heat_rate(-0.5, 0.40)


# ---------------------------------------------------------------------------
# Delivery schedule builder
# ---------------------------------------------------------------------------
class TestDeliverySchedule:
    def test_sparse_to_dense(self):
        deliveries = [
            {"stage": 1, "volume": 50000},
            {"stage": 3, "volume": 30000},
        ]
        result = _build_delivery_schedule(deliveries, num_stages=4)
        assert result == [50000.0, 0.0, 30000.0, 0.0]

    def test_out_of_range_ignored(self):
        deliveries = [
            {"stage": 0, "volume": 10000},
            {"stage": 5, "volume": 20000},
        ]
        result = _build_delivery_schedule(deliveries, num_stages=3)
        assert result == [0.0, 0.0, 0.0]

    def test_empty(self):
        result = _build_delivery_schedule([], num_stages=2)
        assert result == [0.0, 0.0]


# ---------------------------------------------------------------------------
# Single terminal expansion
# ---------------------------------------------------------------------------
class TestExpandTerminal:
    def test_basic_fields(self):
        t = _minimal_terminal()
        result = expand_terminal(t, uid=42, num_stages=4)

        assert result["uid"] == 42
        assert result["name"] == "GNL_Quintero"
        assert result["emax"] == 150000.0
        assert result["eini"] == 80000.0
        assert result["spillway_cost"] == 100.0
        assert result["use_state_variable"] is True

    def test_ecost(self):
        t = _minimal_terminal()
        result = expand_terminal(t, uid=1, num_stages=4)
        assert result["ecost"] == 0.01

    def test_zero_ecost_omitted(self):
        t = _minimal_terminal()
        t["calm"] = 0.0
        result = expand_terminal(t, uid=1, num_stages=4)
        assert "ecost" not in result

    def test_generators_heat_rate(self):
        t = _minimal_terminal()
        result = expand_terminal(t, uid=1, num_stages=4)

        gens = result["generators"]
        assert len(gens) == 1
        assert gens[0]["generator"] == "NEHUENCO_1"
        expected_hr = 1.0 / (0.90 * 0.40 * 3.6)
        assert gens[0]["heat_rate"] == pytest.approx(expected_hr)

    def test_delivery_schedule(self):
        t = _minimal_terminal()
        result = expand_terminal(t, uid=1, num_stages=4)

        assert result["delivery"] == [50000.0, 0.0, 50000.0, 0.0]

    def test_no_deliveries(self):
        t = _minimal_terminal()
        t["deliveries"] = []
        result = expand_terminal(t, uid=1, num_stages=4)
        assert "delivery" not in result

    def test_no_generators(self):
        t = _minimal_terminal()
        t["generators"] = []
        result = expand_terminal(t, uid=1, num_stages=4)
        assert "generators" not in result


# ---------------------------------------------------------------------------
# Full expand
# ---------------------------------------------------------------------------
class TestExpandLng:
    def test_single_terminal(self):
        result = expand_lng(_minimal_config(), num_stages=4)
        assert "lng_terminal_array" in result
        assert len(result["lng_terminal_array"]) == 1
        assert result["lng_terminal_array"][0]["uid"] == 1

    def test_uid_start(self):
        result = expand_lng(_minimal_config(), num_stages=4, uid_start=100)
        assert result["lng_terminal_array"][0]["uid"] == 100

    def test_empty_terminals(self):
        result = expand_lng({"terminals": []}, num_stages=4)
        assert not result["lng_terminal_array"]


# ---------------------------------------------------------------------------
# File I/O round-trip
# ---------------------------------------------------------------------------
class TestExpandFromFile:
    def test_round_trip(self, tmp_path):
        lng_path = tmp_path / "lng.json"
        lng_path.write_text(json.dumps(_minimal_config()), encoding="utf-8")

        result = expand_lng_from_file(lng_path, num_stages=4)
        assert len(result["lng_terminal_array"]) == 1
        assert result["lng_terminal_array"][0]["name"] == "GNL_Quintero"


# ---------------------------------------------------------------------------
# CLI round-trip
# ---------------------------------------------------------------------------
class TestLngCli:
    def test_cli_lng(self, tmp_path):
        lng_path = tmp_path / "lng.json"
        lng_path.write_text(json.dumps(_minimal_config()), encoding="utf-8")
        out_dir = tmp_path / "output"

        rc = cli_main(
            [
                "lng",
                "--input",
                str(lng_path),
                "--output",
                str(out_dir),
                "--num-stages",
                "4",
            ]
        )
        assert rc == 0

        entities_path = out_dir / "lng_entities.json"
        assert entities_path.exists()

        with open(entities_path, encoding="utf-8") as fh:
            entities = json.load(fh)
        assert "lng_terminal_array" in entities
        assert len(entities["lng_terminal_array"]) == 1
        assert entities["lng_terminal_array"][0]["name"] == "GNL_Quintero"

    def test_cli_lng_uid_start(self, tmp_path):
        lng_path = tmp_path / "lng.json"
        lng_path.write_text(json.dumps(_minimal_config()), encoding="utf-8")
        out_dir = tmp_path / "output2"

        rc = cli_main(
            [
                "lng",
                "--input",
                str(lng_path),
                "--output",
                str(out_dir),
                "--num-stages",
                "4",
                "--uid-start",
                "50",
            ]
        )
        assert rc == 0

        with open(out_dir / "lng_entities.json", encoding="utf-8") as fh:
            entities = json.load(fh)
        assert entities["lng_terminal_array"][0]["uid"] == 50

    def test_cli_lng_missing_input(self, tmp_path):
        rc = cli_main(
            [
                "lng",
                "--input",
                str(tmp_path / "missing.json"),
                "--output",
                str(tmp_path / "out"),
                "--num-stages",
                "4",
            ]
        )
        assert rc == 2
