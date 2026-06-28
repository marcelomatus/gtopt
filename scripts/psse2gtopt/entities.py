"""Typed dataclasses for parsed PSS/E RAW entities.

Each parser in :mod:`psse2gtopt.raw_parser` returns one of these.  They
stay close to the PSS/E semantics (bus numbers, per-unit impedances on
the system MVA base, MW ratings) so :mod:`psse2gtopt.gtopt_writer` can
perform the gtopt translation in one place rather than scattering unit
conversions across the parser.

A PSS/E ``.raw`` is a power-flow file: it carries no economic data
(generator costs, fuel prices).  The writer therefore assigns a single
configurable ``gcost`` to every generator — see
:func:`psse2gtopt.gtopt_writer.build_generators`.
"""

from __future__ import annotations

from dataclasses import dataclass, field


# PSS/E bus type codes (field IDE of the bus record).
BUS_TYPE_PQ = 1  # load bus
BUS_TYPE_PV = 2  # generator / voltage-controlled bus
BUS_TYPE_SLACK = 3  # swing / reference bus
BUS_TYPE_ISOLATED = 4  # disconnected — excluded from the conversion

_BUS_TYPE_TAG: dict[int, str] = {
    BUS_TYPE_PQ: "pq",
    BUS_TYPE_PV: "pv",
    BUS_TYPE_SLACK: "slack",
    BUS_TYPE_ISOLATED: "isolated",
}


@dataclass
class CaseSpec:
    """Top-level case identification (line 1 + the two title lines).

    Attributes:
        sbase: System MVA base (``SBASE``) — the per-unit base every
            reactance is normalised to.  100 MVA in virtually every
            PSS/E case.
        rev: RAW format revision (``REV``) — 32 / 33 / 35.
        base_freq: System frequency in Hz (``BASFRQ``).
        title1: First free-form title line.
        title2: Second free-form title line.
    """

    sbase: float = 100.0
    rev: int = 33
    base_freq: float = 60.0
    title1: str = ""
    title2: str = ""


@dataclass
class BusSpec:
    """A bus (node) record.

    Attributes:
        number: PSS/E integer bus number (unique, used as the gtopt uid).
        name: 12-char bus name (informational).
        base_kv: Nominal voltage [kV].
        ide: PSS/E bus type code (see ``BUS_TYPE_*``).
        area: Area number (1 = Guatemala, 2 = El Salvador, …).
        zone: Zone number.
    """

    number: int
    name: str
    base_kv: float = 0.0
    ide: int = BUS_TYPE_PQ
    area: int = 1
    zone: int = 1

    @property
    def type_tag(self) -> str:
        """Human-readable bus type tag (``pq`` / ``pv`` / ``slack``)."""
        return _BUS_TYPE_TAG.get(self.ide, "pq")

    @property
    def is_isolated(self) -> bool:
        """True for disconnected buses (``IDE == 4``)."""
        return self.ide == BUS_TYPE_ISOLATED


@dataclass
class LoadSpec:
    """A constant-power load record.

    Only the active part (``PL``) is used by the DC OPF; the reactive
    and current/admittance parts are parsed but ignored.
    """

    bus: int
    ident: str
    status: int = 1
    pl: float = 0.0
    ql: float = 0.0


@dataclass
class GenSpec:
    """A generator (machine) record.

    ``pmax`` / ``pmin`` come from the PSS/E ``PT`` / ``PB`` fields;
    ``pg`` is the scheduled set-point (informational).  PSS/E carries
    no cost, so the writer assigns one.
    """

    bus: int
    ident: str
    pg: float = 0.0
    status: int = 1
    pmax: float = 0.0
    pmin: float = 0.0
    mbase: float = 0.0


@dataclass
class BranchSpec:
    """A transmission line / cable record (non-transformer branch).

    ``x`` is the series reactance in per-unit on the system MVA base.
    ``rate_a`` is the normal MVA rating (used as the gtopt ``tmax``).
    """

    from_bus: int
    to_bus: int
    ckt: str
    r: float = 0.0
    x: float = 0.0
    b: float = 0.0
    rate_a: float = 0.0
    rate_b: float = 0.0
    rate_c: float = 0.0
    status: int = 1

    def rating(self, rating_set: str) -> float:
        """Pick the MVA rating for set ``A`` / ``B`` / ``C`` (with fallback)."""
        return _pick_rating(self.rate_a, self.rate_b, self.rate_c, rating_set)


@dataclass
class TransformerSpec:
    """A 2- or 3-winding transformer record.

    All reactances are normalised to the **system** MVA base by the
    parser (PSS/E ``CZ`` impedance-code resolved up front), so the
    writer can emit them as plain per-unit line reactances.

    For a 2-winding transformer only ``bus_k == 0`` and ``x12`` /
    ``rate1`` are meaningful.  For a 3-winding transformer the parser
    fills ``x12`` / ``x23`` / ``x31`` (winding-pair reactances) plus
    the three per-winding ratings; the writer converts them to the
    star equivalent.
    """

    bus_i: int
    bus_j: int
    bus_k: int
    ckt: str
    name: str = ""
    status: int = 1
    windings: int = 2
    x12: float = 0.0
    x23: float = 0.0
    x31: float = 0.0
    # Per-winding normal (A), and emergency (B / C) MVA ratings.
    rate1: float = 0.0
    rate2: float = 0.0
    rate3: float = 0.0
    rate1_b: float = 0.0
    rate2_b: float = 0.0
    rate3_b: float = 0.0
    rate1_c: float = 0.0
    rate2_c: float = 0.0
    rate3_c: float = 0.0

    def winding_rating(self, winding: int, rating_set: str) -> float:
        """MVA rating for winding 1/2/3 under rating set ``A`` / ``B`` / ``C``."""
        a = (self.rate1, self.rate2, self.rate3)[winding - 1]
        b = (self.rate1_b, self.rate2_b, self.rate3_b)[winding - 1]
        c = (self.rate1_c, self.rate2_c, self.rate3_c)[winding - 1]
        return _pick_rating(a, b, c, rating_set)


def _pick_rating(rate_a: float, rate_b: float, rate_c: float, rating_set: str) -> float:
    """Select a rating by set, falling back B/C → A when the chosen set is 0."""
    chosen = {"A": rate_a, "B": rate_b, "C": rate_c}.get(rating_set.upper(), rate_a)
    return chosen if chosen > 0 else rate_a


@dataclass
class PsseCase:
    """A fully parsed PSS/E RAW case."""

    case: CaseSpec = field(default_factory=CaseSpec)
    buses: list[BusSpec] = field(default_factory=list)
    loads: list[LoadSpec] = field(default_factory=list)
    gens: list[GenSpec] = field(default_factory=list)
    branches: list[BranchSpec] = field(default_factory=list)
    transformers: list[TransformerSpec] = field(default_factory=list)
