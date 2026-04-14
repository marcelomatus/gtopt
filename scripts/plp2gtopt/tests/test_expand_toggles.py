# -*- coding: utf-8 -*-
"""Unit tests for the Stage-2 auto-expansion toggles.

Covers:

* ``_dump_ror_promoted`` — serializes the resolved RoR spec to
  ``ror_promoted.json`` using the same schema as
  ``gtopt_expand ror``.
* ``_merge_entities`` — appends ``*_array`` keys, aggregates singular
  ``user_constraint_file`` into the plural ``user_constraint_files``.
* ``process_ror_spec`` — dumps the audit artifact only when
  ``expand_ror`` is True.
* ``process_water_rights`` — auto-invokes the ``gtopt_expand`` APIs
  by default, respects ``--no-expand-water-rights`` /
  ``--no-expand-lng`` opt-outs.
"""

from __future__ import annotations

import json
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest

from gtopt_expand.ror_expand import RorSpec
from plp2gtopt.gtopt_writer import GTOptWriter


def _make_writer() -> GTOptWriter:
    parser = MagicMock()
    parser.parsed_data = {}
    writer = GTOptWriter.__new__(GTOptWriter)
    writer.parser = parser
    writer.options = None
    writer.output_path = None
    writer.planning = {"options": {}, "system": {}, "simulation": {}}
    return writer


# ---------------------------------------------------------------------------
# _dump_ror_promoted
# ---------------------------------------------------------------------------
class TestDumpRorPromoted:
    def test_writes_expected_schema(self, tmp_path):
        resolved = {
            "CentA": RorSpec(vmax_hm3=1.5, production_factor=2.0, pmax_mw=100.0),
            "CentB": RorSpec(vmax_hm3=0.8, production_factor=3.1, pmax_mw=None),
        }
        GTOptWriter._dump_ror_promoted(resolved, {"output_dir": tmp_path})

        path = tmp_path / "ror_promoted.json"
        assert path.exists()
        data = json.loads(path.read_text(encoding="utf-8"))
        assert list(data) == ["promoted"]
        names = [e["name"] for e in data["promoted"]]
        assert names == ["CentA", "CentB"]  # sorted

        a = next(e for e in data["promoted"] if e["name"] == "CentA")
        assert a == {
            "name": "CentA",
            "vmax_hm3": 1.5,
            "production_factor": 2.0,
            "pmax_mw": 100.0,
        }
        b = next(e for e in data["promoted"] if e["name"] == "CentB")
        assert "pmax_mw" not in b  # omitted when None

    def test_no_output_dir_is_noop(self, tmp_path):
        resolved = {"X": RorSpec(vmax_hm3=1.0, production_factor=1.0)}
        GTOptWriter._dump_ror_promoted(resolved, {})
        assert not list(tmp_path.iterdir())


# ---------------------------------------------------------------------------
# _merge_entities
# ---------------------------------------------------------------------------
class TestMergeEntities:
    def test_arrays_are_appended(self):
        writer = _make_writer()
        writer.planning["system"]["flow_right_array"] = [{"uid": 1}]
        writer._merge_entities(
            {"flow_right_array": [{"uid": 2}], "volume_right_array": [{"uid": 3}]},
        )
        assert writer.planning["system"]["flow_right_array"] == [{"uid": 1}, {"uid": 2}]
        assert writer.planning["system"]["volume_right_array"] == [{"uid": 3}]

    def test_user_constraint_file_aggregates(self):
        writer = _make_writer()
        writer._merge_entities({"user_constraint_file": "laja.pampl"})
        writer._merge_entities({"user_constraint_file": "maule.pampl"})
        assert writer.planning["system"]["user_constraint_files"] == [
            "laja.pampl",
            "maule.pampl",
        ]

    def test_scalar_keys_overwrite(self):
        writer = _make_writer()
        writer._merge_entities({"lng_terminal_array": [{"uid": 1}]})
        # lng_terminal_array is an _array key, so it's appended not overwritten.
        writer._merge_entities({"lng_terminal_array": [{"uid": 2}]})
        assert writer.planning["system"]["lng_terminal_array"] == [
            {"uid": 1},
            {"uid": 2},
        ]


