#include <gtopt/simulation.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

Simulation& Simulation::merge(Simulation& sim)
{
  gtopt::merge(block_array, sim.block_array);
  gtopt::merge(stage_array, sim.stage_array);
  gtopt::merge(scenario_array, sim.scenario_array);
  gtopt::merge(phase_array, sim.phase_array);
  gtopt::merge(scene_array, sim.scene_array);

  return *this;
}

}  // namespace gtopt
