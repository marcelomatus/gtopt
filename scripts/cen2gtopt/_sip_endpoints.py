# SPDX-License-Identifier: BSD-3-Clause
"""SIP API endpoint catalogue — pinned from
``portal.api.coordinador.cl/swagger/spec/sip.json`` (95 GET endpoints).

The SIP API serves Chile's public operational data: hourly Costo
Marginal Real / Online, real generation, real demand, transmission
limitations, CDC operational instructions, generator/line
catalogues, and the warehouse-style ``/api/v2/recursos/`` family.

Live confirmation status:

* ✅ /costo-marginal-real/v4/findByDate          — live
* ✅ /costo-marginal-online/v4/findByDate        — live
* ✅ /instrucciones-operacionales-cmg/v4/findByDate — live (RO/operating
                                                     state per central)
* ✅ /centrales/v4/findByDate                    — live
* (the rest are spec-pinned but should be covered by the same auth
  scheme; flag any 404/403 at runtime)

Auth: ``?user_key=<key>`` query parameter on every call.
Pagination: every response carries a ``data`` array; many endpoints
also accept ``page`` + ``limit`` query parameters.

Server URL: ``https://sipub.api.coordinador.cl``.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True, frozen=True)
class SipEndpoint:
    """One SIP API endpoint."""

    name: str  # canonical short name used as a dict key
    path: str  # path under the SIP base URL
    params_required: tuple[str, ...] = ()
    params_optional: tuple[str, ...] = ()
    description: str = ""
    confirmed_live: bool = False  # set to True only when we've seen a 200


# Base URL — the 3scale gateway for the Información Pública service.
SIP_BASE_URL = "https://sipub.api.coordinador.cl"


# ----------------------------------------------------------------------
# Marginal cost
# ----------------------------------------------------------------------

COSTO_MARGINAL_REAL = SipEndpoint(
    name="costo_marginal_real",
    path="/costo-marginal-real/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit", "type", "bar_transf"),
    description="Costo Marginal Real (final) — hourly LMP per bus, USD/MWh and CLP/kWh.",
    confirmed_live=True,
)

COSTO_MARGINAL_ONLINE = SipEndpoint(
    name="costo_marginal_online",
    path="/costo-marginal-online/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit", "bar_transf"),
    description="Costo Marginal Online — preliminary hourly LMP per bus.",
    confirmed_live=True,
)

CMG_PROGRAMADO_PCP = SipEndpoint(
    name="cmg_programado_pcp",
    path="/cmg-programado-pcp/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description="Costo Marginal Programado — Programa de Coordinación PCP.",
)

CMG_PROGRAMADO_PID = SipEndpoint(
    name="cmg_programado_pid",
    path="/cmg-programado-pid/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description="Costo Marginal Programado — Programa Intra-Diario PID.",
)

INSTRUCCIONES_OPERACIONALES_CMG = SipEndpoint(
    name="instrucciones_operacionales_cmg",
    path="/instrucciones-operacionales-cmg/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description=(
        "CDC operational instructions for marginal-cost dispatch. Each "
        "row carries `central`, `configuracion`, `despacho` (MW), "
        "`estado` (e.g. 'RO' = Recurso Obligatorio / forced pmin). "
        "Source for master plan §4.11.1 driver #1 (forced pmin) and "
        "#7 (instructed dispatch)."
    ),
    confirmed_live=True,
)


# ----------------------------------------------------------------------
# Generation / demand
# ----------------------------------------------------------------------

GENERACION_REAL = SipEndpoint(
    name="generacion_real",
    path="/generacion-real/v3/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("tipoTecnologia", "idCentral", "page", "pageSize"),
    description="Hourly realised generation per central.",
)

GENERACION_PROGRAMADA = SipEndpoint(
    name="generacion_programada",
    path="/generacion-programada/v3/findAll",
    params_required=("startDate", "endDate"),
    params_optional=("technology", "idCentral", "page", "pageSize"),
    description="Hourly programmed generation per central.",
)

DEMANDA_NETA = SipEndpoint(
    name="demanda_neta",
    path="/demanda-neta/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description="Hourly net demand per bus.",
)

DEMANDA_REAL_ESTIMADA = SipEndpoint(
    name="demanda_real_estimada",
    path="/demanda-real-estimada/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description="Hourly estimated realised demand per bus.",
)


# ----------------------------------------------------------------------
# Topology / catalogue
# ----------------------------------------------------------------------

CENTRALES = SipEndpoint(
    name="centrales",
    path="/centrales/v4/findByDate",
    params_optional=("page", "limit"),
    description="Generator (central) catalogue with ownership and technology.",
    confirmed_live=True,
)

UNIDADES_GENERADORAS = SipEndpoint(
    name="unidades_generadoras",
    path="/unidades-generadoras/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description="Generation-unit catalogue (sub-central level).",
)

LINEAS_TRANSMISION = SipEndpoint(
    name="lineas_transmision",
    path="/lineas-transmision/v4/findByDate",
    params_optional=("page", "limit"),
    description="Transmission-line catalogue with thermal limits.",
)


# ----------------------------------------------------------------------
# Operational constraints / commitment drivers (master plan §4.11)
# ----------------------------------------------------------------------

LIMITACIONES_TRANSMISION = SipEndpoint(
    name="limitaciones_transmision",
    path="/limitaciones-transmision/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description=(
        "CDC transmission-system limitations — operational saturation "
        "declarations. Master plan §4.7 R1 priority-1 source for line "
        "saturation, §6 edge-case 14."
    ),
)

INSTRUCCIONES_OPERACIONALES_SSCC = SipEndpoint(
    name="instrucciones_operacionales_sscc",
    path="/instrucciones-operacionales-sscc/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description=(
        "CDC operational instructions for ancillary services (SSCC). "
        "Master plan §4.11.1 driver #3 (SSCC assignment)."
    ),
)

PROGRAMAS_MANTENIMIENTO_MAYOR = SipEndpoint(
    name="programas_mantenimiento_mayor",
    path="/programas-mantenimiento-mayor/v4/findByDate",
    params_required=("startDate", "endDate"),
    params_optional=("page", "limit"),
    description=(
        "Major-maintenance program. Master plan §4.11.1 driver #5 "
        "(planned maintenance)."
    ),
)

COSTO_COMBUSTIBLE = SipEndpoint(
    name="costo_combustible",
    path="/costo-combustible/v3/findAll",
    params_required=("startDate", "endDate"),
    params_optional=("page",),
    description=(
        "Declared fuel cost per central per day. Schema: nombreCentral, "
        "configuración, empresa, tipoCombustible, costoCombustible "
        "(USD/MWh equivalent for the fuel input). Master plan §4.11.1 "
        "driver #2 (gas inflexible / fuel cost). The 'configuración' "
        "field is the join key against the SIP "
        "instrucciones-operacionales-cmg endpoint. CEN appears to have "
        "stopped publishing through this endpoint after a 2025 cutoff "
        "— call with historical dates (2024-11-01 etc.) to see data."
    ),
    confirmed_live=True,
)


# ----------------------------------------------------------------------
# Index + lookup helpers
# ----------------------------------------------------------------------

ALL_ENDPOINTS: dict[str, SipEndpoint] = {
    e.name: e
    for e in (
        COSTO_MARGINAL_REAL,
        COSTO_MARGINAL_ONLINE,
        CMG_PROGRAMADO_PCP,
        CMG_PROGRAMADO_PID,
        INSTRUCCIONES_OPERACIONALES_CMG,
        GENERACION_REAL,
        GENERACION_PROGRAMADA,
        DEMANDA_NETA,
        DEMANDA_REAL_ESTIMADA,
        CENTRALES,
        UNIDADES_GENERADORAS,
        LINEAS_TRANSMISION,
        LIMITACIONES_TRANSMISION,
        INSTRUCCIONES_OPERACIONALES_SSCC,
        PROGRAMAS_MANTENIMIENTO_MAYOR,
        COSTO_COMBUSTIBLE,
    )
}


def list_confirmed_live() -> list[SipEndpoint]:
    """Endpoints we have actually seen return 200 with real data."""
    return [e for e in ALL_ENDPOINTS.values() if e.confirmed_live]
