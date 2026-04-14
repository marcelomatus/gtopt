# -*- coding: utf-8 -*-

"""Shared base class for the Laja and Maule agreement transforms.

Both :class:`gtopt_expand.laja_agreement.LajaAgreement` and
:class:`gtopt_expand.maule_agreement.MauleAgreement` follow the same
template-driven pipeline:

1. Ingest a canonical JSON config dict.
2. Build a template context (``_prepare_context``, subclass-specific).
3. Render a ``.tson`` template → ``flow_right_array`` /
   ``volume_right_array`` / ``user_constraint_array`` entity lists.
4. Assign unique UIDs to the rendered entities.
5. Render the companion ``.tampl`` file on demand.

Everything except ``_prepare_context`` (and the per-agreement schedule
helpers) is identical between the two classes, so it lives here.

Subclasses must set two class attributes:

* ``_ARTIFACT`` — short name used for ``<artifact>.tson`` /
  ``<artifact>.tampl`` / ``<artifact>.pampl`` lookups (e.g. ``"laja"``).
* ``_UID_START`` — starting integer for the monotonic UID counter
  (e.g. ``1000`` for Maule, ``2000`` for Laja, so the two agreements
  never collide when merged into the same system).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jinja2

from gtopt_expand._template_engine import (
    _TEMPLATE_DIR,
    render_tson,
)


class _RightsAgreementBase:
    """Common scaffolding for irrigation agreement → gtopt entity transforms."""

    #: short name used for ``<artifact>.tson`` / ``.tampl`` / ``.pampl`` lookups
    _ARTIFACT: str = ""
    #: starting value for the monotonic per-instance UID counter
    _UID_START: int = 0

    def __init__(
        self,
        config: dict[str, Any],
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
    ):
        if not self._ARTIFACT:
            raise TypeError(f"{type(self).__name__} must set _ARTIFACT class attribute")
        self._cfg = config
        self._stage_parser = stage_parser
        self._options = options or {}
        self._uid_counter = self._UID_START

        self.flow_rights: list[dict[str, Any]] = []
        self.volume_rights: list[dict[str, Any]] = []
        self.user_constraints: list[dict[str, Any]] = []

        self._build()

    # -------------------------------------------------------- template naming

    def _template_name(self) -> str:
        """Return the template stem used for ``.tson`` / ``.tampl`` lookup.

        Subclasses that support multiple variants may override this to
        return a variant-specific stem (e.g. ``maule_machicura``).  The
        default is the class-level ``_ARTIFACT`` attribute.
        """
        return self._ARTIFACT

    @classmethod
    def from_json(
        cls,
        json_path: Path | str,
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
    ) -> _RightsAgreementBase:
        """Load the canonical JSON file and construct an agreement instance."""
        with open(json_path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
        return cls(cfg, stage_parser=stage_parser, options=options)

    # ------------------------------------------------------------------ core

    def _next_uid(self) -> int:
        """Return a fresh monotonic UID for a rights entity."""
        uid = self._uid_counter
        self._uid_counter += 1
        return uid

    def _get_stages(self) -> list[dict[str, Any]]:
        """Return the effective list of stages, truncated to match options."""
        if self._stage_parser is None:
            return []
        stages = self._stage_parser.get_all()
        last_stage = self._options.get("last_stage", -1)
        try:
            last_stage = int(last_stage)
        except (ValueError, TypeError):
            last_stage = -1
        if last_stage > 0:
            stages = [s for s in stages if s["number"] <= last_stage]
        return stages

    def _stage_block_counts(self, nstages: int) -> list[int]:
        """Return a per-stage ``count_block`` list of length ``nstages``.

        When the stage parser carries ``count_block`` metadata (as it does
        when ``--stages`` points at a full plp2gtopt planning JSON), each
        stage's own block count is used — so the emitted schedule is
        rectangular across blocks *within* a stage and matches the LP's
        ``(stage_idx, block_idx)`` lookup layout even when different stages
        have different block counts.

        When ``count_block`` is missing, falls back to the uniform
        ``blocks_per_stage`` option (default ``1``).  Either way the
        returned list has exactly ``nstages`` entries; trailing stages
        that the stage parser does not cover reuse the last known count,
        or the option default when the parser is empty.
        """
        uniform = int(self._options.get("blocks_per_stage", 1) or 1)
        stages = self._get_stages() if self._stage_parser is not None else []
        counts: list[int] = []
        for idx in range(nstages):
            if idx < len(stages):
                raw = stages[idx].get("count_block")
                try:
                    cb = int(raw) if raw is not None else uniform
                except (TypeError, ValueError):
                    cb = uniform
                counts.append(max(cb, 1))
            else:
                counts.append(uniform)
        return counts

    def _to_stb_sched(
        self,
        values: list[float],
    ) -> float | list[list[list[float]]]:
        """Convert per-stage values to STBRealFieldSched (3D) or scalar.

        Returns scalar 0.0 on empty input, the single value if all stages
        match, otherwise a ``[[[v]*nblocks for v in values]]`` wrapped 3D
        list compatible with ``FieldSched<Real, vector<vector<vector<Real>>>>``.

        The inner block dimension uses each stage's own ``count_block``
        (from the stage parser) when available, so the emitted list is
        ragged-compatible with the LP's per-stage block-count layout.
        """
        if not values:
            return 0.0
        if len(set(values)) == 1:
            return values[0]
        counts = self._stage_block_counts(len(values))
        return [[[v] * nb for v, nb in zip(values, counts)]]

    def _to_tb_sched(
        self,
        values: list[float],
    ) -> float | list[list[float]]:
        """Convert per-stage values to TBRealFieldSched (2D) or scalar.

        Returns scalar 0.0 on empty input, the single value if all stages
        match, otherwise a ``[[v]*nblocks for v in values]`` 2D list
        compatible with ``FieldSched<Real, vector<vector<Real>>>``.

        The inner block dimension uses each stage's own ``count_block``
        (from the stage parser) when available — this ensures the LP's
        ``(stage_idx, block_idx)`` lookup never walks off the inner
        vector when the planning has multiple blocks per stage.
        """
        if not values:
            return 0.0
        if len(set(values)) == 1:
            return values[0]
        counts = self._stage_block_counts(len(values))
        return [[v] * nb for v, nb in zip(values, counts)]

    # ----------------------------------------------------- subclass contract

    def _prepare_context(self) -> dict[str, Any]:
        """Return the template-rendering context.

        Subclasses must override to pre-compute all dynamic values
        (schedules, segments, cost modulations) that the ``.tson``
        template references via ``{{ param }}`` substitution.
        """
        raise NotImplementedError

    # ---------------------------------------------------------- entity build

    def _assign_uids(self) -> None:
        """Assign unique UIDs to all entities after template rendering."""
        for entity_list in (
            self.flow_rights,
            self.volume_rights,
            self.user_constraints,
        ):
            for entity in entity_list:
                entity["uid"] = self._next_uid()

    def _build(self) -> None:
        """Build all rights entities from the parsed configuration.

        Renders the ``<artifact>.tson`` template with the subclass-supplied
        context values and assigns unique UIDs to all rendered entities.
        """
        context = self._prepare_context()
        entities = render_tson(f"{self._template_name()}.tson", context)

        self.flow_rights = entities.get("flow_right_array", [])
        self.volume_rights = entities.get("volume_right_array", [])
        self.user_constraints = entities.get("user_constraint_array", [])

        self._assign_uids()

    # -------------------------------------------------------------- emission

    def generate_pampl(self, output_path: Path) -> str:
        """Render the ``<artifact>.tampl`` template and write the PAMPL file.

        Args:
            output_path: Directory where the ``.pampl`` file will be written.

        Returns:
            Filename of the generated ``.pampl`` file (relative name only).
        """
        # NOTE: do NOT reuse `create_template_env` here.  That environment
        # applies `finalize=_json_finalize`, which is correct for `.tson`
        # templates (rendered as JSON) but wrong for `.tampl` templates,
        # which expect bare AMPL identifiers.  Keep a separate env.
        env = jinja2.Environment(
            loader=jinja2.FileSystemLoader(str(_TEMPLATE_DIR)),
            keep_trailing_newline=True,
            undefined=jinja2.StrictUndefined,
        )
        template = env.get_template(f"{self._template_name()}.tampl")

        rendered = template.render(self._cfg)

        output_path = Path(output_path)
        output_path.mkdir(parents=True, exist_ok=True)
        pampl_name = f"{self._ARTIFACT}.pampl"
        pampl_file = output_path / pampl_name
        pampl_file.write_text(rendered, encoding="utf-8")

        return pampl_name

    def to_json_dict(
        self,
        output_dir: Path | None = None,
    ) -> dict[str, Any]:
        """Return all entities as a dict of arrays for system JSON.

        Args:
            output_dir: If provided, generates a .pampl file in this
                directory and sets ``user_constraint_file`` instead of
                embedding constraints in ``user_constraint_array``.
        """
        result: dict[str, Any] = {}
        if self.flow_rights:
            result["flow_right_array"] = self.flow_rights
        if self.volume_rights:
            result["volume_right_array"] = self.volume_rights
        if self.user_constraints:
            if output_dir is not None:
                result["user_constraint_file"] = self.generate_pampl(output_dir)
            else:
                result["user_constraint_array"] = self.user_constraints
        return result
