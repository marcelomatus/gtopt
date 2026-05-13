// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      sddp_infeasible_probe.hpp
 * @brief     Diagnostic probe for SDDP apertures that gtopt declares
 *            infeasible — dump the LP to disk and re-solve it from
 *            scratch with an external CPLEX process to verify whether
 *            the infeasibility is genuine or a phantom of the
 *            in-memory LP state.
 *
 * Activated by environment variable, never on by default.  The probe
 * is intentionally heavy (spawns `cplex` per call) so it MUST stay
 * gated and throttled.
 *
 * Environment knobs
 * -----------------
 *
 *   GTOPT_DEBUG_INFEASIBLE_PROBE = 1     enable the probe (default: off)
 *   GTOPT_DEBUG_INFEASIBLE_DIR   = path  output directory for `.lp`
 *                                        dumps (default: /tmp)
 *   GTOPT_DEBUG_INFEASIBLE_MAX   = N     stop after N probes per
 *                                        process (default: 20)
 *   CPLEX_BIN                    = path  CPLEX CLI binary
 *                                        (default: `cplex` from PATH)
 *
 * Output
 * ------
 *
 * On each fired probe, one warning-level log line is emitted with
 * the prefix ``INFEAS-PROBE [<context>]`` reporting:
 *
 *   * `inmem_status` — the int status code from the in-memory solve
 *   * `fresh`        — cold CPLEX verdict (`optimal` / `infeasible` /
 *                      `unbounded` / `unknown`)
 *   * `obj`          — objective value when fresh = optimal
 *   * `nrows ncols`  — in-memory LP shape at probe time
 *   * `lp`           — path to the dumped `.lp` file
 */
#pragma once

#include <string>

namespace gtopt
{
class LinearInterface;

/**
 * Run the infeasibility probe.  No-op unless the gating environment
 * variable is set.
 *
 * @param li            The interface whose backend just returned a
 *                      non-optimal status.  Read-only.
 * @param context       Slash-free identifier embedded in the dump
 *                      filename and the log line (e.g.
 *                      `"aperture_i1_s6_p42_a62"`).
 * @param inmem_status  Solver status the in-memory backend returned.
 */
void probe_infeasible_lp(const LinearInterface& li,
                         std::string context,
                         int inmem_status);

}  // namespace gtopt
