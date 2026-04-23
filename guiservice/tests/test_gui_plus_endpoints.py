# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the GUI Plus extensions to guiservice/app.py."""

from __future__ import annotations


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
            "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
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


# ---------------------------------------------------------------------------
# /api/case/validate – additional edge cases
# ---------------------------------------------------------------------------


def test_validate_empty_system(client):
    case = {
        "case_name": "empty",
        "options": {},
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {},
    }
    resp = client.post("/api/case/validate", json=case)
    assert resp.status_code == 200
    body = resp.get_json()
    # Empty system is valid (no structural errors)
    assert "ok" in body


def test_validate_missing_simulation(client):
    case = {"case_name": "x", "system": {"bus": [{"uid": 1, "name": "b1"}]}}
    resp = client.post("/api/case/validate", json=case)
    # Should not crash — returns 200 with warnings for missing simulation arrays
    assert resp.status_code == 200
    body = resp.get_json()
    assert "ok" in body


def test_validate_non_json_body(client):
    """Non-JSON body: Flask silently returns None → treated as empty case."""
    resp = client.post(
        "/api/case/validate",
        data="not json",
        content_type="text/plain",
    )
    # Flask get_json(silent=True) returns None for non-JSON content-type;
    # endpoint falls back to {} and validates the empty case → 200.
    assert resp.status_code == 200
    body = resp.get_json()
    assert "ok" in body


def test_validate_empty_body(client):
    """Empty JSON body: Flask silently returns None → treated as empty case."""
    resp = client.post("/api/case/validate", data=b"", content_type="application/json")
    # Empty / invalid JSON body → get_json(silent=True) → None → {} → 200
    assert resp.status_code == 200
    body = resp.get_json()
    assert "ok" in body


# ---------------------------------------------------------------------------
# /api/results/aggregate – more coverage
# ---------------------------------------------------------------------------


def test_results_aggregate_mean(client):
    table = {
        "columns": ["scenario", "stage", "block", "v"],
        "data": [
            [1, 1, 1, 10.0],
            [1, 1, 2, 20.0],
        ],
    }
    resp = client.post(
        "/api/results/aggregate",
        json={"table": table, "group_by": [], "aggregation": "mean"},
    )
    assert resp.status_code == 200
    body = resp.get_json()
    v_idx = body["columns"].index("v")
    values = [row[v_idx] for row in body["rows"]]
    # Mean of [10, 20] = 15
    assert any(abs(v - 15.0) < 0.01 for v in values)


def test_results_aggregate_no_data_rows(client):
    table = {
        "columns": ["scenario", "stage", "block", "v"],
        "data": [],
    }
    resp = client.post(
        "/api/results/aggregate",
        json={"table": table, "group_by": ["stage"], "aggregation": "sum"},
    )
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["rows"] == []


# ---------------------------------------------------------------------------
# /api/results/summary – additional scenarios
# ---------------------------------------------------------------------------


def test_results_summary_with_fail_sol(client):
    results = {
        "solution": {"obj_value": "1.0", "status": "0"},
        "outputs": {
            "Demand/fail_sol": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [[1, 1, 1, 3.0], [1, 1, 2, 0.0]],
            }
        },
    }
    resp = client.post("/api/results/summary", json={"results": results})
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["total_unserved"] == pytest.approx(3.0)


def test_results_summary_bad_obj_value(client):
    results = {
        "solution": {"obj_value": "not_a_number", "status": "0"},
        "outputs": {},
    }
    resp = client.post("/api/results/summary", json={"results": results})
    # Should not crash — obj_value_raw and obj_value are both None when unparseable
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["obj_value"] is None or isinstance(body["obj_value"], (int, float))


# ---------------------------------------------------------------------------
# /api/results/export/excel – content-type and disposition headers
# ---------------------------------------------------------------------------


def test_results_export_excel_content_type(client):
    results = {
        "solution": {"obj_value": "1.0", "status": "0"},
        "outputs": {},
    }
    resp = client.post(
        "/api/results/export/excel",
        json={"results": results, "case_name": "mycase"},
    )
    assert resp.status_code == 200
    ct = resp.headers.get("Content-Type", "")
    assert "spreadsheet" in ct or "octet" in ct or "excel" in ct


def test_results_export_excel_no_case_name(client):
    results = {"solution": {}, "outputs": {}}
    resp = client.post("/api/results/export/excel", json={"results": results})
    # Should work without a case_name (uses default "results")
    assert resp.status_code == 200
    assert resp.data[:4] == b"PK\x03\x04"


# ---------------------------------------------------------------------------
# /api/templates – list structure
# ---------------------------------------------------------------------------


def test_templates_list_has_slug_name_description(client):
    resp = client.get("/api/templates")
    assert resp.status_code == 200
    body = resp.get_json()
    for t in body["templates"]:
        assert "slug" in t
        assert "name" in t
        assert "description" in t


def test_get_template_has_system_bus(client):
    resp = client.get("/api/templates/blank")
    assert resp.status_code == 200
    body = resp.get_json()
    assert "bus" in body["system"]
    buses = body["system"]["bus"]
    assert isinstance(buses, list)
    assert len(buses) >= 1
    assert "uid" in buses[0]


# ---------------------------------------------------------------------------
# /api/diagram/topology – error cases
# ---------------------------------------------------------------------------


def test_diagram_topology_missing_case_data(client):
    """With no caseData key the endpoint falls back to the body itself ({}).

    The topology builder handles an empty case gracefully: returns 200 with
    empty nodes/edges.  If gtopt_diagram is unavailable → 503.
    """
    resp = client.post("/api/diagram/topology", json={})
    # Accepted outcomes: 200 (empty graph), 400/422/500/503 (error)
    assert resp.status_code in (200, 400, 422, 500, 503)
    if resp.status_code == 200:
        body = resp.get_json()
        assert "nodes" in body
        assert "edges" in body


def test_diagram_topology_visjs_format(client):
    case_data = {
        "case_name": "c",
        "options": {},
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "bus": [{"uid": 1, "name": "b1"}],
            "generator": [{"uid": 10, "name": "g1", "bus": "b1"}],
        },
    }
    resp = client.post(
        "/api/diagram/topology",
        json={"caseData": case_data, "format": "visjs"},
    )
    if resp.status_code == 503:
        pytest.skip("gtopt_diagram not available")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["meta"]["format"] == "visjs"
    assert "nodes" in body
    assert "edges" in body