# ---------------------------------------------------------------------------
# process_ror_spec → ror_promoted.json audit artifact
# ---------------------------------------------------------------------------
class _StubCentralParser:
    def __init__(self, centrals_of_type):
        self.centrals_of_type = centrals_of_type


@pytest.fixture
def writer_with_pasada():
    """Writer with a single pasada central eligible for promotion."""
    writer = _make_writer()
    writer.parser.parsed_data["central_parser"] = _StubCentralParser(
        {
            "serie": [],
            "embalse": [],
            "pasada": [
                {
                    "name": "CentA",
                    "number": 1,
                    "bus": 1,
                    "type": "pasada",
                    "pmax": 100.0,
                    "efficiency": 1.0,
                },
            ],
        },
    )
    return writer


@pytest.fixture
def ror_csv(tmp_path):
    path = tmp_path / "ror.csv"
    path.write_text(
        "name,vmax_hm3,production_factor,pmax_mw,enabled\nCentA,1.5,2.0,100,true\n",
        encoding="utf-8",
    )
    return path


class TestProcessRorSpecAuditArtifact:
    def test_emits_ror_promoted_by_default(
        self,
        writer_with_pasada,
        ror_csv,
        tmp_path,
    ):
        opts = {
            "output_dir": tmp_path,
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": ror_csv,
            "_pasada_hydro_names": {"CentA"},
        }
        writer_with_pasada.process_ror_spec(opts)

        audit = tmp_path / "ror_promoted.json"
        assert audit.exists()
        data = json.loads(audit.read_text(encoding="utf-8"))
        assert [e["name"] for e in data["promoted"]] == ["CentA"]

    def test_no_expand_ror_suppresses_dump(
        self,
        writer_with_pasada,
        ror_csv,
        tmp_path,
    ):
        opts = {
            "output_dir": tmp_path,
            "ror_as_reservoirs": "all",
            "ror_as_reservoirs_file": ror_csv,
            "_pasada_hydro_names": {"CentA"},
            "expand_ror": False,
        }
        writer_with_pasada.process_ror_spec(opts)

        assert not (tmp_path / "ror_promoted.json").exists()
        # Inline resolution still populates the opts caches so downstream
        # writers keep working.
        assert "CentA" in opts["_ror_spec_resolved"]

    def test_no_match_emits_nothing(self, writer_with_pasada, ror_csv, tmp_path):
        opts = {
            "output_dir": tmp_path,
            "ror_as_reservoirs": "none",  # disables resolution
            "ror_as_reservoirs_file": ror_csv,
            "_pasada_hydro_names": {"CentA"},
        }
        writer_with_pasada.process_ror_spec(opts)
        assert not (tmp_path / "ror_promoted.json").exists()


# ---------------------------------------------------------------------------
# process_water_rights auto-expand behaviour
# ---------------------------------------------------------------------------
class _StubAgreementParser:
    """Minimal parser-like stub whose ``.config`` dict drives the Stage-2 API."""

    def __init__(self, config):
        self.config = config


@pytest.fixture
def writer_with_dummy_parsers(monkeypatch):
    """Writer wired to minimal parser stubs plus patched expansion helpers."""
    writer = _make_writer()

    writer.parser.parsed_data["laja_parser"] = _StubAgreementParser({"k": "laja"})
    writer.parser.parsed_data["maule_parser"] = _StubAgreementParser({"k": "maule"})
    writer.parser.parsed_data["gnl_parser"] = _StubAgreementParser({"k": "lng"})
    writer.parser.parsed_data["stage_parser"] = SimpleNamespace(get_all=lambda: [])

    calls: dict[str, int] = {"laja": 0, "maule": 0, "lng": 0}

    def _fake_laja(self, cfg, stage_parser, output_dir):
        calls["laja"] += 1

    def _fake_maule(self, cfg, stage_parser, output_dir):
        calls["maule"] += 1

    def _fake_lng(self, cfg):
        calls["lng"] += 1

    monkeypatch.setattr(GTOptWriter, "_expand_laja", _fake_laja)
    monkeypatch.setattr(GTOptWriter, "_expand_maule", _fake_maule)
    monkeypatch.setattr(GTOptWriter, "_expand_lng", _fake_lng)

    return writer, calls


