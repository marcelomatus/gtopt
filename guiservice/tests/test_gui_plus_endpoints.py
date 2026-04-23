# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the GUI Plus extensions to guiservice/app.py."""

from __future__ import annotations

import json

import pytest

from guiservice.app import app


@pytest.fixture
def client():
    app.config["TESTING"] = True
    with app.test_client() as c:
        yield c


# ---------------------------------------------------------------------------
# /api/templates
# ---------------------------------------------------------------------------


def test_list_templates(client):
    resp = client.get("/api/templates")
    assert resp.status_code == 200
    body = resp.get_json()
    assert "templates" in body
    slugs = {t["slug"] for t in body["templates"]}
    assert "blank" in slugs


def test_get_template_blank(client):
    resp = client.get("/api/templates/blank")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["case_name"] == "blank"
    assert "bus" in body["system"]
    assert body["system"]["bus"]


def test_get_template_unknown(client):
    resp = client.get("/api/templates/this-does-not-exist")
    assert resp.status_code == 404


def test_get_template_ieee_9b_ori(client):
    resp = client.get("/api/templates/ieee_9b_ori")
    # Skip if the template file is missing in a shallow checkout.
    if resp.status_code == 404:
        pytest.skip("ieee_9b_ori template not available")
    assert resp.status_code == 200
    body = resp.get_json()
    # The case should include buses, generators, demands, lines
    assert body["system"].get("bus")
    assert body["system"].get("line")


# ---------------------------------------------------------------------------
# /api/case/validate and /api/case/check_refs
# ---------------------------------------------------------------------------


def _valid_case() -> dict:
    return {
        "case_name": "c",
        "options": {},
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [
                {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
            ],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "bus": [
                {"uid": 1, "name": "b1"},
                {"uid": 2, "name": "b2"},
            ],
            "generator": [
                {"uid": 10, "name": "g1", "bus": "b1"},
            ],
            "demand": [
                {"uid": 20, "name": "d1", "bus": "b2"},
            ],
            "line": [
                {
                    "uid": 30,
                    "name": "l1",
                    "bus_a": "b1",
                    "bus_b": "b2",
                },
            ],
        },
    }


def test_validate_ok(client):
    resp = client.post("/api/case/validate", json=_valid_case())
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["ok"] is True
    assert body["n_errors"] == 0


def test_validate_duplicate_uid(client):
    case = _valid_case()
    case["system"]["bus"].append({"uid": 1, "name": "b3"})  # duplicate uid
    resp = client.post("/api/case/validate", json=case)
    body = resp.get_json()
    assert body["ok"] is False
    types = {e["type"] for e in body["errors"]}
    assert "duplicate_uid" in types


def test_validate_dangling_ref(client):
    case = _valid_case()
    case["system"]["generator"][0]["bus"] = "nonexistent"
    resp = client.post("/api/case/validate", json=case)
    body = resp.get_json()
    assert body["ok"] is False
    types = {e["type"] for e in body["errors"]}
    assert "dangling_ref" in types


def test_validate_missing_required_ref(client):
    case = _valid_case()
    case["system"]["generator"][0].pop("bus")
    resp = client.post("/api/case/validate", json=case)
    body = resp.get_json()
    assert body["ok"] is False
    types = {e["type"] for e in body["errors"]}
    assert "missing_required_ref" in types


def test_validate_isolated_bus_warning(client):
    case = _valid_case()
    case["system"]["bus"].append({"uid": 99, "name": "b_isolated"})
    resp = client.post("/api/case/validate", json=case)
    body = resp.get_json()
    # Still ok (only a warning)
    assert body["ok"] is True
    warn_types = {w["type"] for w in body["warnings"]}
    assert "isolated_bus" in warn_types


def test_validate_empty_simulation_arrays(client):
    case = _valid_case()
    case["simulation"] = {"block_array": [], "stage_array": [], "scenario_array": []}
    resp = client.post("/api/case/validate", json=case)
    body = resp.get_json()
    warn_types = {w["type"] for w in body["warnings"]}
    assert "empty_simulation_array" in warn_types


