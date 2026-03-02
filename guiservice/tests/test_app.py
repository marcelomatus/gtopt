"""Tests for the gtopt GUI Service application."""

import io
import json
import os
import signal
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

    def test_parquet_integer_columns_preserve_types(self):
        """Verify that parquet files are written without type conversion."""
        import pandas as pd

        data = _sample_case_data()
        data["options"]["input_format"] = "parquet"
        data["data_files"]["Demand/lmax"] = {
            "columns": ["scenario", "stage", "block", "uid:1"],
            "data": [[1, 1, 1, 10], [1, 2, 2, 15]],
        }
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            parquet_bytes = zf.read("test_case/Demand/lmax.parquet")
            df = pd.read_parquet(io.BytesIO(parquet_bytes))
            # Columns should be readable; no forced type conversion
            assert len(df) == 2
            assert list(df.columns) == ["scenario", "stage", "block", "uid:1"]

    def test_build_zip_preserves_raw_parquet_bytes(self):
        """When raw parquet bytes are available, _build_zip must write them
        unchanged instead of reconstructing from JSON data."""
        import numpy as np
        import pandas as pd

        df = pd.DataFrame(
            {
                "scenario": np.array([1, 2], dtype=np.int8),
                "stage": np.array([10, 20], dtype=np.int16),
                "value": np.array([1.5, 2.5], dtype=np.float64),
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)
        original_bytes = pq_buf.getvalue()

        # Simulate an uploaded case with raw parquet bytes preserved
        from base64 import b64encode

        data = _sample_case_data()
        data["options"]["input_format"] = "parquet"
        data["data_files"]["Demand/lmax"] = {
            "columns": ["scenario", "stage", "value"],
            "data": [[1, 10, 1.5], [2, 20, 2.5]],
            "_raw_parquet_b64": b64encode(original_bytes).decode("ascii"),
        }
        buf = _build_zip(data)

        with zipfile.ZipFile(buf, "r") as zf:
            rebuilt_bytes = zf.read("test_case/Demand/lmax.parquet")
            assert rebuilt_bytes == original_bytes

    def test_upload_download_parquet_round_trip_preserves_schema(self):
        """Upload a ZIP with typed parquet, then _build_zip must produce
        identical parquet bytes preserving the original schema."""
        import numpy as np
        import pandas as pd

        df = pd.DataFrame(
            {
                "scenario": np.array([1, 2], dtype=np.int8),
                "stage": np.array([10, 20], dtype=np.int16),
                "block": np.array([100, 200], dtype=np.int32),
                "uid": np.array([1000, 2000], dtype=np.int64),
                "value": np.array([1.5, 2.5], dtype=np.float64),
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)
        original_bytes = pq_buf.getvalue()

        # Build a ZIP with the parquet
        case_json = {
            "options": {"input_directory": "test_case", "input_format": "parquet"},
            "system": {"name": "test_case"},
        }
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("test_case.json", json.dumps(case_json))
            zf.writestr("test_case/Demand/lmax.parquet", original_bytes)
        zip_buf.seek(0)

        # Parse (upload) then rebuild (download)
        parsed = _parse_uploaded_zip(zip_buf.read())
        rebuilt_buf = _build_zip(parsed)

        with zipfile.ZipFile(rebuilt_buf, "r") as zf:
            rebuilt_bytes = zf.read("test_case/Demand/lmax.parquet")
            assert rebuilt_bytes == original_bytes

            # Verify schema is preserved
            rebuilt_df = pd.read_parquet(io.BytesIO(rebuilt_bytes))
            assert rebuilt_df["scenario"].dtype == np.int8
            assert rebuilt_df["stage"].dtype == np.int16
            assert rebuilt_df["block"].dtype == np.int32
            assert rebuilt_df["uid"].dtype == np.int64
            assert rebuilt_df["value"].dtype == np.float64


class TestParseUploadedZip:
    def test_parse_basic_case(self):
        # Create a zip
        buf = io.BytesIO()
        case_json = {
            "options": {"annual_discount_rate": 0.1},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
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

    def test_parse_parquet_preserves_integer_types(self):
        """Integer columns (int8/16/32/64) must remain ints after parsing."""
        import numpy as np
        import pandas as pd

        df = pd.DataFrame(
            {
                "scenario": np.array([1, 2], dtype=np.int8),
                "stage": np.array([10, 20], dtype=np.int16),
                "block": np.array([100, 200], dtype=np.int32),
                "uid": np.array([1000, 2000], dtype=np.int64),
                "value": np.array([1.5, 2.5], dtype=np.float64),
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)
        pq_bytes = pq_buf.getvalue()

        case_json = {
            "options": {"input_directory": "sys", "input_format": "parquet"},
            "system": {"name": "case_int"},
        }
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("case_int.json", json.dumps(case_json))
            zf.writestr("sys/Demand/lmax.parquet", pq_bytes)
        zip_buf.seek(0)

        result = _parse_uploaded_zip(zip_buf.read())
        data = result["data_files"]["Demand/lmax"]
        row0 = data["data"][0]
        # Integer columns must be Python int, not float
        assert isinstance(row0[0], int), f"int8 became {type(row0[0])}"
        assert isinstance(row0[1], int), f"int16 became {type(row0[1])}"
        assert isinstance(row0[2], int), f"int32 became {type(row0[2])}"
        assert isinstance(row0[3], int), f"int64 became {type(row0[3])}"
        # Float column stays float
        assert isinstance(row0[4], float), f"float became {type(row0[4])}"


# ---------------------------------------------------------------------------
# Integration tests – Flask routes
# ---------------------------------------------------------------------------


class TestRoutes:
    def test_index(self, client):
        resp = client.get("/")
        assert resp.status_code == 200
        assert b"gtopt Case Editor" in resp.data
        assert b'id="mainNav"' in resp.data
        assert b"/static/js/app.js" in resp.data
        assert b"Quick Assistant" in resp.data

    def test_spreadsheet_libs_served_locally(self, client):
        """Spreadsheet libraries must be served from local vendor files, not CDN."""
        resp = client.get("/")
        html = resp.data
        # Local vendor references must be present (both JS and CSS)
        assert b"/static/vendor/jsuites/jsuites.js" in html
        assert b"/static/vendor/jsuites/jsuites.css" in html
        assert b"/static/vendor/jspreadsheet-ce/jspreadsheet.js" in html
        assert b"/static/vendor/jspreadsheet-ce/jspreadsheet.css" in html
        # No CDN references for jsuites or jspreadsheet
        assert b"cdn.jsdelivr.net/npm/jsuites" not in html
        assert b"cdn.jsdelivr.net/npm/jspreadsheet" not in html

    def test_assistant_has_controls(self, client):
        """The Quick Assistant card should have move, minimize and close controls."""
        resp = client.get("/")
        html = resp.data
        # Card is identifiable and has a draggable header
        assert b'id="assistantCard"' in html
        assert b'id="assistantHeader"' in html
        # Minimize and close buttons are present
        assert b"minimizeAssistant()" in html
        assert b"closeAssistant()" in html
        # Body section is wrapped for minimize toggle
        assert b'id="assistantBody"' in html
        assert b"assistant-controls" in html

    def test_schemas(self, client):
        resp = client.get("/api/schemas")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "bus" in data
        assert "generator" in data
        assert "demand" in data
        assert "line" in data
        assert data["bus"]["label"] == "Bus"

    def test_logs_endpoint(self, client):
        resp = client.get("/api/logs?lines=20")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "logs" in data
        assert isinstance(data["logs"], list)

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
                "generator_array": [{"uid": 1, "name": "gen1", "bus": "bus1", "capacity": 50}],
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

    def test_parquet_round_trip_preserves_bytes(self, client):
        """Uploaded ZIP should be preserved for passthrough submission."""
        import pandas as pd

        # Create original parquet in memory
        original_df = pd.DataFrame(
            {"scenario": [1, 1], "stage": [1, 2], "block": [1, 2], "uid:1": [10, 15]}
        )
        original_buf = io.BytesIO()
        original_df.to_parquet(original_buf, index=False)
        original_bytes = original_buf.getvalue()

        # Build a ZIP with the parquet
        case_json = {
            "options": {"input_directory": "test_case", "input_format": "parquet"},
            "simulation": {},
            "system": {"name": "test_case"},
        }
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("test_case.json", json.dumps(case_json))
            zf.writestr("test_case/Demand/lmax.parquet", original_bytes)
        zip_buf.seek(0)
        original_zip_bytes = zip_buf.getvalue()

        # Upload
        zip_buf.seek(0)
        resp = client.post(
            "/api/case/upload",
            data={"file": (zip_buf, "test_case.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        case_data = resp.get_json()

        # The original ZIP is preserved for passthrough
        assert "_uploaded_zip_b64" in case_data
        assert "_system_file" in case_data

        # Decode the stored ZIP and verify it matches
        from base64 import b64decode

        stored_zip = b64decode(case_data["_uploaded_zip_b64"])
        assert stored_zip == original_zip_bytes


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


# ---------------------------------------------------------------------------
# End-to-end webservice connection and workflow tests
# ---------------------------------------------------------------------------


class TestWebserviceEndToEnd:
    """Test the full connection and functionality of sending/receiving cases
    and simulation results to/from the webservice."""

    @patch("guiservice.app.http_requests.get")
    @patch("guiservice.app.http_requests.post")
    def test_full_solve_workflow(self, mock_post, mock_get, client):
        """End-to-end: configure → submit case → poll status → get results."""
        # Step 1: Configure webservice URL
        resp = client.post(
            "/api/solve/config",
            data=json.dumps({"webservice_url": "http://testserver:3000"}),
            content_type="application/json",
        )
        assert resp.status_code == 200
        assert resp.get_json()["webservice_url"] == "http://testserver:3000"

        # Step 2: Submit case
        mock_submit_resp = MagicMock()
        mock_submit_resp.status_code = 201
        mock_submit_resp.json.return_value = {
            "token": "workflow-token-001",
            "status": "pending",
            "message": "Job submitted successfully.",
        }
        mock_submit_resp.raise_for_status.return_value = None
        mock_post.return_value = mock_submit_resp

        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        submit_data = resp.get_json()
        assert submit_data["token"] == "workflow-token-001"
        assert submit_data["status"] == "pending"
        token = submit_data["token"]

        # Verify submit called webservice with correct URL
        call_args = mock_post.call_args
        assert "http://testserver:3000/api/jobs" == call_args[0][0]
        assert call_args[1]["data"]["systemFile"] == "test_case.json"

        # Step 3: Poll status – first pending, then running, then completed
        mock_pending = MagicMock()
        mock_pending.json.return_value = {
            "token": token,
            "status": "pending",
            "createdAt": "2025-06-01T10:00:00Z",
            "systemFile": "test_case.json",
        }
        mock_pending.raise_for_status.return_value = None

        mock_running = MagicMock()
        mock_running.json.return_value = {
            "token": token,
            "status": "running",
            "createdAt": "2025-06-01T10:00:00Z",
            "systemFile": "test_case.json",
        }
        mock_running.raise_for_status.return_value = None

        mock_completed = MagicMock()
        mock_completed.json.return_value = {
            "token": token,
            "status": "completed",
            "createdAt": "2025-06-01T10:00:00Z",
            "completedAt": "2025-06-01T10:01:30Z",
            "systemFile": "test_case.json",
        }
        mock_completed.raise_for_status.return_value = None

        # Simulate progression: pending → running → completed
        mock_get.side_effect = [mock_pending, mock_running, mock_completed]

        resp1 = client.get(f"/api/solve/status/{token}")
        assert resp1.status_code == 200
        assert resp1.get_json()["status"] == "pending"

        resp2 = client.get(f"/api/solve/status/{token}")
        assert resp2.status_code == 200
        assert resp2.get_json()["status"] == "running"

        resp3 = client.get(f"/api/solve/status/{token}")
        assert resp3.status_code == 200
        assert resp3.get_json()["status"] == "completed"

        # Verify all status calls used the correct URL
        for call in mock_get.call_args_list:
            assert call[0][0] == f"http://testserver:3000/api/jobs/{token}"

        # Step 4: Retrieve results
        mock_get.reset_mock()
        mock_get.side_effect = None
        results_zip = io.BytesIO()
        with zipfile.ZipFile(results_zip, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,99.5\nstatus,0\n")
            zf.writestr(
                "output/Generator/generation_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,15\n1,2,2,18\n',
            )
            zf.writestr(
                "output/Bus/marginal_cost_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,50.0\n1,2,2,55.0\n',
            )
        results_zip.seek(0)

        mock_results_resp = MagicMock()
        mock_results_resp.content = results_zip.getvalue()
        mock_results_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_results_resp

        resp = client.get(f"/api/solve/results/{token}")
        assert resp.status_code == 200
        results = resp.get_json()

        # Verify solution data
        assert results["solution"]["obj_value"] == "99.5"
        assert results["solution"]["status"] == "0"

        # Verify output data
        assert "Generator/generation_sol.csv" in results["outputs"]
        assert "Bus/marginal_cost_sol.csv" in results["outputs"]
        gen_data = results["outputs"]["Generator/generation_sol.csv"]
        assert gen_data["columns"] == ["scenario", "stage", "block", "uid:1"]
        assert len(gen_data["data"]) == 2

        # Verify download called correct URL
        assert mock_get.call_args[0][0] == f"http://testserver:3000/api/jobs/{token}/download"

    @patch("guiservice.app.http_requests.post")
    def test_submit_and_receive_case_zip_contents(self, mock_post, client):
        """Verify the ZIP sent to the webservice contains the correct files."""
        captured_files = {}

        def capture_post(url, files=None, data=None, timeout=None):
            # Capture the ZIP file content sent to the webservice
            if files and "file" in files:
                fname, fobj, ftype = files["file"]
                captured_files["name"] = fname
                captured_files["type"] = ftype
                captured_files["data"] = data
                fobj.seek(0)
                captured_files["content"] = fobj.read()
            resp = MagicMock()
            resp.status_code = 201
            resp.json.return_value = {
                "token": "zip-test-token",
                "status": "pending",
                "message": "OK",
            }
            resp.raise_for_status.return_value = None
            return resp

        mock_post.side_effect = capture_post

        case_data = _sample_case_data()
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(case_data),
            content_type="application/json",
        )
        assert resp.status_code == 200

        # Verify ZIP was sent
        assert captured_files["name"] == "test_case.zip"
        assert captured_files["type"] == "application/zip"
        assert captured_files["data"]["systemFile"] == "test_case.json"

        # Verify ZIP contents
        with zipfile.ZipFile(io.BytesIO(captured_files["content"]), "r") as zf:
            names = zf.namelist()
            assert "test_case.json" in names

            # Verify system JSON content
            system_json = json.loads(zf.read("test_case.json"))
            assert system_json["system"]["name"] == "test_case"
            assert "bus_array" in system_json["system"]
            assert len(system_json["system"]["bus_array"]) == 1
            assert system_json["system"]["bus_array"][0]["name"] == "b1"
            assert "generator_array" in system_json["system"]
            assert system_json["system"]["generator_array"][0]["name"] == "g1"
            assert "demand_array" in system_json["system"]
            assert system_json["system"]["demand_array"][0]["name"] == "d1"

    def test_results_parsing_with_multiple_outputs(self, client):
        """Verify results with multiple output types are correctly parsed."""
        results_zip = io.BytesIO()
        with zipfile.ZipFile(results_zip, "w") as zf:
            zf.writestr("solution.csv", "obj_value,1234.56\nstatus,0\nsolver_time,5.2\n")
            zf.writestr(
                "Generator/generation_sol.csv",
                '"scenario","stage","block","uid:1","uid:2"\n'
                "1,1,1,10,20\n1,1,2,12,22\n1,2,1,15,25\n",
            )
            zf.writestr(
                "Bus/marginal_cost_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,50.0\n1,1,2,52.0\n1,2,1,48.0\n',
            )
            zf.writestr(
                "Demand/demand_sol.csv",
                '"scenario","stage","block","uid:1"\n1,1,1,10\n1,1,2,10\n1,2,1,10\n',
            )
        results_zip.seek(0)

        resp = client.post(
            "/api/results/upload",
            data={"file": (results_zip, "results.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        data = resp.get_json()

        # Verify solution metadata
        assert data["solution"]["obj_value"] == "1234.56"
        assert data["solution"]["status"] == "0"
        assert data["solution"]["solver_time"] == "5.2"

        # Verify multiple output files parsed
        assert "Generator/generation_sol.csv" in data["outputs"]
        assert "Bus/marginal_cost_sol.csv" in data["outputs"]
        assert "Demand/demand_sol.csv" in data["outputs"]

        # Verify generator output has multiple columns and rows
        gen = data["outputs"]["Generator/generation_sol.csv"]
        assert gen["columns"] == ["scenario", "stage", "block", "uid:1", "uid:2"]
        assert len(gen["data"]) == 3

        # Verify bus marginal cost output
        bus = data["outputs"]["Bus/marginal_cost_sol.csv"]
        assert bus["columns"] == ["scenario", "stage", "block", "uid:1"]
        assert len(bus["data"]) == 3

    @patch("guiservice.app.http_requests.post")
    def test_submit_with_data_files(self, mock_post, client):
        """Verify cases with CSV data files are correctly transmitted."""
        captured_content = {}

        def capture_post(url, files=None, data=None, timeout=None):
            if files and "file" in files:
                fname, fobj, ftype = files["file"]
                fobj.seek(0)
                captured_content["zip"] = fobj.read()
            resp = MagicMock()
            resp.status_code = 201
            resp.json.return_value = {"token": "t1", "status": "pending", "message": "OK"}
            resp.raise_for_status.return_value = None
            return resp

        mock_post.side_effect = capture_post

        case_data = _sample_case_data()
        case_data["data_files"]["Demand/lmax"] = {
            "columns": ["scenario", "stage", "block", "uid:1"],
            "data": [[1, 1, 1, 10], [1, 2, 2, 15]],
        }

        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(case_data),
            content_type="application/json",
        )
        assert resp.status_code == 200

        # Verify the ZIP sent includes data files
        with zipfile.ZipFile(io.BytesIO(captured_content["zip"]), "r") as zf:
            names = zf.namelist()
            assert "test_case.json" in names
            assert "test_case/Demand/lmax.csv" in names

            # Verify CSV data content
            csv_content = zf.read("test_case/Demand/lmax.csv").decode()
            assert "scenario" in csv_content
            assert "10" in csv_content

    @patch("guiservice.app.http_requests.post")
    @patch("guiservice.app.http_requests.get")
    def test_webservice_timeout_handling(self, mock_get, mock_post, client):
        """Verify proper handling of timeout during each stage."""
        import requests

        # Timeout on submit
        mock_post.side_effect = requests.Timeout("timed out")
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()

        # Timeout on status poll
        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/status/some-token")
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()

        # Timeout on results retrieval
        resp = client.get("/api/solve/results/some-token")
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()

        # Timeout on jobs list
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()

    @patch("guiservice.app.http_requests.post")
    @patch("guiservice.app.http_requests.get")
    def test_webservice_http_error_handling(self, mock_get, mock_post, client):
        """Verify proper error propagation from webservice HTTP errors."""
        import requests

        # HTTP error on submit (e.g., 400 Bad Request)
        error_resp = MagicMock()
        error_resp.status_code = 400
        error_resp.text = "Bad Request"
        error_resp.json.return_value = {"error": "Missing systemFile"}
        http_error = requests.HTTPError(response=error_resp)
        mock_post.side_effect = http_error

        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 502
        assert "Missing systemFile" in resp.get_json()["error"]

        # HTTP error on status (e.g., 404 Not Found)
        error_resp_404 = MagicMock()
        error_resp_404.status_code = 404
        http_error_404 = requests.HTTPError(response=error_resp_404)
        mock_get.side_effect = http_error_404

        resp = client.get("/api/solve/status/invalid-token")
        assert resp.status_code == 502
        assert "404" in resp.get_json()["error"]

        # HTTP error on results (e.g., 409 Conflict – job not finished)
        error_resp_409 = MagicMock()
        error_resp_409.status_code = 409
        http_error_409 = requests.HTTPError(response=error_resp_409)
        mock_get.side_effect = http_error_409

        resp = client.get("/api/solve/results/some-token")
        assert resp.status_code == 502
        assert "409" in resp.get_json()["error"]


class TestPingWebservice:
    @patch("guiservice.app.http_requests.get")
    def test_ping_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "status": "ok",
            "service": "gtopt-webservice",
            "gtopt_bin": "/usr/local/bin/gtopt",
            "gtopt_version": "gtopt 1.0",
            "log_file": "/tmp/gtopt-webservice.log",
            "timestamp": "2025-01-01T00:00:00Z",
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/ping")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["status"] == "ok"
        assert data["gtopt_version"] == "gtopt 1.0"

    @patch("guiservice.app.http_requests.get")
    def test_ping_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502
        assert "Cannot connect" in resp.get_json()["error"]

    @patch("guiservice.app.http_requests.get")
    def test_ping_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()


class TestWebserviceLogs:
    @patch("guiservice.app.http_requests.get")
    def test_logs_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "log_file": "/tmp/gtopt-webservice.log",
            "lines": ["[INFO] test log line 1", "[INFO] test log line 2"],
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/logs?lines=100")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "lines" in data
        assert len(data["lines"]) == 2

    @patch("guiservice.app.http_requests.get")
    def test_logs_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 502
        assert "Cannot connect" in resp.get_json()["error"]


class TestJobLogs:
    @patch("guiservice.app.http_requests.get")
    def test_job_logs_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "token": "abc-123",
            "status": "completed",
            "stdout": "Solving model...\nOptimal solution found.\n",
            "stderr": "",
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/job_logs/abc-123")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["token"] == "abc-123"
        assert "Solving model" in data["stdout"]

        # Verify correct webservice URL was called
        call_url = mock_get.call_args[0][0]
        assert "/api/jobs/abc-123/logs" in call_url

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/job_logs/abc-123")
        assert resp.status_code == 502
        assert "Cannot connect" in resp.get_json()["error"]

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_not_found(self, mock_get, client):
        import requests

        mock_resp = MagicMock()
        mock_resp.status_code = 404
        mock_resp.raise_for_status.side_effect = requests.HTTPError(response=mock_resp)
        mock_get.return_value = mock_resp
        resp = client.get("/api/solve/job_logs/nonexistent")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/job_logs/abc-123")
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()


class TestResultsWithTerminalOutput:
    def test_results_zip_with_terminal_log(self):
        """Verify _parse_results_zip extracts terminal output."""
        from guiservice.app import _parse_results_zip

        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,42.0\nstatus,0\n")
            zf.writestr(
                "output/gtopt_terminal.log",
                "Solving model...\nOptimal solution found.\n",
            )
        buf.seek(0)

        results = _parse_results_zip(buf.getvalue())
        assert "terminal_output" in results
        assert "Solving model" in results["terminal_output"]
        assert "Optimal solution found" in results["terminal_output"]
        assert results["solution"]["obj_value"] == "42.0"

    def test_results_zip_with_stdout_stderr_logs(self):
        """Verify _parse_results_zip extracts stdout.log and stderr.log."""
        from guiservice.app import _parse_results_zip

        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("stdout.log", "iteration 1\niteration 2\n")
            zf.writestr("stderr.log", "warning: something\n")
        buf.seek(0)

        results = _parse_results_zip(buf.getvalue())
        assert "iteration 1" in results["terminal_output"]
        assert "warning: something" in results["terminal_output"]

    def test_results_zip_without_terminal_output(self):
        """Verify _parse_results_zip works when no log files are present."""
        from guiservice.app import _parse_results_zip

        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,10.0\nstatus,0\n")
        buf.seek(0)

        results = _parse_results_zip(buf.getvalue())
        assert results["terminal_output"] == ""
        assert results["solution"]["obj_value"] == "10.0"

    @patch("guiservice.app.http_requests.get")
    def test_results_endpoint_includes_terminal_output(self, mock_get, client):
        """Verify /api/solve/results includes terminal output in response."""
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,42.0\nstatus,0\n")
            zf.writestr(
                "output/gtopt_terminal.log",
                "gtopt solver output line 1\ngtopt solver output line 2\n",
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
        assert "terminal_output" in data
        assert "gtopt solver output line 1" in data["terminal_output"]

    def test_results_upload_includes_terminal_output(self, client):
        """Verify /api/results/upload includes terminal output."""
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("output/solution.csv", "obj_value,99.9\nstatus,0\n")
            zf.writestr("stdout.log", "Starting optimization...\nDone.\n")
        buf.seek(0)

        resp = client.post(
            "/api/results/upload",
            data={"file": (buf, "results.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert "terminal_output" in data
        assert "Starting optimization" in data["terminal_output"]


class TestWebserviceConnectivity:
    """Tests verifying the guiservice can properly connect to webservice endpoints."""

    @patch("guiservice.app.http_requests.get")
    def test_ping_returns_all_expected_fields(self, mock_get, client):
        """Ping response should contain all service metadata fields."""
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "status": "ok",
            "service": "gtopt-webservice",
            "gtopt_bin": "/usr/bin/gtopt",
            "gtopt_version": "gtopt 2.0",
            "log_file": "/var/log/gtopt.log",
            "timestamp": "2025-06-01T00:00:00Z",
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/ping")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["status"] == "ok"
        assert data["service"] == "gtopt-webservice"
        assert data["gtopt_bin"] == "/usr/bin/gtopt"
        assert data["gtopt_version"] == "gtopt 2.0"
        assert data["log_file"] == "/var/log/gtopt.log"
        assert "timestamp" in data

    @patch("guiservice.app.http_requests.get")
    def test_logs_returns_correct_structure(self, mock_get, client):
        """Webservice logs response should have log_file and lines."""
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "log_file": "/var/log/gtopt-webservice.log",
            "lines": ["[INFO] line 1", "[INFO] line 2"],
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/logs?lines=50")
        assert resp.status_code == 200
        data = resp.get_json()
        assert "log_file" in data
        assert "lines" in data
        assert isinstance(data["lines"], list)

        # Verify correct params forwarded
        call_kwargs = mock_get.call_args[1]
        assert call_kwargs.get("params", {}).get("lines") == 50

    @patch("guiservice.app.http_requests.get")
    def test_jobs_list_returns_correct_structure(self, mock_get, client):
        """Jobs listing should return an array of job objects."""
        mock_resp = MagicMock()
        mock_resp.json.return_value = {
            "jobs": [
                {
                    "token": "abc-123",
                    "status": "completed",
                    "createdAt": "2025-01-01T00:00:00Z",
                    "systemFile": "case.json",
                },
                {
                    "token": "def-456",
                    "status": "running",
                    "createdAt": "2025-01-01T01:00:00Z",
                    "systemFile": "case2.json",
                },
            ]
        }
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 200
        data = resp.get_json()
        assert len(data["jobs"]) == 2
        assert data["jobs"][0]["token"] == "abc-123"
        assert data["jobs"][1]["status"] == "running"

    @patch("guiservice.app.http_requests.get")
    def test_webservice_http_error_reports_status(self, mock_get, client):
        """HTTP errors from the webservice should be reported correctly."""
        import requests

        mock_resp = MagicMock()
        mock_resp.status_code = 404
        mock_resp.raise_for_status.side_effect = requests.HTTPError(response=mock_resp)
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502
        data = resp.get_json()
        assert "error" in data
        assert "404" in data["error"]


class TestCheckServer:
    """Tests for the /api/check_server endpoint."""

    @patch("guiservice.app.http_requests.get")
    def test_check_server_all_ok(self, mock_get, client):
        """All three checks (ping, logs, jobs) succeed."""

        def side_effect(url, **kwargs):
            resp = MagicMock()
            resp.raise_for_status.return_value = None
            if "/api/ping" in url:
                resp.json.return_value = {
                    "status": "ok",
                    "service": "gtopt-webservice",
                    "gtopt_bin": "/usr/bin/gtopt",
                    "gtopt_version": "1.0",
                    "log_file": "/tmp/ws.log",
                    "timestamp": "2025-01-01T00:00:00Z",
                }
            elif "/api/logs" in url:
                resp.json.return_value = {
                    "log_file": "/tmp/ws.log",
                    "lines": ["[INFO] line1"],
                }
            elif "/api/jobs" in url:
                resp.json.return_value = {"jobs": []}
            return resp

        mock_get.side_effect = side_effect

        resp = client.get("/api/check_server")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["ping"]["status"] == "ok"
        assert data["logs"]["status"] == "ok"
        assert data["jobs"]["status"] == "ok"

    @patch("guiservice.app.http_requests.get")
    def test_check_server_ping_fails(self, mock_get, client):
        """Ping fails but logs and jobs succeed."""
        import requests

        def side_effect(url, **kwargs):
            if "/api/ping" in url:
                raise requests.ConnectionError("refused")
            resp = MagicMock()
            resp.raise_for_status.return_value = None
            if "/api/logs" in url:
                resp.json.return_value = {"log_file": "", "lines": []}
            elif "/api/jobs" in url:
                resp.json.return_value = {"jobs": []}
            return resp

        mock_get.side_effect = side_effect

        resp = client.get("/api/check_server")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["ping"]["status"] == "error"
        assert data["logs"]["status"] == "ok"
        assert data["jobs"]["status"] == "ok"

    @patch("guiservice.app.http_requests.get")
    def test_check_server_all_fail(self, mock_get, client):
        """All three checks fail."""
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")

        resp = client.get("/api/check_server")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["ping"]["status"] == "error"
        assert data["logs"]["status"] == "error"
        assert data["jobs"]["status"] == "error"


class TestShutdownEndpoint:
    """Tests for the /api/shutdown endpoint."""

    @patch("os.kill")
    def test_shutdown_returns_status(self, mock_kill, client):
        """Shutdown endpoint should return shutting_down status."""
        resp = client.post("/api/shutdown")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["status"] == "shutting_down"
        mock_kill.assert_called_once_with(os.getpid(), signal.SIGTERM)

    def test_shutdown_rejects_get(self, client):
        """Shutdown endpoint should only accept POST."""
        resp = client.get("/api/shutdown")
        assert resp.status_code == 405


class TestPingWebserviceSavesConfig:
    """Verify that ping and logs proxy correctly through the guiservice."""

    @patch("guiservice.app.http_requests.get")
    def test_ping_uses_configured_url(self, mock_get, client):
        """Ping should use the configured webservice URL."""
        # First set a custom URL
        client.post(
            "/api/solve/config",
            json={"webservice_url": "http://myhost:4000"},
        )

        mock_resp = MagicMock()
        mock_resp.json.return_value = {"status": "ok", "service": "gtopt-webservice"}
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/ping")
        assert resp.status_code == 200

        # Verify it called the custom URL
        call_url = mock_get.call_args[0][0]
        assert "myhost:4000" in call_url
        assert "/api/ping" in call_url

    @patch("guiservice.app.http_requests.get")
    def test_logs_uses_configured_url(self, mock_get, client):
        """Webservice logs should use the configured webservice URL."""
        client.post(
            "/api/solve/config",
            json={"webservice_url": "http://myhost:4000"},
        )

        mock_resp = MagicMock()
        mock_resp.json.return_value = {"log_file": "/tmp/ws.log", "lines": ["line1"]}
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp

        resp = client.get("/api/solve/logs?lines=50")
        assert resp.status_code == 200

        call_url = mock_get.call_args[0][0]
        assert "myhost:4000" in call_url
        assert "/api/logs" in call_url


# ---------------------------------------------------------------------------
# Additional tests for uncovered lines
# ---------------------------------------------------------------------------


class TestParseUploadedZipNoJson:
    """_parse_uploaded_zip returns defaults when ZIP has no JSON file."""

    def test_no_json_in_zip_returns_defaults(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("readme.txt", "hello")
        buf.seek(0)
        result = _parse_uploaded_zip(buf.read())
        assert result["case_name"] == "uploaded_case"
        assert result["options"] == {}
        assert result["system"] == {}


class TestDownloadCase:
    """Tests for /api/case/download endpoint."""

    def test_download_case_returns_zip(self, client):
        resp = client.post(
            "/api/case/download",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        assert resp.content_type == "application/zip"

    def test_download_case_no_data(self, client):
        resp = client.post("/api/case/download", data="", content_type="application/json")
        assert resp.status_code in (400, 415)

    def test_download_case_filename(self, client):
        data = _sample_case_data()
        resp = client.post(
            "/api/case/download",
            data=json.dumps(data),
            content_type="application/json",
        )
        assert resp.status_code == 200
        assert "test_case.zip" in resp.headers.get("Content-Disposition", "")


class TestUploadCaseErrors:
    """Tests for /api/case/upload error paths."""

    def test_no_file_uploaded(self, client):
        resp = client.post("/api/case/upload")
        assert resp.status_code == 400
        assert resp.get_json()["error"] == "No file uploaded"

    def test_empty_filename(self, client):
        data = {"file": (io.BytesIO(b""), "")}
        resp = client.post(
            "/api/case/upload",
            data=data,
            content_type="multipart/form-data",
        )
        assert resp.status_code == 400
        assert resp.get_json()["error"] == "No file selected"

    def test_non_zip_file_rejected(self, client):
        data = {"file": (io.BytesIO(b"hello"), "test.txt")}
        resp = client.post(
            "/api/case/upload",
            data=data,
            content_type="multipart/form-data",
        )
        assert resp.status_code == 400
        assert resp.get_json()["error"] == "Only ZIP files are accepted"

    def test_valid_zip_accepted(self, client):
        buf = io.BytesIO()
        case_json = {"options": {}, "simulation": {}, "system": {"name": "c"}}
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr("c.json", json.dumps(case_json))
        buf.seek(0)
        data = {"file": (buf, "c.zip")}
        resp = client.post(
            "/api/case/upload",
            data=data,
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200


class TestPreviewCase:
    """Tests for /api/case/preview endpoint."""

    def test_preview_returns_json(self, client):
        resp = client.post(
            "/api/case/preview",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 200
        data = resp.get_json()
        assert "options" in data
        assert "system" in data

    def test_preview_no_data(self, client):
        resp = client.post("/api/case/preview", data="", content_type="application/json")
        assert resp.status_code in (400, 415)


class TestUploadResultsErrors:
    """Tests for /api/results/upload error paths."""

    def test_no_file_uploaded(self, client):
        resp = client.post("/api/results/upload")
        assert resp.status_code == 400
        assert resp.get_json()["error"] == "No file uploaded"

    def test_empty_filename(self, client):
        data = {"file": (io.BytesIO(b""), "")}
        resp = client.post(
            "/api/results/upload",
            data=data,
            content_type="multipart/form-data",
        )
        assert resp.status_code == 400
        assert resp.get_json()["error"] == "No file selected"


class TestWebserviceProxyErrors:
    """Test ConnectionError / Timeout / HTTPError paths for all proxy routes."""

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
        err = resp.get_json()["error"]
        assert "Is the webservice running?" in err

    @patch("guiservice.app.http_requests.post")
    def test_submit_timeout(self, mock_post, client):
        import requests

        mock_post.side_effect = requests.Timeout("timed out")
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.post")
    def test_submit_http_error(self, mock_post, client):
        import requests

        mock_resp = MagicMock()
        mock_resp.status_code = 500
        mock_resp.json.return_value = {"error": "internal server error"}
        mock_post.side_effect = requests.HTTPError(response=mock_resp)
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_list_jobs_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_ping_connection_error_hint(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502
        err = resp.get_json()["error"]
        assert "Is the webservice running?" in err

    @patch("guiservice.app.http_requests.get")
    def test_ping_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_ping_http_error(self, mock_get, client):
        import requests

        mock_resp = MagicMock()
        mock_resp.status_code = 503
        mock_get.side_effect = requests.HTTPError(response=mock_resp)
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_jobs_connection_error_hint(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 502
        err = resp.get_json()["error"]
        assert "Is the webservice running?" in err

    @patch("guiservice.app.http_requests.get")
    def test_status_connection_error_hint(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/status/abc-token")
        assert resp.status_code == 502
        err = resp.get_json()["error"]
        assert "Is the webservice running?" in err

    @patch("guiservice.app.http_requests.get")
    def test_results_connection_error_hint(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/results/abc-token")
        assert resp.status_code == 502
        err = resp.get_json()["error"]
        assert "Is the webservice running?" in err


# ---------------------------------------------------------------------------
# Error-path tests for Timeout / HTTPError / generic Exception handlers
# ---------------------------------------------------------------------------


class TestSolveErrorPaths:
    """Cover Timeout, HTTPError, and generic Exception paths in all proxy endpoints."""

    # ---- submit_solve ----

    @patch("guiservice.app.http_requests.post")
    def test_submit_timeout(self, mock_post, client):
        import requests

        mock_post.side_effect = requests.Timeout("timed out")
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 504
        assert "timed out" in resp.get_json()["error"].lower()

    @patch("guiservice.app.http_requests.post")
    def test_submit_http_error(self, mock_post, client):
        import requests

        http_err = requests.HTTPError("server error")
        mock_resp = MagicMock()
        mock_resp.status_code = 500
        mock_resp.text = "Internal Server Error"
        mock_resp.json.return_value = {"error": "server crashed"}
        http_err.response = mock_resp
        mock_post.side_effect = http_err
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.post")
    def test_submit_generic_exception(self, mock_post, client):
        mock_post.side_effect = RuntimeError("unexpected")
        resp = client.post(
            "/api/solve/submit",
            data=json.dumps(_sample_case_data()),
            content_type="application/json",
        )
        assert resp.status_code == 500
        assert "unexpected" in resp.get_json()["error"].lower()

    # ---- get_solve_status ----

    @patch("guiservice.app.http_requests.get")
    def test_status_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/status/tok-1")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_status_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("not found")
        mock_resp = MagicMock()
        mock_resp.status_code = 404
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/status/tok-1")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_status_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/status/tok-1")
        assert resp.status_code == 500

    # ---- get_solve_results ----

    @patch("guiservice.app.http_requests.get")
    def test_results_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/results/tok-1")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_results_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("not found")
        mock_resp = MagicMock()
        mock_resp.status_code = 404
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/results/tok-1")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_results_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/results/tok-1")
        assert resp.status_code == 500

    # ---- list_solve_jobs ----

    @patch("guiservice.app.http_requests.get")
    def test_list_jobs_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_list_jobs_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("server error")
        mock_resp = MagicMock()
        mock_resp.status_code = 503
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_list_jobs_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/jobs")
        assert resp.status_code == 500

    # ---- ping_webservice ----

    @patch("guiservice.app.http_requests.get")
    def test_ping_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {"status": "ok", "gtopt_version": "1.0"}
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 200
        assert resp.get_json()["status"] == "ok"

    @patch("guiservice.app.http_requests.get")
    def test_ping_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502
        assert "Is the webservice running?" in resp.get_json()["error"]

    @patch("guiservice.app.http_requests.get")
    def test_ping_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_ping_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("server error")
        mock_resp = MagicMock()
        mock_resp.status_code = 500
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_ping_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/ping")
        assert resp.status_code == 500

    # ---- get_webservice_logs ----

    @patch("guiservice.app.http_requests.get")
    def test_webservice_logs_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {"logs": ["line 1", "line 2"]}
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 200

    @patch("guiservice.app.http_requests.get")
    def test_webservice_logs_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_webservice_logs_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_webservice_logs_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("server error")
        mock_resp = MagicMock()
        mock_resp.status_code = 500
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_webservice_logs_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/logs")
        assert resp.status_code == 500

    # ---- get_job_logs ----

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_success(self, mock_get, client):
        mock_resp = MagicMock()
        mock_resp.json.return_value = {"stdout": "all good\n", "stderr": ""}
        mock_resp.raise_for_status.return_value = None
        mock_get.return_value = mock_resp
        resp = client.get("/api/solve/job_logs/tok-1")
        assert resp.status_code == 200

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_connection_error(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.ConnectionError("refused")
        resp = client.get("/api/solve/job_logs/tok-1")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_timeout(self, mock_get, client):
        import requests

        mock_get.side_effect = requests.Timeout("timed out")
        resp = client.get("/api/solve/job_logs/tok-1")
        assert resp.status_code == 504

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_http_error(self, mock_get, client):
        import requests

        http_err = requests.HTTPError("not found")
        mock_resp = MagicMock()
        mock_resp.status_code = 404
        http_err.response = mock_resp
        mock_get.side_effect = http_err
        resp = client.get("/api/solve/job_logs/tok-1")
        assert resp.status_code == 502

    @patch("guiservice.app.http_requests.get")
    def test_job_logs_generic_exception(self, mock_get, client):
        mock_get.side_effect = RuntimeError("unexpected")
        resp = client.get("/api/solve/job_logs/tok-1")
        assert resp.status_code == 500


# ---------------------------------------------------------------------------
# NaN / non-finite float handling in parquet → JSON
# ---------------------------------------------------------------------------


class TestParquetNanHandling:
    """Verify that NaN and Inf values in parquet files are converted to
    ``null`` in JSON responses instead of the non-standard ``NaN`` token."""

    def test_parse_results_zip_parquet_with_nan(self):
        """_parse_results_zip must replace NaN with None for valid JSON."""
        import numpy as np
        import pandas as pd

        from guiservice.app import _parse_results_zip

        df = pd.DataFrame(
            {
                "scenario": [1, 1, 1],
                "stage": [1, 1, 1],
                "block": [1, 2, 3],
                "uid:1": [10.0, float("nan"), 30.0],
                "uid:2": [np.nan, 0.0, np.nan],
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)

        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("output/Generator/generation_sol.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        results = _parse_results_zip(zip_buf.getvalue())
        output = results["outputs"]["Generator/generation_sol"]
        # NaN values should have been replaced with None
        row0 = output["data"][0]
        assert row0[3] == 10.0
        assert row0[4] is None  # was np.nan

        row1 = output["data"][1]
        assert row1[3] is None  # was float('nan')
        assert row1[4] == 0.0

        row2 = output["data"][2]
        assert row2[3] == 30.0
        assert row2[4] is None

    def test_parse_results_zip_parquet_nan_produces_valid_json(self, client):
        """Upload results with NaN parquet data; response must be valid JSON."""
        import numpy as np
        import pandas as pd

        df = pd.DataFrame(
            {
                "scenario": [1, 1],
                "stage": [1, 1],
                "block": [1, 2],
                "uid:1": [0.0, float("nan")],
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)

        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("output/Demand/fail_sol.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        resp = client.post(
            "/api/results/upload",
            data={"file": (zip_buf, "results.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200
        # Must parse as valid JSON without errors
        data = json.loads(resp.data)
        output = data["outputs"]["Demand/fail_sol"]
        assert output["data"][0][3] == 0.0
        assert output["data"][1][3] is None

    def test_parse_uploaded_zip_parquet_with_nan(self):
        """_parse_uploaded_zip must replace NaN with None in data files."""
        import numpy as np
        import pandas as pd

        df = pd.DataFrame(
            {
                "scenario": [1],
                "stage": [1],
                "block": [1],
                "uid:1": [np.nan],
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)

        case_json = {
            "options": {"input_directory": "mycase", "input_format": "parquet"},
            "system": {"name": "mycase"},
        }
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("mycase.json", json.dumps(case_json))
            zf.writestr("mycase/Demand/lmax.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        result = _parse_uploaded_zip(zip_buf.read())
        row0 = result["data_files"]["Demand/lmax"]["data"][0]
        assert row0[3] is None  # NaN → None

    def test_infinity_replaced_with_none(self):
        """Infinity and -Infinity must also become None for valid JSON."""
        import numpy as np
        import pandas as pd

        from guiservice.app import _parse_results_zip

        df = pd.DataFrame(
            {
                "scenario": [1, 1],
                "block": [1, 2],
                "value": [float("inf"), float("-inf")],
            }
        )
        pq_buf = io.BytesIO()
        df.to_parquet(pq_buf, index=False)

        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("output/Test/values.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        results = _parse_results_zip(zip_buf.getvalue())
        rows = results["outputs"]["Test/values"]["data"]
        assert rows[0][2] is None  # +Inf → None
        assert rows[1][2] is None  # -Inf → None

    def test_arrow_null_bitmap_produces_none(self):
        """Arrow null bitmap (as produced by the C++ solver) must become None.

        The C++ ``flat_helper`` marks missing solver entries using Arrow's
        null bitmap.  When Python reads the parquet file, pandas converts
        these to ``NaN``.  The guiservice must turn them into ``None``
        (JSON ``null``) — matching how Arrow's CSV writer outputs empty
        strings for the same nulls.
        """
        import pyarrow as pa
        import pyarrow.parquet as pq

        from guiservice.app import _parse_results_zip

        # Simulate what the C++ code produces: float64 array with null bitmap
        scenario = pa.array([1, 1, 1], type=pa.int32())
        stage = pa.array([1, 1, 1], type=pa.int32())
        block = pa.array([1, 2, 3], type=pa.int32())
        # uid:1 has value only at block 1; blocks 2-3 are null (missing)
        uid1 = pa.array([42.0, None, None], type=pa.float64())
        # uid:2 has values at all blocks
        uid2 = pa.array([1.0, 2.0, 3.0], type=pa.float64())

        table = pa.table(
            {
                "Scenario": scenario,
                "Stage": stage,
                "Block": block,
                "uid:1": uid1,
                "uid:2": uid2,
            }
        )

        pq_buf = io.BytesIO()
        pq.write_table(table, pq_buf)

        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            zf.writestr("output/Generator/generation_sol.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        results = _parse_results_zip(zip_buf.getvalue())
        output = results["outputs"]["Generator/generation_sol"]

        # Prelude int32 columns must stay as integers
        assert output["data"][0][0] == 1  # Scenario
        assert isinstance(output["data"][0][0], int)

        # uid:1 — null entries become None (JSON null)
        assert output["data"][0][3] == 42.0  # valid value
        assert output["data"][1][3] is None  # Arrow null → None
        assert output["data"][2][3] is None  # Arrow null → None

        # uid:2 — all valid
        assert output["data"][0][4] == 1.0
        assert output["data"][1][4] == 2.0
        assert output["data"][2][4] == 3.0

    def test_csv_and_parquet_null_consistency(self, client):
        """CSV empty strings and parquet nulls both produce valid JSON.

        When the C++ solver writes missing values:
        - CSV format: Arrow writes empty strings → csv.reader reads ""
        - Parquet format: Arrow writes null bitmap → pandas reads NaN
          → _sanitize_value converts to None (JSON null)

        Both must produce valid JSON responses from the results upload
        endpoint.
        """
        import pyarrow as pa
        import pyarrow.parquet as pq

        # Build a results ZIP with both CSV and parquet outputs
        zip_buf = io.BytesIO()
        with zipfile.ZipFile(zip_buf, "w") as zf:
            # CSV with empty strings for missing values (as Arrow CSV writer
            # produces)
            zf.writestr(
                "output/Generator/generation_sol.csv",
                '"Scenario","Stage","Block","uid:1","uid:2"\n'
                "1,1,1,42.0,1.0\n"
                "1,1,2,,2.0\n"
                "1,1,3,,3.0\n",
            )

            # Parquet with null bitmap for the same missing values
            table = pa.table(
                {
                    "Scenario": pa.array([1, 1, 1], type=pa.int32()),
                    "Stage": pa.array([1, 1, 1], type=pa.int32()),
                    "Block": pa.array([1, 2, 3], type=pa.int32()),
                    "uid:1": pa.array([42.0, None, None], type=pa.float64()),
                    "uid:2": pa.array([1.0, 2.0, 3.0], type=pa.float64()),
                }
            )
            pq_buf = io.BytesIO()
            pq.write_table(table, pq_buf)
            zf.writestr("output/Demand/load_sol.parquet", pq_buf.getvalue())
        zip_buf.seek(0)

        resp = client.post(
            "/api/results/upload",
            data={"file": (zip_buf, "results.zip")},
            content_type="multipart/form-data",
        )
        assert resp.status_code == 200

        # Must parse as valid JSON
        data = json.loads(resp.data)

        # CSV output: missing values are empty strings
        csv_out = data["outputs"]["Generator/generation_sol.csv"]
        assert csv_out["data"][0][3] == "42.0"
        assert csv_out["data"][1][3] == ""  # missing → empty string
        assert csv_out["data"][2][3] == ""  # missing → empty string

        # Parquet output: missing values are None (JSON null)
        pq_out = data["outputs"]["Demand/load_sol"]
        assert pq_out["data"][0][3] == 42.0
        assert pq_out["data"][1][3] is None  # missing → null
        assert pq_out["data"][2][3] is None  # missing → null