class TestProcessWaterRightsToggles:
    def test_default_expands_laja_and_maule(self, writer_with_dummy_parsers, tmp_path):
        writer, calls = writer_with_dummy_parsers
        writer.process_water_rights(
            {"emit_water_rights": True, "output_dir": tmp_path},
        )
        # process_water_rights is no longer responsible for LNG.
        assert calls == {"laja": 1, "maule": 1, "lng": 0}
        # Intermediate *_dat.json files are NOT written (never shipped).
        assert not (tmp_path / "laja_dat.json").exists()
        assert not (tmp_path / "maule_dat.json").exists()
        assert not (tmp_path / "lng_dat.json").exists()

    def test_no_expand_water_rights_skips_everything(
        self,
        writer_with_dummy_parsers,
        tmp_path,
    ):
        writer, calls = writer_with_dummy_parsers
        writer.process_water_rights(
            {
                "emit_water_rights": True,
                "output_dir": tmp_path,
                "expand_water_rights": False,
            },
        )
        assert calls == {"laja": 0, "maule": 0, "lng": 0}
        assert not any(tmp_path.iterdir())

    def test_emit_disabled_skips_everything(
        self,
        writer_with_dummy_parsers,
        tmp_path,
    ):
        writer, calls = writer_with_dummy_parsers
        writer.process_water_rights({"output_dir": tmp_path})
        assert calls == {"laja": 0, "maule": 0, "lng": 0}
        assert not any(tmp_path.iterdir())


class TestProcessLngToggle:
    def test_default_expands_lng(self, writer_with_dummy_parsers, tmp_path):
        writer, calls = writer_with_dummy_parsers
        writer.process_lng(
            {"emit_water_rights": True, "output_dir": tmp_path},
        )
        assert calls == {"laja": 0, "maule": 0, "lng": 1}
        # No intermediate lng_dat.json written.
        assert not (tmp_path / "lng_dat.json").exists()

    def test_no_expand_lng_skips(self, writer_with_dummy_parsers, tmp_path):
        writer, calls = writer_with_dummy_parsers
        writer.process_lng(
            {
                "emit_water_rights": True,
                "output_dir": tmp_path,
                "expand_lng": False,
            },
        )
        assert calls == {"laja": 0, "maule": 0, "lng": 0}
        assert not any(tmp_path.iterdir())

    def test_emit_disabled_skips_everything(self, writer_with_dummy_parsers, tmp_path):
        writer, calls = writer_with_dummy_parsers
        writer.process_lng({"output_dir": tmp_path})
        assert calls == {"laja": 0, "maule": 0, "lng": 0}
        assert not any(tmp_path.iterdir())


# ---------------------------------------------------------------------------
# process_hb_maule — params-file loading and plpcnfce.dat fallback
# ---------------------------------------------------------------------------
class _StubCentralParserForHb:
    """Stub with a COLBUN embalse entry carrying emin/emax."""

    def __init__(self, emin: float, emax: float):
        self.centrals_of_type = {
            "embalse": [{"name": "COLBUN", "emin": emin, "emax": emax}],
        }


def _writer_for_hb_maule():
    writer = _make_writer()
    writer.parser.parsed_data["central_parser"] = _StubCentralParserForHb(
        emin=1000.0, emax=10000.0
    )
    writer.planning["system"]["reservoir_array"] = [
        {"name": "COLBUN", "emin": 1000.0, "emax": 10000.0},
        {"name": "MACHICURA"},
    ]
    return writer