def test_check_refs_only_returns_ref_errors(client):
    case = _valid_case()
    case["system"]["generator"][0]["bus"] = "nonexistent"
    case["system"]["bus"].append({"uid": 1, "name": "b_dup"})  # duplicate uid
    resp = client.post("/api/case/check_refs", json=case)
    body = resp.get_json()
    assert body["ok"] is False
    # check_refs should only return dangling_ref / missing_required_ref
    types = {e["type"] for e in body["errors"]}
    assert types.issubset({"dangling_ref", "missing_required_ref"})
    assert "dangling_ref" in types


# ---------------------------------------------------------------------------
# /api/results/summary
# ---------------------------------------------------------------------------


def test_results_summary(client):
    results = {
        "solution": {"obj_value": "2.0", "status": "0"},
        "outputs": {
            "Generator/generation_sol": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [[1, 1, 1, 10.0], [1, 1, 2, 20.0]],
            }
        },
    }
    resp = client.post(
        "/api/results/summary",
        json={"results": results, "scale_objective": 1000},
    )
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["total_generation"] == pytest.approx(30.0)
    assert body["obj_value"] == pytest.approx(2000.0)


def test_results_summary_empty(client):
    resp = client.post("/api/results/summary", json={"results": {}})
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["total_generation"] == 0.0


# ---------------------------------------------------------------------------
# /api/results/aggregate
# ---------------------------------------------------------------------------


def test_results_aggregate_sum(client):
    table = {
        "columns": ["scenario", "stage", "block", "v"],
        "data": [
            [1, 1, 1, 10.0],
            [1, 1, 2, 20.0],
            [1, 2, 1, 30.0],
        ],
    }
    resp = client.post(
        "/api/results/aggregate",
        json={"table": table, "group_by": ["stage"], "aggregation": "sum"},
    )
    assert resp.status_code == 200
    body = resp.get_json()
    # Two stages: stage=1 -> 30, stage=2 -> 30
    assert body["columns"] == ["stage", "scenario", "block", "v"] or "v" in body["columns"]
    stage_values = {row[body["columns"].index("stage")]: row for row in body["rows"]}
    assert len(stage_values) == 2


def test_results_aggregate_bad_aggregation(client):
    resp = client.post(
        "/api/results/aggregate",
        json={"table": {"columns": ["a"], "data": [[1]]}, "aggregation": "foo"},
    )
    assert resp.status_code == 400


def test_results_aggregate_empty_table(client):
    resp = client.post("/api/results/aggregate", json={"table": {}})
    assert resp.status_code == 400


# ---------------------------------------------------------------------------
# /api/results/export/excel
# ---------------------------------------------------------------------------


def test_results_export_excel(client):
    results = {
        "solution": {"obj_value": "1.0", "status": "0"},
        "outputs": {
            "Generator/generation_sol": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [[1, 1, 1, 5.0]],
            }
        },
    }
    resp = client.post(
        "/api/results/export/excel",
        json={"results": results, "case_name": "mycase"},
    )
    assert resp.status_code == 200
    # Must be a real xlsx payload (Zip-based)
    assert resp.data[:4] == b"PK\x03\x04"
    # content-disposition with the provided case_name
    cd = resp.headers.get("Content-Disposition", "")
    assert "mycase" in cd


# ---------------------------------------------------------------------------
# /api/diagram/topology with format=reactflow
# ---------------------------------------------------------------------------


def test_diagram_topology_reactflow(client):
    case_data = _valid_case()
    resp = client.post(
        "/api/diagram/topology",
        json={"caseData": case_data, "format": "reactflow"},
    )
    if resp.status_code == 503:
        pytest.skip("gtopt_diagram not available")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["meta"]["format"] == "reactflow"
    assert body["nodes"]
    # ReactFlow nodes must have `id`, `type`, `position`, `data`
    n0 = body["nodes"][0]
    assert set(n0.keys()) >= {"id", "type", "position", "data"}
    assert set(n0["position"].keys()) == {"x", "y"}
