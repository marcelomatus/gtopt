# SPDX-License-Identifier: BSD-3-Clause
"""Hydro, cascade, battery, and capacity adequacy checks."""

from typing import Any

from gtopt_check_json._checks_common import (
    Finding,
    Severity,
    _extract_scalar_values,
)


def check_affluent_nonneg(planning: dict[str, Any]) -> list[Finding]:
    """Check that all Flow affluent values are non-negative."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for idx, flow in enumerate(sys.get("flow_array", [])):
        affluent = flow.get("affluent")
        values = _extract_scalar_values(affluent)
        neg_vals = [v for v in values if v < 0]
        if neg_vals:
            name = flow.get("name", f"index {idx}")
            findings.append(
                Finding(
                    check_id="affluent_nonneg",
                    severity=Severity.WARNING,
                    message=(
                        f"Flow '{name}' (uid={flow.get('uid')}): "
                        f"negative affluent value(s): {neg_vals}"
                    ),
                )
            )

    return findings


def check_battery_efficiency(planning: dict[str, Any]) -> list[Finding]:
    """Check that battery efficiencies are at most 1.0 (100 %).

    Both ``input_efficiency`` (charging) and ``output_efficiency``
    (discharging) must be in the range [0, 1].  Values above 1.0
    indicate a data entry error (energy would be created from nothing).
    """
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for idx, bat in enumerate(sys.get("battery_array", [])):
        name = bat.get("name", f"index {idx}")
        uid = bat.get("uid")
        for field in ("input_efficiency", "output_efficiency"):
            values = _extract_scalar_values(bat.get(field))
            bad = [v for v in values if v > 1.0]
            if bad:
                findings.append(
                    Finding(
                        check_id="battery_efficiency",
                        severity=Severity.WARNING,
                        message=(
                            f"Battery '{name}' (uid={uid}): "
                            f"{field} value(s) > 1.0: {bad}"
                        ),
                        action="Check plpess.dat / plpcenbat.dat efficiency values",
                    )
                )
            neg = [v for v in values if v < 0.0]
            if neg:
                findings.append(
                    Finding(
                        check_id="battery_efficiency",
                        severity=Severity.WARNING,
                        message=(
                            f"Battery '{name}' (uid={uid}): "
                            f"negative {field} value(s): {neg}"
                        ),
                        action="Check plpess.dat / plpcenbat.dat efficiency values",
                    )
                )

    return findings


def check_capacity_adequacy(planning: dict[str, Any]) -> list[Finding]:
    """Check that total generation capacity exceeds peak system demand.

    This implements a basic *capacity adequacy* check --- a standard
    reliability indicator in generation expansion planning (see NERC
    *Probabilistic Adequacy and Measures* reports, IEA *World Energy
    Outlook* methodology, and Billinton & Allan, *Reliability Evaluation
    of Power Systems*).

    The check computes:

    * **Total generation capacity** --- sum of the ``capacity`` (or
      ``pmax``) field of every generator, excluding failure generators
      (``type == "falla"``).
    * **Peak system demand** --- maximum of the per-block total demand
      (sum of all ``lmax`` values at each block).

    Findings:

    * ``CRITICAL`` if capacity < peak demand (capacity deficit).
    * ``WARNING`` if capacity < 1.15 x peak demand (thin reserve margin;
      15 % is a common planning threshold).
    * ``NOTE`` otherwise, reporting the computed adequacy ratio.
    """
    from gtopt_check_json._info import (  # noqa: PLC0415
        _CAPACITY_MARGIN_THRESHOLD,
        compute_indicators,
    )

    findings: list[Finding] = []
    ind = compute_indicators(planning)

    if ind.peak_demand_mw <= 0:
        return findings  # no demand -> nothing to check

    ratio = ind.capacity_adequacy_ratio

    if ratio < 1.0:
        findings.append(
            Finding(
                check_id="capacity_adequacy",
                severity=Severity.CRITICAL,
                message=(
                    f"Capacity deficit: total generation capacity "
                    f"({ind.total_gen_capacity_mw:,.1f} MW) is less than "
                    f"peak demand ({ind.peak_demand_mw:,.1f} MW). "
                    f"Adequacy ratio = {ratio:.3f}"
                ),
                action="Add generation capacity or reduce demand",
            )
        )
    elif ratio < _CAPACITY_MARGIN_THRESHOLD:
        findings.append(
            Finding(
                check_id="capacity_adequacy",
                severity=Severity.WARNING,
                message=(
                    f"Thin reserve margin: total generation capacity "
                    f"({ind.total_gen_capacity_mw:,.1f} MW) is only "
                    f"{ratio:.1%} of peak demand "
                    f"({ind.peak_demand_mw:,.1f} MW). "
                    f"A margin >= 15 % is recommended."
                ),
            )
        )

    return findings


def check_seepage_at_vmin(planning: dict[str, Any]) -> list[Finding]:
    """Check that piecewise reservoir seepage yields q ≈ 0 at vmin.

    PLP's physical seepage curves are designed so that an *empty*
    reservoir has zero filtration: ``q_filt(0) = 0``.  When the first
    segment doesn't satisfy this — i.e. ``constant + slope * vmin ≠ 0``
    — the LP can be forced to discharge non-zero seepage even when the
    reservoir is depleted, which is physically infeasible (no water in
    storage to seep) and tends to make the SDDP forward pass infeasible
    near the lower volume bound.
    """
    findings: list[Finding] = []
    sys = planning.get("system", {})

    reservoir_emin: dict[str, float] = {}
    for r in sys.get("reservoir_array", []):
        emin_vals = _extract_scalar_values(r.get("emin"))
        reservoir_emin[r["name"]] = emin_vals[0] if emin_vals else 0.0

    for idx, seep in enumerate(sys.get("reservoir_seepage_array", [])):
        segments = seep.get("segments")
        if not segments:
            continue

        first = segments[0]
        slope = float(first.get("slope", 0.0))
        constant = float(first.get("constant", 0.0))

        rsv_name = seep.get("reservoir")
        vmin = reservoir_emin.get(rsv_name, 0.0)

        q_at_vmin = constant + slope * vmin
        if abs(q_at_vmin) > 1e-9:
            name = seep.get("name", f"index {idx}")
            findings.append(
                Finding(
                    check_id="seepage_at_vmin",
                    severity=Severity.WARNING,
                    message=(
                        f"Reservoir seepage '{name}' (uid={seep.get('uid')}, "
                        f"reservoir='{rsv_name}'): first segment yields "
                        f"q={q_at_vmin:.4f} m³/s at vmin={vmin} (expected 0); "
                        f"first segment slope={slope}, constant={constant}."
                    ),
                    action=(
                        "Verify plpfilemb.dat: first segment must give "
                        "q=0 at the empty-reservoir level — typically "
                        "constant=0 with the segment starting at vol=0. "
                        "Otherwise the LP forces seepage at vmin and may "
                        "become infeasible during the SDDP forward pass."
                    ),
                )
            )

    return findings


def check_cascade_levels(planning: dict[str, Any]) -> list[Finding]:
    """Check cascade level configuration for consistency.

    Validates:
    - Each level has a ``name``.
    - Level 0 should not have a ``transition`` (nothing to inherit from).
    - ``transition.target_penalty`` > 0 when ``inherit_targets`` is true.
    - ``transition.target_rtol`` in (0, 1] when present.
    - ``sddp_options.max_iterations`` >= 0 in each level.
    - ``sddp_options.convergence_tol`` in (0, 1) when present.
    - Warn if both ``inherit_optimality_cuts`` and ``inherit_targets`` are true.
    """
    findings: list[Finding] = []
    opts = planning.get("options", {})
    cascade = opts.get("cascade_options", {})
    levels = cascade.get("level_array", [])

    if not levels:
        return findings

    for idx, level in enumerate(levels):
        label = level.get("name", f"index {idx}")

        # Each level should have a name
        if not level.get("name"):
            findings.append(
                Finding(
                    check_id="cascade_levels",
                    severity=Severity.WARNING,
                    message=(f"Cascade level {idx}: missing 'name' field"),
                )
            )

        # Level 0 should not have a transition
        transition = level.get("transition")
        if idx == 0 and transition is not None:
            findings.append(
                Finding(
                    check_id="cascade_levels",
                    severity=Severity.WARNING,
                    message=(
                        f"Cascade level '{label}' (index 0): "
                        f"has 'transition' but is the first level "
                        f"(nothing to inherit from)"
                    ),
                )
            )

        if transition is not None:
            inherit_targets = transition.get("inherit_targets", False)
            inherit_cuts = transition.get("inherit_optimality_cuts", False)

            # target_penalty should be > 0 when inherit_targets is true
            target_penalty = transition.get("target_penalty")
            if inherit_targets and target_penalty is not None and target_penalty <= 0:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.WARNING,
                        message=(
                            f"Cascade level '{label}': "
                            f"target_penalty={target_penalty} should be "
                            f"> 0 when inherit_targets is true"
                        ),
                    )
                )

            # target_rtol should be in (0, 1] when present
            target_rtol = transition.get("target_rtol")
            if target_rtol is not None and (target_rtol <= 0 or target_rtol > 1):
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"target_rtol={target_rtol} must be in (0, 1]"
                        ),
                    )
                )

            # Warn if both inherit_optimality_cuts and inherit_targets
            if inherit_cuts and inherit_targets:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.NOTE,
                        message=(
                            f"Cascade level '{label}': both "
                            f"inherit_optimality_cuts and inherit_targets "
                            f"are true (unusual but valid)"
                        ),
                    )
                )

        # sddp_options validation within each level
        sddp = level.get("sddp_options")
        if sddp is not None:
            max_iter = sddp.get("max_iterations")
            if max_iter is not None and max_iter < 0:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"sddp_options.max_iterations={max_iter} "
                            f"must be >= 0"
                        ),
                    )
                )

            conv_tol = sddp.get("convergence_tol")
            if conv_tol is not None and (conv_tol <= 0 or conv_tol >= 1):
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"sddp_options.convergence_tol={conv_tol} "
                            f"must be in (0, 1)"
                        ),
                    )
                )

    return findings
