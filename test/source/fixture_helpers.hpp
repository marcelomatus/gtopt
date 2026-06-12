// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      fixture_helpers.hpp
 * @brief     Small fixture builders for test helpers (Block / Stage / Phase)
 * @date      2026-04-11
 *
 * These helpers encapsulate the fixture-only convention that test data is
 * constructed with contiguous UIDs `{1, 2, …, n}`.  Production code must
 * never synthesize a `Uid` from a loop counter — `Uid` is an opaque,
 * user-facing identifier, not a position into an array.  Tests accept this
 * shortcut only because fixture data is internally consistent: the UID and
 * the array position are both defined by the fixture author.  The helpers
 * below localise that shortcut in one place and use `iota_range` from
 * `gtopt/utils.hpp` (never `std::views::iota`, which has compatibility
 * issues with our toolchains).
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt::test_fixtures
{

/// Generate `n` uniform-duration Blocks with fixture UIDs `{1 … n}`.
inline auto make_uniform_blocks(std::size_t n, double duration = 1.0)
    -> Array<Block>
{
  Array<Block> out;
  out.reserve(n);
  for (const auto i : iota_range(std::size_t {0}, n)) {
    out.push_back(Block {
        .uid = Uid {static_cast<uid_t>(i + 1)},
        .duration = duration,
    });
  }
  return out;
}

/// Generate `n` Stages, each spanning `blocks_per_stage` consecutive blocks,
/// with fixture UIDs `{1 … n}` and `first_block = s * blocks_per_stage`.
inline auto make_uniform_stages(std::size_t n, std::size_t blocks_per_stage)
    -> Array<Stage>
{
  Array<Stage> out;
  out.reserve(n);
  for (const auto s : iota_range(std::size_t {0}, n)) {
    out.push_back(Stage {
        .uid = Uid {static_cast<uid_t>(s + 1)},
        .first_block = static_cast<Size>(s * blocks_per_stage),
        .count_block = blocks_per_stage,
    });
  }
  return out;
}

/// Generate `n` single-stage Phases (one Stage per Phase) with fixture UIDs
/// `{1 … n}` and `first_stage = p`.
inline auto make_single_stage_phases(std::size_t n) -> Array<Phase>
{
  Array<Phase> out;
  out.reserve(n);
  for (const auto p : iota_range(std::size_t {0}, n)) {
    out.push_back(Phase {
        .uid = Uid {static_cast<uid_t>(p + 1)},
        .first_stage = static_cast<Size>(p),
        .count_stage = 1,
    });
  }
  return out;
}

}  // namespace gtopt::test_fixtures
