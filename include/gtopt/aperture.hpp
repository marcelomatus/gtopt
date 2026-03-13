/**
 * @file      aperture.hpp
 * @brief     Aperture definition for SDDP backward-pass scenario sampling
 * @date      Thu Mar 13 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * An Aperture represents one hydrological (or stochastic) realisation used
 * in the SDDP backward pass.  Each aperture references a **source scenario**
 * whose affluent data (flow bounds) are applied to the cloned phase LP before
 * solving.  The source scenario can come from:
 *
 *   1. The regular forward-scenario definitions (in the standard input
 *      directory).
 *   2. An aperture-specific scenario stored in a dedicated
 *      `aperture_directory` (see `SddpOptions::sddp_aperture_directory`).
 *
 * When no explicit `aperture_array` is provided in the `Simulation`, the
 * SDDP solver falls back to the legacy behaviour controlled by
 * `sddp_num_apertures` (first N scenarios or all scenarios).
 *
 * ### JSON Example
 * ```json
 * {
 *   "aperture_array": [
 *     {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
 *     {"uid": 2, "source_scenario": 5, "probability_factor": 0.3},
 *     {"uid": 3, "source_scenario": 10, "probability_factor": 0.2}
 *   ]
 * }
 * ```
 *
 * @see SDDPSolver::backward_pass_with_apertures, Scene, Scenario
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Aperture
 * @brief One backward-pass opening referencing a source scenario
 *
 * Maps an aperture to the scenario whose affluent values should be used
 * when solving the backward-pass LP.  The `probability_factor` determines
 * the weight of this aperture's cut in the expected (probability-weighted
 * average) Benders cut.
 *
 * When `probability_factor` values across all apertures do not sum to 1,
 * the solver normalises them internally.
 */
struct Aperture
{
  Uid uid {unknown_uid};  ///< Unique identifier for this aperture
  OptName name {};  ///< Optional human-readable label
  OptBool active {};  ///< Activation status (default: active)

  /// UID of the scenario whose affluent data to use.
  /// If the scenario exists in the aperture_directory it is loaded from
  /// there; otherwise it is looked up in the regular forward-scenario list.
  Uid source_scenario {unknown_uid};

  /// Probability weight of this aperture [p.u.].
  /// Values are normalised to sum 1 across all active apertures.
  OptReal probability_factor {1};

  static constexpr std::string_view class_name = "aperture";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

using ApertureUid = StrongUidType<Aperture>;
using ApertureIndex = StrongIndexType<Aperture>;

}  // namespace gtopt
