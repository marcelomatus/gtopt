"""Unit tests for block_parser.py BlockParser class."""

from pathlib import Path
import pytest
from ..block_parser import BlockParser
from .conftest import get_example_file


@pytest.fixture
def sample_block_file():
    """Fixture providing path to sample block file."""
    return get_example_file("plpblo.dat")


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
    assert num_blocks == 10
    blocks = parser.get_blocks()
    assert len(blocks) == num_blocks

    # Verify all blocks have required fields
    for block in blocks:
        assert isinstance(block["number"], int)
        assert isinstance(block["stage"], int) 
        assert isinstance(block["duration"], float)
        assert block["number"] > 0
        assert block["stage"] > 0
        assert block["duration"] > 0

    # Verify first block data
    block1 = blocks[0]
    assert block1["number"] == 1
    assert block1["stage"] == 1
    assert block1["duration"] == 7.0

    # Verify last block data
    last_block = blocks[-1]
    assert last_block["number"] == 10
    assert last_block["stage"] == 1
    assert last_block["duration"] == 8.0

    # Verify block numbers are sequential
    for i, block in enumerate(blocks, 1):
        assert block["number"] == i


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
    assert block["duration"] == 7  # Duration should be positive

    # Test non-existent block
    missing = parser.get_block_by_number(9999)
    assert missing is None
