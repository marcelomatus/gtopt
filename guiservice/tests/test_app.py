"""Tests for the gtopt GUI Service application."""

import io
import json
import zipfile
from unittest.mock import MagicMock, patch

import pytest

from guiservice.app import app, _build_case_json, _build_zip, _parse_uploaded_zip


@pytest.fixture
def client():
    app.config["TESTING"] = True
    with app.test_client() as c:
        yield c


# ---------------------------------------------------------------------------
# Unit tests – helper functions
# ---------------------------------------------------------------------------


def _sample_case_data():
    return {
        "case_name": "test_case",
        "options": {
            "annual_discount_rate": 0.1,
            "output_format": "csv",
            "input_directory": "test_case",
            "input_format": "csv",
            "demand_fail_cost": 1000,
        },
        "simulation": {
            "block_array": [
                {"uid": 1, "duration": 1},
                {"uid": 2, "duration": 2},
            ],
            "stage_array": [
                {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
                {"uid": 2, "first_block": 1, "count_block": 1, "active": 1},
            ],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "bus": [
                {"uid": 1, "name": "b1"},
            ],
            "generator": [
                {"uid": 1, "name": "g1", "bus": "b1", "gcost": 100, "capacity": 20},
            ],
            "demand": [
                {"uid": 1, "name": "d1", "bus": "b1", "lmax": 10},
            ],
        },
        "data_files": {},
    }


class TestBuildCaseJson:
    def test_basic_structure(self):
        data = _sample_case_data()
        result = _build_case_json(data)

        assert "options" in result
        assert "simulation" in result
        assert "system" in result
        assert result["system"]["name"] == "test_case"

    def test_system_arrays(self):
        data = _sample_case_data()
        result = _build_case_json(data)

        assert "bus_array" in result["system"]
        assert "generator_array" in result["system"]
        assert "demand_array" in result["system"]
        assert len(result["system"]["bus_array"]) == 1
        assert result["system"]["bus_array"][0]["name"] == "b1"

    def test_empty_arrays_excluded(self):
        data = _sample_case_data()
        result = _build_case_json(data)

        assert "line_array" not in result["system"]
        assert "battery_array" not in result["system"]

    def test_null_fields_stripped(self):
        data = _sample_case_data()
        data["system"]["generator"][0]["expcap"] = None
        data["system"]["generator"][0]["expmod"] = ""
        result = _build_case_json(data)

        gen = result["system"]["generator_array"][0]
        assert "expcap" not in gen
        assert "expmod" not in gen

    def test_options_included(self):
        data = _sample_case_data()
        result = _build_case_json(data)

        assert result["options"]["annual_discount_rate"] == 0.1
        assert result["options"]["demand_fail_cost"] == 1000


class TestBuildZip:
    def test_zip_contains_json(self):
        data = _sample_case_data()
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            names = zf.namelist()
            assert "test_case.json" in names

    def test_zip_json_valid(self):
        data = _sample_case_data()
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            content = json.loads(zf.read("test_case.json"))
            assert content["system"]["name"] == "test_case"

    def test_zip_with_csv_data_file(self):
        data = _sample_case_data()
        data["data_files"]["Demand/lmax"] = {
            "columns": ["scenario", "stage", "block", "uid:1"],
            "data": [[1, 1, 1, 10], [1, 2, 2, 15]],
        }
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            names = zf.namelist()
            assert "test_case/Demand/lmax.csv" in names
            content = zf.read("test_case/Demand/lmax.csv").decode()
            assert "scenario" in content
            assert "10" in content

    def test_zip_with_parquet_data_file(self):
        data = _sample_case_data()
        data["options"]["input_format"] = "parquet"
        data["data_files"]["Demand/lmax"] = {
            "columns": ["scenario", "stage", "block", "uid:1"],
            "data": [[1, 1, 1, 10], [1, 2, 2, 15]],
        }
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            names = zf.namelist()
            assert "test_case/Demand/lmax.parquet" in names


class TestParseUploadedZip:
    def test_parse_basic_case(self):
        # Create a zip
        buf = io.BytesIO()
        case_json = {
            "options": {"annual_discount_rate": 0.1},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "stage_array": [
                    {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
                ],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "parsed_case",
                "bus_array": [{"uid": 1, "name": "b1"}],
            },
        }
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("parsed_case.json", json.dumps(case_json))
        buf.seek(0)

        result = _parse_uploaded_zip(buf.read())
        assert result["case_name"] == "parsed_case"
        assert result["options"]["annual_discount_rate"] == 0.1
        assert len(result["system"]["bus"]) == 1
        assert result["system"]["bus"][0]["name"] == "b1"

    def test_parse_with_csv_data(self):
        buf = io.BytesIO()
        case_json = {
            "options": {"input_directory": "data"},
            "system": {"name": "case_csv"},
        }
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("case_csv.json", json.dumps(case_json))
            zf.writestr(
                "data/Demand/lmax.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,10\n',
            )
        buf.seek(0)

        result = _parse_uploaded_zip(buf.read())
        assert "Demand/lmax" in result["data_files"]
        assert result["data_files"]["Demand/lmax"]["columns"][0] == "scenario"


# ---------------------------------------------------------------------------
# Integration tests – Flask routes
# ---------------------------------------------------------------------------


class TestRoutes:
    def test_index(self, client):
        resp = client.get("/")
        assert resp.status_code == 200
        assert b"gtopt Case Editor" in resp.data

    def test_schemas(self, client):
        resp = client.get("/api/schemas")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "bus" in data
        assert "generator" in data
        assert "demand" in data
        assert "line" in data
        assert data["bus"]["label"] == "Bus"

    def test_preview(self, client):
        resp = client.post(
            "/api/case/preview",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert "system" in data
        assert data["system"]["name"] == "test_case"

    def test_download(self, client):
        resp = client.post(
            "/api/case/download",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        assert resp.content_type == "application/zip"

        with zipfile.ZipFile(io.BytesIO(resp.data), "r") as zf:
            assert "test_case.json" in zf.namelist()

    def test_upload(self, client):
        buf = io.BytesIO()
        case_json = {
            "options": {"annual_discount_rate": 0.05},
            "system": {
                "name": "uploaded",
                "bus_array": [{"uid": 1, "name": "bus1"}],
                "generator_array": [
                    {"uid": 1, "name": "gen1", "bus": "bus1", "capacity": 50}
                ],
            },
        }
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("uploaded.json", json.dumps(case_json))
        buf.seek(0)

        resp = client.post(
            "/api/case/upload",
            data={"file": (buf, "uploaded.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["case_name"] == "uploaded"
        assert len(data["system"]["bus"]) == 1
        assert len(data["system"]["generator"]) == 1

    def test_upload_no_file(self, client):
        resp = client.post("/api/case/upload")
        assert resp.status_code == 400

    def test_download_no_data(self, client):
        resp = client.post("/api/case/download")
        assert resp.status_code in (400, 415)

    def test_results_upload(self, client):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("solution.csv", "obj_value,123.45\nstatus,0\n")
            zf.writestr(
                "Generator/generation_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,10\n1,2,2,20\n',
            )
        buf.seek(0)

        resp = client.post(
            "/api/results/upload",
            data={"file": (buf, "results.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["solution"]["obj_value"] == "123.45"
        assert "Generator/generation_sol.csv" in data["outputs"]


class TestRoundTrip:
    """Test that a case can be downloaded and re-uploaded without data loss."""

    def test_download_and_upload(self, client):
        original = _sample_case_data()

        # Download
        resp = client.post(
            "/api/case/download",
            data=json.dumps(original),
            content_type="application/json",
        )
        assert resp.status_code == 200

        # Upload the same ZIP
        resp2 = client.post(
            "/api/case/upload",
            data={"file": (io.BytesIO(resp.data), "test_case.zip")},
            content_type="multipart/form-data",
        )
        assert resp2.status_code == 200
        loaded = resp2.get_json()

        assert loaded["case_name"] == "test_case"
        assert loaded["options"]["annual_discount_rate"] == 0.1
        assert len(loaded["system"]["bus"]) == 1
        assert len(loaded["system"]["generator"]) == 1
        assert len(loaded["system"]["demand"]) == 1
        assert loaded["system"]["bus"][0]["name"] == "b1"


# ---------------------------------------------------------------------------
# Webservice integration tests
# ---------------------------------------------------------------------------


class TestSolveConfig:
    def test_get_config(self, client):
        resp = client.get("/api/solve/config")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "webservice_url" in data

    def test_set_config(self, client):
        resp = client.post(
            "/api/solve/config",
            data=json.dumps({"webservice_url": "http://example.com:3000"}),
            content_type="application/json",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["webservice_url"] == "http://example.com:3000"

    def test_set_config_strips_trailing_slash(self, client):
        resp = client.post(
            "/api/solve/config",
            data=json.dumps({"webservice_url": "http://example.com:3000/"}),
            content_type="application/json",
        )
        assert resp.status_code == 200
        assert resp.get_json()["webservice_url"] == "http://example.com:3000"

    def test_set_config_missing_url(self, client):
        resp = client.post(
            "/api/solve/config",
            data=json.dumps({}),
            content_type="application/json",
        )
        assert resp.status_code == 400

    def test_set_config_empty_url(self, client):
        resp = client.post(
            "/api/solve/config",
            data=json.dumps({"webservice_url": ""}),
            content_type="application/json",
        )
        assert resp.status_code == 400


class TestSolveSubmit:
    @patch("guiservice.app.http_requests.post")
    def test_submit_success(self, mock_post, client):
        mock_resp = MagicMock()
        mock_resp.status_code = 201
        mock_resp.json.return_value = {
            "token": "abc-123",
            "status": "pending",
            "message": "Job submitted successfully.",
        }
        mock_resp.raise_for_status.return_value = None
        mock_post.return_value = mock_resp

        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["token"] == "abc-123"
        assert data["status"] == "pending"

        # Verify the webservice was called with correct endpoint and fields
        call_args = mock_post.call_args
        assert "/api/jobs" in call_args[0][0]
        assert "files" in call_args[1]
        assert "data" in call_args[1]
        assert call_args[1]["data"]["systemFile"] == "test_case.json"

    @patch("guiservice.app.http_requests.post")
    def test_submit_connection_error(self, mock_post, client):
        import requests

        mock_post.side_effect = requests.ConnectionError("refused")

        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 502
        assert "Cannot connect" in resp.get_json()["error"]

    def test_submit_no_data(self, client):
        resp = client.post("/api/solve/submit")
        assert resp.status_code in (400, 415)


class TestSolveStatus:
    @patch("guiservice.app.http_requests.get")
    def test_status_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "token": "abc-123",
            "status": "completed",
            "createdAt": "2025-01-01T00:00:00Z",
            "completedAt": "2025-01-01T00:01:00Z",
            "systemFile": "test.json",
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/status/abc-123")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["status"] == "completed"
        assert data["token"] == "abc-123"

        # Verify correct webservice URL was called
        call_url = mock_get.call_args[0][0]
        assert "/api/jobs/abc-123" in call_url

    @patch("guiservice.app.http_requests.get")
    def test_status_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")

        resp = client.get("/api/solve/status/abc-123")
        assert resp.status_code == 502


class TestSolveResults:
    @patch("guiservice.app.http_requests.get")
    def test_results_success(self, mock_get, client):
        # Create a mock results ZIP
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,42.0\nstatus,0\n")
            zf.writestr(
                "output/Generator/generation_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,10\n',
            )
        buf.seek(0)

        mock_resp = MagicMock()
        mock_resp.content = buf.getvalue()
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/results/abc-123")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["solution"]["obj_value"] == "42.0"

        # Verify correct webservice URL
        call_url = mock_get.call_args[0][0]
        assert "/api/jobs/abc-123/download" in call_url

    @patch("guiservice.app.http_requests.get")
    def test_results_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")

        resp = client.get("/api/solve/results/abc-123")
        assert resp.status_code == 502


class TestSolveJobsList:
    @patch("guiservice.app.http_requests.get")
    def test_list_jobs(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "jobs": [
                {
                    "token": "job-1",
                    "status": "completed",
                    "createdAt": "2025-01-01T00:00:00Z",
                    "systemFile": "system.json",
                },
                {
                    "token": "job-2",
                    "status": "running",
                    "createdAt": "2025-01-02T00:00:00Z",
                    "systemFile": "other.json",
                },
            ]
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 200
        data = resp.get_json()
        assert len(data["jobs"]) == 2
        assert data["jobs"][0]["token"] == "job-1"

        # Verify correct webservice URL
        call_url = mock_get.call_args[0][0]
        assert "/api/jobs" in call_url

    @patch("guiservice.app.http_requests.get")
    def test_list_jobs_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")

        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 502
