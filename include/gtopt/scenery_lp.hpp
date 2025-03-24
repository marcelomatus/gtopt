#pragma once

#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class SceneryLP
{
public:
  using StageSpan = std::span<const StageLP>;

  SceneryLP() = default;

  template<class Stages>
  explicit SceneryLP(Scenery pscenery, const Stages& pstages)
      : scenery(std::move(pscenery))
      , stage_span(pstages)
  {
  }

  [[nodiscard]] constexpr auto is_active() const
  {
    return scenery.active.value_or(true);
  }
  [[nodiscard]] constexpr auto uid() const { return SceneryUid(scenery.uid); }
  [[nodiscard]] constexpr auto probability_factor() const
  {
    return scenery.probability_factor.value_or(1.0);
  }

  [[nodiscard]] constexpr auto&& stage(const StageIndex index) const
  {
    return stage_span[index];
  }
  [[nodiscard]] constexpr auto&& stages() const { return stage_span; }

private:
  Scenery scenery;
  StageSpan stage_span;
};

}  // namespace gtopt
