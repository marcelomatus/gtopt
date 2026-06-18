"""Unit tests for :mod:`plexos2gtopt._options`.

Pins the legacy semantics of the env-var bridge so a future tweak to
:class:`ConversionOptions` cannot silently change which knobs reach the
deep extractor sites (which all still read ``os.environ[GTOPT_*]``).

Three semantic classes are covered:

1. **Always-export string / int / float** — vert_routing, lift_line_caps,
   nseg_losses, etc.  ``None`` ⇒ skip; otherwise export the formatted value.
2. **Truthy-only boolean** — use_plexos_commit, use_plexos_gen_cap,
   use_plexos_efin, loss_extend_overload.  ``True`` ⇒ write ``"1"``;
   ``False`` and ``None`` ⇒ leave env var ABSENT (matches legacy
   ``if options.get("..."):`` guard).
3. **Presence boolean** — emin_eod_day1, battery_efin_pin.  ``True`` ⇒
   ``"1"``; ``False`` ⇒ ``"0"``; ``None`` ⇒ skip (matches legacy
   ``if "key" in options:`` guard).
"""

from __future__ import annotations

import pytest

from plexos2gtopt._options import ConversionOptions


# ── Constructor: from_options_dict ─────────────────────────────────────────
class TestFromOptionsDict:
    def test_empty_dict_yields_default(self):
        opts = ConversionOptions.from_options_dict({})
        assert opts.vert_routing is None
        assert opts.use_plexos_commit is None
        assert opts.nseg_losses is None
        assert opts.emin_eod_day1 is None

    def test_unknown_keys_silently_dropped(self):
        """Non-conversion knobs (paths, run_check, etc.) must NOT raise."""
        opts = ConversionOptions.from_options_dict(
            {"output_dir": "/tmp/x", "run_check": False, "compare_json": None}
        )
        # All conversion fields stay at default; constructor must not raise.
        assert opts.vert_routing is None

    def test_known_keys_round_trip(self):
        raw = {
            "vert_routing": "cascade",
            "use_plexos_commit": True,
            "nseg_losses": 6,
            "loss_error_pct": 0.05,
            "emin_eod_day1": False,
        }
        opts = ConversionOptions.from_options_dict(raw)
        assert opts.vert_routing == "cascade"
        assert opts.use_plexos_commit is True
        assert opts.nseg_losses == 6
        assert opts.loss_error_pct == 0.05
        assert opts.emin_eod_day1 is False


# ── Env-bridge: install_env() ──────────────────────────────────────────────
class TestInstallEnvAlwaysExport:
    """String / numeric fields export when set, skip when None."""

    def test_string_field_exported(self):
        env: dict[str, str] = {}
        ConversionOptions(vert_routing="cascade").install_env(env)
        assert env == {"GTOPT_VERT_ROUTING": "cascade"}

    def test_int_field_exported_as_int_str(self):
        env: dict[str, str] = {}
        ConversionOptions(nseg_losses=8).install_env(env)
        assert env == {"GTOPT_NSEG_LOSSES": "8"}

    def test_float_field_exported_as_float_str(self):
        env: dict[str, str] = {}
        ConversionOptions(loss_error_pct=0.025).install_env(env)
        assert "GTOPT_LOSS_ERROR_PCT" in env
        assert float(env["GTOPT_LOSS_ERROR_PCT"]) == pytest.approx(0.025)

    def test_none_skips_export(self):
        env: dict[str, str] = {}
        ConversionOptions().install_env(env)
        assert not env


class TestInstallEnvTruthyOnly:
    """``use_plexos_*`` / ``loss_extend_overload`` ⇒ write ``"1"`` only on True."""

    @pytest.mark.parametrize(
        "field,env_var",
        [
            ("use_plexos_commit", "GTOPT_USE_PLEXOS_COMMIT"),
            ("use_plexos_gen_cap", "GTOPT_USE_PLEXOS_GEN_CAP"),
            ("use_plexos_efin", "GTOPT_USE_PLEXOS_EFIN"),
            ("loss_extend_overload", "GTOPT_LOSS_EXTEND_OVERLOAD"),
        ],
    )
    def test_true_writes_one(self, field, env_var):
        env: dict[str, str] = {}
        ConversionOptions(**{field: True}).install_env(env)
        assert env == {env_var: "1"}

    @pytest.mark.parametrize(
        "field",
        [
            "use_plexos_commit",
            "use_plexos_gen_cap",
            "use_plexos_efin",
            "loss_extend_overload",
        ],
    )
    def test_false_does_not_write_env(self, field):
        """Legacy ``if options.get("..."):`` only set the var on truthy."""
        env: dict[str, str] = {}
        ConversionOptions(**{field: False}).install_env(env)
        assert not env, (
            f"truthy-only flag {field}=False must leave env-var ABSENT so "
            f"the consumer's os.environ.get(KEY, '0') fallback applies"
        )


