// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/block.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Block construction and default values")
{
  const Block block;

  CHECK(block.uid == Uid {unknown_uid});
  CHECK_FALSE(block.name.has_value());
  CHECK(block.duration == doctest::Approx(0.0));
  CHECK(block.class_name == "block");
}

TEST_CASE("Block attribute assignment")
{
  Block block;

  block.uid = 1;
  block.name = "peak_hour";
  block.duration = 1.0;

  CHECK(block.uid == 1);
  REQUIRE(block.name.has_value());
  CHECK(block.name.value() == "peak_hour");
  CHECK(block.duration == doctest::Approx(1.0));
}

TEST_CASE("Block designated initializer construction")
{
  const Block block {
      .uid = Uid {10},
      .name = "morning",
      .duration = 3.5,
  };

  CHECK(block.uid == Uid {10});
  REQUIRE(block.name.has_value());
  CHECK(block.name.value() == "morning");
  CHECK(block.duration == doctest::Approx(3.5));
}

TEST_CASE("Block with zero duration")
{
  const Block block {
      .uid = Uid {99},
      .duration = 0.0,
  };

  CHECK(block.uid == Uid {99});
  CHECK(block.duration == doctest::Approx(0.0));
}

TEST_CASE("BlockUid and BlockIndex strong types")
{
  const BlockUid buid {42};
  const BlockIndex bidx {3};

  CHECK(buid == BlockUid {42});
  CHECK(bidx == BlockIndex {3});

  // Verify incrementable_traits (used for iteration)
  BlockIndex idx {0};
  ++idx;
  CHECK(idx == BlockIndex {1});
}

TEST_CASE("Block array construction")  // NOLINT
{
  const Array<Block> blocks {
      {
          .uid = Uid {1},
          .name = "h1",
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "h2",
          .duration = 2.0,
      },
      {
          .uid = Uid {3},
          .name = "h3",
          .duration = 0.5,
      },
  };

  CHECK(blocks.size() == 3);
  CHECK(blocks[0].uid == Uid {1});
  CHECK(blocks[1].duration == doctest::Approx(2.0));
  CHECK(blocks[2].duration == doctest::Approx(0.5));
}
