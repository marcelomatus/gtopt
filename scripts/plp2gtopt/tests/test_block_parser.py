"""Unit tests for block_parser.py BlockParser class."""

from pathlib import Path
import pytest
from ..block_parser import BlockParser


@pytest.fixture
def sample_block_file():
    """Fixture providing path to sample block file."""
    test_file = Path(__file__).parent.parent / "test_data" / "plpblo.dat"
    if not test_file.exists():
        test_file = (
            Path(__file__).parent.parent.parent / "cases" / "plp_case_2y" / "plpblo.dat"
        )
    return test_file


def test_block_parser_initialization():
    """Test BlockParser initialization."""
    parser = BlockParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.blocks  # Check empty list
    assert parser.num_blocks == 0


def test_get_num_blocks():
    """Test get_num_blocks returns correct value."""
    parser = BlockParser("test.dat")
    parser.num_blocks = 5
    assert parser.get_num_blocks() == 5


def test_get_blocks():
    """Test get_blocks returns blocks list."""
    parser = BlockParser("test.dat")
    test_blocks = [{"test": "data"}]
    parser.blocks = test_blocks
    assert parser.get_blocks() == test_blocks


def test_parse_sample_file(sample_block_file):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample block file."""
    parser = BlockParser(str(sample_block_file))
    parser.parse()

    # Verify basic structure
    num_blocks = parser.get_num_blocks()
    assert num_blocks > 0
    blocks = parser.get_blocks()
    assert len(blocks) == num_blocks

    # Verify first block data
    block1 = blocks[0]
    assert block1["number"] == 1
    assert block1["stage"] > 0
    assert block1["duration"] > 0  # Duration should be positive


def test_get_block_by_number(sample_block_file):  # pylint: disable=redefined-outer-name
    """Test getting block by number."""
    parser = BlockParser(str(sample_block_file))
    parser.parse()

    # Test existing block
    blocks = parser.get_blocks()
    first_block_num = blocks[0]["number"]
    block = parser.get_block_by_number(first_block_num)
    assert block is not None
    assert block["number"] == first_block_num
    assert block["duration"] > 0  # Duration should be positive

    # Test non-existent block
    missing = parser.get_block_by_number(9999)
    assert missing is None