class TestProcessHbMaule:
    def test_disabled_by_default(self, tmp_path):
        writer = _writer_for_hb_maule()
        writer.process_hb_maule({"output_dir": tmp_path})
        assert not (tmp_path / "hb_maule.json").exists()
        assert "waterway_array" not in writer.planning["system"]

    def test_plpcnfce_vmin_vmax_used_when_no_file(self, tmp_path):
        writer = _writer_for_hb_maule()
        writer.process_hb_maule(
            {"expand_hb_maule": True, "output_dir": tmp_path},
        )
        hb_path = tmp_path / "hb_maule.json"
        assert hb_path.exists()
        payload = json.loads(hb_path.read_text(encoding="utf-8"))
        segs = payload["system"]["reservoir_production_factor_array"][0]["segments"]
        # Defaults: vmin=1000, vmax=10000 from the COLBUN stub
        assert segs[0]["volume"] == 1000.0
        assert segs[1]["volume"] == 10000.0

    def test_params_file_overrides_defaults(self, tmp_path):
        writer = _writer_for_hb_maule()
        params_path = tmp_path / "hb_params.json"
        params_path.write_text(
            json.dumps(
                {
                    "vmin": 2000.0,
                    "vmax": 8000.0,
                    "pump": {"pmax_mw": 75.0, "qmax_m3s": 40.0, "pump_factor": 2.0},
                }
            ),
            encoding="utf-8",
        )
        writer.process_hb_maule(
            {
                "expand_hb_maule": True,
                "output_dir": tmp_path,
                "hb_maule_params_file": params_path,
            },
        )
        payload = json.loads((tmp_path / "hb_maule.json").read_text(encoding="utf-8"))
        segs = payload["system"]["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 2000.0
        assert segs[1]["volume"] == 8000.0
        assert payload["system"]["pump_array"][0]["pump_factor"] == 2.0

    def test_params_file_zero_vmin_falls_back_to_plpcnfce(self, tmp_path):
        """vmin=0/vmax=0 in the user file → resolve from plpcnfce.dat."""
        writer = _writer_for_hb_maule()
        params_path = tmp_path / "hb_params.json"
        params_path.write_text(
            json.dumps({"vmin": 0.0, "vmax": 0.0}),
            encoding="utf-8",
        )
        writer.process_hb_maule(
            {
                "expand_hb_maule": True,
                "output_dir": tmp_path,
                "hb_maule_params_file": params_path,
            },
        )
        payload = json.loads((tmp_path / "hb_maule.json").read_text(encoding="utf-8"))
        segs = payload["system"]["reservoir_production_factor_array"][0]["segments"]
        assert segs[0]["volume"] == 1000.0
        assert segs[1]["volume"] == 10000.0

    def test_no_colbun_embalse_and_no_file_raises(self, tmp_path):
        writer = _make_writer()
        writer.parser.parsed_data["central_parser"] = type(
            "P", (), {"centrals_of_type": {"embalse": []}}
        )()
        writer.planning["system"]["reservoir_array"] = [{"name": "MACHICURA"}]
        with pytest.raises(ValueError, match="COLBUN"):
            writer.process_hb_maule(
                {"expand_hb_maule": True, "output_dir": tmp_path},
            )

    def test_invalid_params_file_raises(self, tmp_path):
        writer = _writer_for_hb_maule()
        params_path = tmp_path / "hb_params.json"
        params_path.write_text(json.dumps([1, 2, 3]), encoding="utf-8")
        with pytest.raises(ValueError, match="expected a JSON object"):
            writer.process_hb_maule(
                {
                    "expand_hb_maule": True,
                    "output_dir": tmp_path,
                    "hb_maule_params_file": params_path,
                },
            )


class TestHbMauleParamsTemplate:
    def test_template_matches_default_config(self):
        """print_hb_maule_params_template outputs the default_config dict."""
        import io  # noqa: PLC0415
        from contextlib import redirect_stdout  # noqa: PLC0415

        from gtopt_expand import hb_maule_default_config  # noqa: PLC0415
        from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
            print_hb_maule_params_template,
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = print_hb_maule_params_template()
        assert rc == 0
        printed = json.loads(buf.getvalue())
        assert printed == hb_maule_default_config(vmin=0.0, vmax=0.0)
        # Sanity: pump.pdf defaults are present.
        assert printed["pump"]["pump_factor"] == 1.88
        assert printed["gen_nominal"]["production_factor"] == 1.44
