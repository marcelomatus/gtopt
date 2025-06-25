#include <doctest/doctest.h>
#include <gtopt/stage_lp.hpp>

using namespace gtopt;

TEST_SUITE("StageLP")
{
  TEST_CASE("Construction and basic properties")
  {
    std::vector<Block> blocks = {Block {.uid = 1, .duration = 12.0},
                                 Block {.uid = 2, .duration = 24.0},
                                 Block {.uid = 3, .duration = 24.0},
                                 Block {.uid = 4, .duration = 12.0}};

    const Stage stage {.uid = 42,
                       .active = true,
                       .first_block = 1,
                       .count_block = 2,
                       .discount_factor = 0.9};

    const StageLP stage_lp(stage, blocks, 0.05, StageIndex {1}, PhaseIndex {2});

    SUBCASE("Basic properties")
    {
      CHECK(stage_lp.stage().uid == 42);
      CHECK(stage_lp.index() == StageIndex {1});
      CHECK(stage_lp.phase_index() == PhaseIndex {2});
      CHECK(stage_lp.is_active() == true);
      CHECK(stage_lp.uid() == StageUid {42});
    }

    SUBCASE("Duration calculations")
    {
      CHECK(stage_lp.duration() == doctest::Approx(48.0));  // 24 + 24
      CHECK(stage_lp.timeinit()
            == doctest::Approx(12.0));  // first block's duration
    }

    SUBCASE("Discount factor calculations")
    {
      // With annual discount rate 5% and timeinit 12h (0.5 day)
      // daily rate = (1 + 0.05)^(1/365) - 1 ≈ 0.000134
      // discount factor for 0.5 day ≈ 1 - 0.000134/2 ≈ 0.999933
      // Combined with stage discount 0.9 → 0.9 * 0.999933 ≈ 0.89994
      CHECK(stage_lp.discount_factor()
            == doctest::Approx(0.9 * 0.999933).epsilon(1e-6));
    }

    SUBCASE("Block access")
    {
      const auto& stage_blocks = stage_lp.blocks();
      REQUIRE(stage_blocks.size() == 2);
      CHECK(stage_blocks[0].uid() == BlockUid {2});
      CHECK(stage_blocks[1].uid() == BlockUid {3});
    }
  }

  TEST_CASE("Edge cases")
  {
    std::vector<Block> blocks = {Block {.uid = 1, .duration = 24.0}};

    SUBCASE("Empty stage")
    {
      const Stage stage {.first_block = 0, .count_block = 0};

      const StageLP stage_lp(stage, blocks);

      CHECK(stage_lp.blocks().empty());
      CHECK(stage_lp.duration() == doctest::Approx(0.0));
      CHECK(stage_lp.timeinit() == doctest::Approx(0.0));
    }

    SUBCASE("Inactive stage")
    {
      const Stage stage {.active = false, .first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks);

      CHECK(stage_lp.is_active() == false);
    }

    SUBCASE("Default discount factor")
    {
      const Stage stage {.first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks, 0.0);

      CHECK(stage_lp.discount_factor() == doctest::Approx(1.0));
    }

    SUBCASE("No stage discount factor")
    {
      const Stage stage {.first_block = 0, .count_block = 1};

      const StageLP stage_lp(stage, blocks, 0.05);

      CHECK(stage_lp.discount_factor() == doctest::Approx(1.0).epsilon(1e-6));
    }
  }

  TEST_CASE("Move semantics")
  {
    std::vector<Block> blocks = {Block {.uid = 1, .duration = 24.0}};

    const Stage stage {.uid = 10, .first_block = 0, .count_block = 1};

    StageLP original(stage, blocks);
    const StageLP moved(std::move(original));

    CHECK(moved.uid() == StageUid {10});
    CHECK(moved.blocks().size() == 1);
    CHECK(moved.duration() == doctest::Approx(24.0));
  }
}