class TestInstallEnvPresenceBoolean:
    """``emin_eod_day1`` / ``battery_efin_pin`` ⇒ write "0"/"1" on explicit bool."""

    def test_true_writes_one(self):
        env: dict[str, str] = {}
        ConversionOptions(emin_eod_day1=True, battery_efin_pin=True).install_env(env)
        assert env == {
            "GTOPT_EMIN_EOD_DAY1": "1",
            "GTOPT_BATTERY_PIN_EFIN": "1",
        }

    def test_false_writes_zero(self):
        env: dict[str, str] = {}
        ConversionOptions(emin_eod_day1=False, battery_efin_pin=False).install_env(env)
        assert env == {
            "GTOPT_EMIN_EOD_DAY1": "0",
            "GTOPT_BATTERY_PIN_EFIN": "0",
        }

    def test_none_skips(self):
        env: dict[str, str] = {}
        ConversionOptions(emin_eod_day1=None, battery_efin_pin=None).install_env(env)
        assert not env


# ── Integration: full mapping table coverage ──────────────────────────────
class TestFullMappingTable:
    """Every documented (option_name, GTOPT_VAR) edge from the legacy
    bridge is covered by the dataclass."""

    def test_all_legacy_env_vars_reachable(self):
        """Every ``GTOPT_*`` env var the legacy code wrote MUST be
        reachable from at least one :class:`ConversionOptions` field."""
        # Build an options dict that sets every known field to a truthy
        # value, then check the install_env output contains every
        # legacy env-var name.
        full = ConversionOptions(
            vert_routing="ocean",
            reservoir_spillway="basic",
            spill_fcost=3.6,
            spill_fcost_scale=1.0,
            emin_eod_day1=True,
            battery_efin_pin=True,
            hydro_min_mode="soft",
            nseg_losses=6,
            loss_error_pct=0.01,
            loss_extend_overload=True,
            loss_pwl_layout="dynamic",
            loss_tangent_lines="L1,L2",
            nseg_tangent=6,
            nseg_uniform=4,
            lift_line_caps="A->B",
            no_lift_lines="C->D",
            el0_lines="extended",
            use_plexos_commit=True,
            use_plexos_gen_cap=True,
            use_plexos_efin=True,
        )
        env: dict[str, str] = {}
        full.install_env(env)
        legacy_vars = {
            "GTOPT_VERT_ROUTING",
            "GTOPT_USE_PLEXOS_COMMIT",
            "GTOPT_USE_PLEXOS_GEN_CAP",
            "GTOPT_USE_PLEXOS_EFIN",
            "GTOPT_LIFT_LINE_CAPS",
            "GTOPT_NO_LIFT_LINES",
            "GTOPT_EL0_LINES",
            "GTOPT_RESERVOIR_SPILL",
            "GTOPT_SPILL_FCOST",
            "GTOPT_SPILL_FCOST_SCALE",
            "GTOPT_NSEG_LOSSES",
            "GTOPT_LOSS_ERROR_PCT",
            "GTOPT_LOSS_EXTEND_OVERLOAD",
            "GTOPT_LOSS_PWL_LAYOUT",
            "GTOPT_HYDRO_MIN_MODE",
            "GTOPT_LOSS_TANGENT_LINES",
            "GTOPT_NSEG_TANGENT",
            "GTOPT_NSEG_UNIFORM",
            "GTOPT_EMIN_EOD_DAY1",
            "GTOPT_BATTERY_PIN_EFIN",
        }
        assert set(env.keys()) == legacy_vars

    def test_install_env_does_not_clobber_unrelated_keys(self):
        """``install_env`` writes only the bridged keys."""
        env: dict[str, str] = {"SHELL": "/bin/bash", "USER": "x"}
        ConversionOptions(vert_routing="cascade").install_env(env)
        assert env["SHELL"] == "/bin/bash"
        assert env["USER"] == "x"
        assert env["GTOPT_VERT_ROUTING"] == "cascade"
