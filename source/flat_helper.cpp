#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <range/v3/view/filter.hpp>

namespace
{
using namespace gtopt;

template<typename FlatHelper, typename Operation = decltype(true_fnc)>
constexpr STBUids make_stb_uids(const FlatHelper& helper,
                                Operation op = true_fnc) noexcept
{
  const auto size = helper.active_scenario_count() * helper.active_block_count();
  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);
  std::vector<Uid> block_uids;
  block_uids.reserve(size);

  for (auto&& scenario : helper.active_scenarios()) {
    for (auto&& stage : helper.active_stages()) {
      for (auto&& block : helper.active_stage_blocks()[stage]) {
        scenario_uids.push_back(scenario);
        stage_uids.push_back(stage);
        block_uids.push_back(block);
      }
    }
  }

  scenario_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();
  block_uids.shrink_to_fit();

  return {
      std::move(scenario_uids), std::move(stage_uids), std::move(block_uids)};
}

template<typename FlatHelper, typename Operation = decltype(true_fnc)>
constexpr STUids make_st_uids(const FlatHelper& helper,
                              Operation op = true_fnc) noexcept
{
  const auto size = helper.active_scenario_count() * helper.active_stage_count();
  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& scenario : helper.active_scenarios()) {
    for (auto&& stage : helper.active_stages()) {
      scenario_uids.push_back(scenario);
      stage_uids.push_back(stage);
    }
  }

  scenario_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();

  return {std::move(scenario_uids), std::move(stage_uids)};
}

template<typename FlatHelper, typename Operation = decltype(true_fnc)>
constexpr TUids make_t_uids(const FlatHelper& helper,
                            Operation op = true_fnc) noexcept
{
  const auto size = helper.active_stage_count();
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& stage : helper.active_stages()) {
    stage_uids.push_back(stage);
  }

  stage_uids.shrink_to_fit();

  return stage_uids;
}

}  // namespace

namespace gtopt
{

STBUids FlatHelper::stb_active_uids() const
{
  return make_stb_uids(*this, active_fnc);
}

STBUids FlatHelper::stb_uids() const
{
  return make_stb_uids(*this);
}

STUids FlatHelper::st_active_uids() const
{
  return make_st_uids(*this, active_fnc);
}

STUids FlatHelper::st_uids() const
{
  return make_st_uids(*this);
}

TUids FlatHelper::t_active_uids() const
{
  return make_t_uids(*this, active_fnc);
}

TUids FlatHelper::t_uids() const
{
  return make_t_uids(*this);
}

}  // namespace gtopt
