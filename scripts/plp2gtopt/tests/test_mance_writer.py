"""Unit tests for ManceWriter class."""

import json
import tempfile
from pathlib import Path
import pytest
from ..mance_writer import ManceWriter
from ..mance_parser import ManceParser
from ..central_parser import CentralParser
from ..block_parser import BlockParser
from .conftest import get_example_file


@pytest.fixture
def sample_mance_file():
    """Fixture providing path to sample maintenance file."""
    return get_example_file("plpmance.dat")


@pytest.fixture
def sample_mance_writer(sample_mance_file):
    """Fixture providing initialized ManceWriter with sample data."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    return ManceWriter(parser)


def test_mance_writer_initialization(sample_mance_file):
    """Test ManceWriter initialization."""
    parser = ManceParser(sample_mance_file)
    parser.parse()
    writer = ManceWriter(parser)

    assert writer.parser == parser
    assert writer.items is not None and len(writer.items) == parser.num_mances


def test_to_json_array(sample_mance_writer):
    """Test conversion of maintenance data to JSON array format."""
    json_mances = sample_mance_writer.to_json_array()

    # Verify basic structure
    assert isinstance(json_mances, list)
    assert len(json_mances) > 0

    # Verify each maintenance has required fields
    required_fields = {
        "name": str,
        "block": list,
        "pmin": list,
        "pmax": list,
    }

    for mance in json_mances:
        for field, field_type in required_fields.items():
            assert field in mance, f"Missing field: {field}"
            assert isinstance(mance[field], field_type), (
                f"Field {field} should be {field_type}, got {type(mance[field])}"
            )


def test_write_to_file(sample_mance_writer):
    """Test writing maintenance data to JSON file."""
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        sample_mance_writer.write_to_file(output_path)

        # Verify file was created and contains valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) > 0


def test_json_output_structure(sample_mance_writer):
    """Verify JSON output matches expected structure."""
    json_mances = sample_mance_writer.to_json_array()

    # Expected structure
    required_fields = {
        "name": str,
        "block": list,
        "pmin": list,
        "pmax": list,
    }

    for mance in json_mances:
        # Check all required fields exist and have correct types
        assert set(mance.keys()) == set(required_fields.keys())
        for field, field_type in required_fields.items():
            assert isinstance(mance[field], field_type), (
                f"Field {field} should be {field_type}, got {type(mance[field])}"
            )

        # Additional value checks
        assert len(mance["name"]) > 0, "Name should not be empty"
        assert len(mance["block"]) > 0, "Should have at least one block"
        assert len(mance["block"]) == len(mance["pmin"]), (
            "Blocks and p_min should match"
        )
        assert len(mance["block"]) == len(mance["pmax"]), (
            "Blocks and p_max should match"
        )


class MockEmptyManceParser(ManceParser):
    """Mock parser that returns empty data."""

    def __init__(self):
        """Initializes the mock parser with a dummy file path."""
        super().__init__("dummy.dat")

    def get_all(self) -> list:
        """Return an empty list of items."""
        return []


def test_write_empty_mances():
    """Test handling of empty maintenance list."""
    # Create parser with no maintenance data
    parser = MockEmptyManceParser()
    writer = ManceWriter(parser)

    # Test empty array conversion
    json_mances = writer.to_json_array()
    assert isinstance(json_mances, list)
    assert len(json_mances) == 0

    # Test writing empty list
    with tempfile.NamedTemporaryFile(suffix=".json") as tmp_file:
        output_path = Path(tmp_file.name)
        writer.write_to_file(output_path)

        # Verify file exists and is valid JSON
        assert output_path.exists()
        with open(output_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            assert isinstance(data, list)
            assert len(data) == 0


def test_to_dataframe_with_empty_parser():
    """Test DataFrame creation with empty parser."""
    parser = MockEmptyManceParser()
    writer = ManceWriter(parser)

    df_pmin, df_pmax = writer.to_dataframe()

    assert df_pmin.empty
    assert df_pmax.empty


def test_to_dataframe_with_parsers(tmp_path):
    """Test to_dataframe with central and block parsers."""
    # Build minimal plpcnfce.dat with one central matching the mance file
    cnfce = tmp_path / "plpcnfce.dat"
    cnfce.write_text(
        "# Num.Centrales  Num.Embalses Num.Serie Num.Fallas Num.Pas.Pur. Num.BAT\n"
        "    1            0            0          0          1            0\n"
        "# Interm Min.Tec. Cos.Arr.Det. FFaseSinMT EtapaCambioFase\n"
        "  F      F        F            F          00\n"
        "# Centrales de Pasada\n"
        "    5 'GENUNIT'                                     "
        "     1    F       F       F       F           F          0           0\n"
        "          PotMin PotMax VertMin VertMax\n"
        "           000.0  100.0   000.0   000.0\n"
        "           Start   Stop ON(t<0) NEta_OnOff\n"
        "             0.0    0.0 F       0              "
        " Pot.           Volumen    Volumen    Volumen    Volumen  Factor\n"
        "          CosVar  Rendi  Barra Genera Vertim    t<0  Afluen    Inicial  "
        "    Final     Minimo     Maximo  Escala EmbCFUE\n"
        "             0.0  1.000      1      1      0    0.0  "
        "0020.0  0.0  0.0  0.0  1.0  1.0E+0       F\n"
    )
    blo = tmp_path / "plpblo.dat"
    blo.write_text(
        "# Bloques\n"
        "     2\n"
        "# Bloque   Etapa   NHoras  Ano   Mes  TipoBloque\n"
        "    001    001      007    001    003    'Bloque 01'\n"
        "    002    001      007    001    003    'Bloque 02'\n"
    )
    mance_f = tmp_path / "plpmance.dat"
    mance_f.write_text(
        " 1\n"
        "'GENUNIT'\n"
        "   2                 01\n"
        "     03      001        1     5.00    80.00\n"
        "     03      002        1     5.00    80.00\n"
    )

    central_parser = CentralParser(cnfce)
    central_parser.parse()
    block_parser = BlockParser(blo)
    block_parser.parse()
    mance_parser = ManceParser(mance_f)
    mance_parser.parse()

    writer = ManceWriter(mance_parser, central_parser, block_parser)
    df_pmin, df_pmax = writer.to_dataframe()

    assert not df_pmin.empty
    assert not df_pmax.empty
    assert "stage" in df_pmin.columns
    assert "stage" in df_pmax.columns


def test_to_parquet(tmp_path):
    """Test to_parquet writes pmin and pmax files."""
    cnfce = tmp_path / "plpcnfce.dat"
    cnfce.write_text(
        "# Num.Centrales  Num.Embalses Num.Serie Num.Fallas Num.Pas.Pur. Num.BAT\n"
        "    1            0            0          0          1            0\n"
        "# Interm Min.Tec. Cos.Arr.Det. FFaseSinMT EtapaCambioFase\n"
        "  F      F        F            F          00\n"
        "# Centrales de Pasada\n"
        "    5 'GENUNIT'                                     "
        "     1    F       F       F       F           F          0           0\n"
        "          PotMin PotMax VertMin VertMax\n"
        "           000.0  100.0   000.0   000.0\n"
        "           Start   Stop ON(t<0) NEta_OnOff\n"
        "             0.0    0.0 F       0              "
        " Pot.           Volumen    Volumen    Volumen    Volumen  Factor\n"
        "          CosVar  Rendi  Barra Genera Vertim    t<0  Afluen    Inicial  "
        "    Final     Minimo     Maximo  Escala EmbCFUE\n"
        "             0.0  1.000      1      1      0    0.0  "
        "0020.0  0.0  0.0  0.0  1.0  1.0E+0       F\n"
    )
    blo = tmp_path / "plpblo.dat"
    blo.write_text(
        "# Bloques\n"
        "     2\n"
        "# Bloque   Etapa   NHoras  Ano   Mes  TipoBloque\n"
        "    001    001      007    001    003    'Bloque 01'\n"
        "    002    001      007    001    003    'Bloque 02'\n"
    )
    mance_f = tmp_path / "plpmance.dat"
    mance_f.write_text(
        " 1\n"
        "'GENUNIT'\n"
        "   2                 01\n"
        "     03      001        1     5.00    80.00\n"
        "     03      002        1     5.00    80.00\n"
    )

    central_parser = CentralParser(cnfce)
    central_parser.parse()
    block_parser = BlockParser(blo)
    block_parser.parse()
    mance_parser = ManceParser(mance_f)
    mance_parser.parse()

    writer = ManceWriter(mance_parser, central_parser, block_parser)
    out_dir = tmp_path / "mance_out"
    cols = writer.to_parquet(out_dir)

    assert (out_dir / "pmin.parquet").exists()
    assert (out_dir / "pmax.parquet").exists()
    assert len(cols["pmin"]) > 0
    assert len(cols["pmax"]) > 0


def test_to_parquet_empty(tmp_path):
    """Test to_parquet with empty data creates no parquet files."""
    parser = MockEmptyManceParser()
    writer = ManceWriter(parser)
    out_dir = tmp_path / "empty_out"
    cols = writer.to_parquet(out_dir)
    assert not cols["pmin"]
    assert not cols["pmax"]
