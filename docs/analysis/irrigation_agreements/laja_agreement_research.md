# Convenio del Laja -- Research Summary

> **See also**: [Laja Agreement Template](../../../scripts/plp2gtopt/templates/laja.md) —
> complete gtopt entity specifications with basin topology diagrams, LaTeX
> formulations, and embedded JSON/PAMPL template code.

## Overview

The "Convenio del Laja" (Laja Irrigation Agreement) is a water management convention
signed in 1958 between the Direccion de Riego (now Direccion de Obras Hidraulicas,
DOH) and the Empresa Nacional de Electricidad (ENDESA), governing the use of water
from Laguna del Laja for both hydroelectric generation and agricultural irrigation.
It is one of the most important water allocation agreements in Chile, regulating the
largest reservoir in the country.

The agreement has been modified multiple times since the late 1990s, and a permanent
replacement convention was signed in November 2017 between the Ministry of Public
Works (MOP) and Enel Generacion Chile (successor to Endesa).

---

## 1. Historical Context

### The Laja River Basin

Laguna del Laja is a natural lake located at approximately 1,365 meters above sea
level (m.s.n.m.) in the Antuco commune, Biobio Region, in the Andes foothills of
central Chile. It is the largest reservoir in the country, with a storage capacity
exceeding 6,000 million cubic meters (6 billion m3), and is the only reservoir in
Chile with multi-year regulation capacity.

### Early Hydroelectric Development

- **1948**: Central Hidroelectrica Abanico entered operation. This was the first
  hydroelectric intervention on the lake. The run-of-river plant (136 MW) receives
  water from the lake through a drainage tunnel (tunel de vaciado).

- **1958**: The Convenio del Laja was signed between the Direccion de Riego and
  ENDESA (state-owned at the time) to manage water resources in the Laja basin.
  The agreement decreed the construction of a dam/reservoir infrastructure on Lake
  Laja to ensure irrigation for agriculture in the lower basin zones, and assigned
  ENDESA the responsibility of administering the reservoir.

- **1963**: A drainage tunnel (tunel de vaciado) was constructed, allowing artificial
  extraction of water and lowering the lake level. By 1971, the level had dropped to
  approximately 1,324 m.s.n.m.

- **1973**: Central Hidroelectrica El Toro entered operation. This is a reservoir-type
  plant with 450 MW capacity (4 Pelton turbine units), located at the headwaters of
  the Laja River. Water is conducted through an 8 km tunnel with a 610 m drop to the
  powerhouse on the banks of the Polcura River. Average annual generation: ~1,686 GWh.

- **1977**: Captacion Alto Polcura was built, diverting water from the Polcura River
  basin into Laguna del Laja to increase inflows.

- **1981**: Central Hidroelectrica Antuco entered operation (run-of-river).

### The Laja Hydroelectric System

The complete Laja hydroelectric system comprises five power plants:

| Plant     | Type        | Capacity (MW) | Year |
|-----------|-------------|---------------|------|
| El Toro   | Reservoir   | 450           | 1973 |
| Abanico   | Run-of-river| 136           | 1948 |
| Antuco    | Run-of-river| 320           | 1981 |
| Rucue     | Run-of-river| 178           | 1998 |
| Quilleco  | Run-of-river| 70            | 2007 |

**Total installed capacity: ~1,150 MW**

This system is one of the most important in the Chilean interconnected system (SIC,
now SEN). The lake represents approximately 20% of Enel's hydroelectric production
in Chile.

### Privatization and Conflict

In 1989, during the military dictatorship, ENDESA was privatized and acquired by
a Spanish company (later Enel). The new private owner interpreted the 1958
convention's conditions more liberally regarding water extraction, leading to
decades of conflict with irrigators (canalistas).

The Asociacion de Canalistas del Laja, founded in 1916, represents approximately
90,000 hectares of irrigated farmland downstream. The conflict pitted hydroelectric
interests against agricultural needs in one of Chile's most productive farming
regions.

---

## 2. The 1958 Convention -- Key Provisions

The original 1958 agreement established the following framework:

### Water Allocation Rights

- **Irrigators**: Right to 90 m3/s (cubic meters per second) of flow to irrigate
  90,000 hectares during the 9-month irrigation season (September through April).

- **ENDESA**: Can extract up to 57 m3/s as an annual average for hydroelectric
  generation.

### Reservoir Management

- **Administration**: ENDESA holds control of valves and gates (compuertas) and is
  responsible for delivering the necessary water flows.

- **Reserve volume zone (colchon de reserva)**: 500 million m3 of water that cannot
  be extracted under any circumstances. This serves as a safety buffer to ensure
  minimum lake levels.

- **Minimum elevation**: Extractions cannot bring the lake below a critical
  elevation (approximately 1,305.27 m.s.n.m. was later referenced as a floor).

- **Flow delivery obligation**: If natural flow is insufficient to meet irrigation
  needs, ENDESA must open the gates to deliver the deficit.

### Volume Savings Mechanism

Both the irrigation department and ENDESA can execute volume savings ("ahorros de
volumen"), meaning they can bank certain water volumes for later use. However, if
overflow occurs, the lost water is counted against the saved volumes.

---

## 3. Modifications and Flexibilizations

The 1958 convention has been modified at least five times since the late 1990s:

### 1998 Flexibilization (Major)

Due to a severe drought in 1997-1998, the reservoir reached its lowest level in
50 years. ENDESA and the DOH signed a flexibilization to allow the company to
extract more water. This was the first major modification and set a precedent for
subsequent changes.

### Post-1998 Modifications

Five additional modifications were made after the 1998 flexibilization. These
generally allowed more flexible water extraction rules, adapting to changing
climatic conditions and drought periods.

### 2012-2013 Season Flexibilization

A specific flexibilization for the 2012-2013 irrigation season was signed between
the DOH and Endesa. This modification stipulated that under no circumstances could
extractions for generation bring the lake below elevation 1,305.27 m.s.n.m. -- a
condition that was reportedly violated.

### 2013 -- Peak of Conflict

In October 2013, the dispute between canalistas and Endesa reached critical
levels:

- The Asociacion de Canalistas del Laja publicly accused Endesa of excessive
  water extraction.
- Endesa opted for judicial proceedings rather than negotiation.
- Irrigators filed a complaint with the Contraloria General de la Republica
  (national comptroller) alleging illegal flexibilizations.

### 2014 -- Contraloria Investigation

The Contraloria investigated the legality of the flexibilizations, examining
whether the DOH had followed proper legal procedures in modifying the 1958
convention. The investigation put the entire agreement framework at risk.

Starting in 2014, annual temporary agreements were signed. One notable provision
was that Enel (formerly Endesa) agreed to stop operating the El Toro plant
(450 MW) in the months prior to snowmelt to help restore reservoir volumes.

### December 2015 -- Tripartite Agreement

The ministers of Public Works, Energy, and Agriculture, together with Endesa Chile
representatives and irrigation associations, signed a convention for the recovery
and efficient use of Lago Laja. Key numbers:

- 700 hm3 (hectometers cubic) fixed extraction allowed
- 200 hm3 variable extraction
- 300 hm3 allocated to irrigation
- 300 hm3 allocated to energy
- 100 hm3 for combined/shared use

### 2015 -- El Toro Shutdown

Due to the crisis, Endesa announced it would paralyze operations at Central El Toro
in 2015 after reaching agreement with agricultural associations.

### December 2016 -- Transitory Modification

A transitory modification of the 1958 convention was signed. The Canalistas del
Laja publicly called it a failure, arguing it did not adequately protect irrigation
rights.

---

## 4. The 2017 Permanent Convention

In November 2017, the Ministry of Public Works (MOP) and Enel Generacion Chile
signed a permanent convention for the operation and recovery of the Laja Reservoir.
This agreement:

### Development Process

- A technical commission with representatives from DOH, Enel, and various farmer
  associations worked for almost three years.
- Multiple proposals for new operating rules were evaluated through reservoir
  operation simulation models.
- The agreement was ratified by irrigation associations and approved by all
  members of the working table.

### Key Features

1. **Level-based operation**: The manner of water use is determined by the
   reservoir's water level (cota). The lake level determines whether generation or
   irrigation takes priority at any given time.

2. **Monthly flexibility**: Unlike the 1958 agreement (which used rigid annual
   allocations), the new convention allows monthly programming of withdrawals.

3. **Salto del Laja waterfall protection**: Irrigators assured delivery of a
   minimum flow to maintain the Salto del Laja waterfall, a major tourist
   attraction. A minimum flow of approximately 10 m3/s was established for the
   waterfall. (Actual flow as of recent measurements: ~22 m3/s.)

4. **Recovery reserve**: Users agreed to reserve water volumes for lake recovery,
   with larger reservations required when the lake is more depleted.

5. **Ecosystem services**: The agreement includes recognition of ecosystem
   services and users without formal water use rights (Derechos de
   Aprovechamiento de Aguas).

6. **Climate change adaptation**: The framework explicitly incorporates
   adaptation to climate change conditions.

7. **Coverage**: Ensures irrigation for 117,000 hectares while allowing operation
   of ~1,150 MW of hydroelectric capacity.

---

## 5. Comparison with Convenio del Maule (1947)

Chile has a similar agreement for the Laguna del Maule:

| Feature          | Laja (1958)                        | Maule (1947)                    |
|------------------|------------------------------------|---------------------------------|
| Date             | 1958                               | 1947                            |
| Parties          | DOH + ENDESA                       | DOH + ENDESA                    |
| Priority         | Shared (contested)                 | Irrigation priority (confirmed) |
| State ownership  | ~80%/20% (estimated)               | ~80% State / 20% ENDESA        |
| Court ruling     | No definitive ruling               | 2021 Supreme Court: irrigation  |
| Investigative    | Not formally concluded for Laja    | 2023 Camara de Diputados        |
| commission       |                                    | concluded no breach             |

The Maule 1947 convention was confirmed by the Chilean Supreme Court in 2021: water
use in Laguna del Maule is a priority for irrigation, with only excess water
available for electricity generation.

---

## 6. Environmental Impact

### Lake Level Decline

According to Nunez et al. (2018) in Dialogo Andino (SciELO):

- Precipitation in the basin decreased from 1,500-2,000 mm to 970-780 mm annually.
- Temperature increased by 1.3-1.6 degrees C.
- The lake's flooded surface area decreased from 9,994.63 hectares (1985) to
  5,135.6 hectares (2013) -- a 48.6% reduction.
- The researchers concluded that the retreat "cannot be explained by analyzed
  climatic variations" alone, inferring that the main external cause is anthropic
  action -- pressure and competition for water among actors with unequal power
  relations.

### Critical Levels

- The lake has reached deficit levels exceeding 80% of capacity in drought years.
- In 2020, the lake presented a 61% deficit.
- Historical minimum levels have approached ~1,305 m.s.n.m.

---

## 7. Relevance to Power System Planning (GTEP/PLP)

The Convenio del Laja directly impacts long-term power system planning models:

### UChile Thesis (2016)

**Title**: "Efecto del convenio de riego del sistema hidroelectrico Laja sobre la
programacion de largo plazo del sistema interconectado central de Chile"

**Repository**: http://repositorio.uchile.cl/handle/2250/139279

**Key finding**: The SIC (Central Interconnected System) is so large that adding
a constraint affecting only one of its reservoirs does not considerably alter
system-level operation, but it can significantly alter the local environment at
the basin level.

### Modeling Implications

- The convention imposes operational constraints on the Laja reservoir that must
  be modeled in long-term planning tools (PLP/SDDP).
- Water allocation rules between irrigation and generation affect dispatch
  optimization.
- The level-based operating rules in the 2017 convention require modeling
  reservoir level as a state variable with conditional constraints.
- Multi-year regulation capacity of Laguna del Laja makes it unique in the
  Chilean system and critical for drought management.

### Coordinador Electrico Nacional

The Coordinador Electrico Nacional monitors Laja reservoir levels in real-time
and incorporates the convention's restrictions into its operational planning:
- Real-time cota data: https://www.coordinador.cl/operacion/graficos/operacion-real/cotas-y-niveles-de-embalses-reales/
- Operational programming: https://www.coordinador.cl/operacion/graficos/operacion-programada/

---

## 8. Key Sources and URLs

### Primary Sources

1. **Enel press release (2017 Convention)**:
   https://www.enel.cl/es/conoce-enel/prensa/press-enel-generacion/d201711-ministerio-de-obras-publicas-y-enel-generacion-chile-firman-convenio-permanente-para-operacion-y-recuperacion-del-embalse-laja.html

2. **Asociacion de Canalistas del Laja**:
   http://www.canalistasdellaja.cl/?p=274

3. **Camara de Diputados -- DOH Document (ORD. DOH IM 7343)**:
   https://www.camara.cl/verDoc.aspx?prmID=43050&prmTIPO=DOCUMENTOCOMISION

4. **Camara de Diputados -- Investigative Commission (Maule, 2023)**:
   https://www.camara.cl/cms/noticias/2023/07/27/comision-investigadora-sobre-convenio-de-riego-endesa-aprobo-conclusiones/

### Academic Sources

5. **UChile Thesis -- "Efecto del convenio de riego del sistema hidroelectrico Laja
   sobre la programacion de largo plazo del SIC"** (2016):
   http://repositorio.uchile.cl/handle/2250/139279

6. **Nunez et al. (2018) -- "Presion hidrica en ambientes lacustres de alta montana:
   Entre el cambio climatico y el desarrollo energetico. Laguna del Laja, Chile"**
   (Dialogo Andino, SciELO):
   https://www.scielo.cl/scielo.php?script=sci_arttext&pid=S0719-26812018000100143

7. **Hrudnick (UC) -- Cuenca del Laja**:
   http://hrudnick.sitios.ing.uc.cl/alumno15/rieg/claja.html

8. **UChile -- "Tradeoffs entre hidroelectricidad y riego en un sistema electrico
   hidrotermico multi-cuenca"**:
   https://repositorio.uchile.cl/handle/2250/165722

### News and Analysis

9. **La Tercera -- "La disputa que enfrenta a canalistas y Endesa por uso de
   laguna del Laja"** (Oct 2013):
   https://www.latercera.com/noticia/la-disputa-que-enfrenta-a-canalistas-y-endesa-por-uso-de-laguna-del-laja/

10. **Revista Electricidad -- same article** (Oct 2013):
    https://www.revistaei.cl/2013/10/21/la-disputa-que-enfrenta-a-canalistas-y-endesa-por-uso-de-laguna-del-laja/

11. **El Desconcierto -- "Conflictos por el agua: El caso de El Laja"** (May 2014):
    https://eldesconcierto.cl/2014/05/30/conflictos-por-el-agua-el-caso-de-el-laja

12. **Derecho al Agua -- same article**:
    https://www.derechoalagua.cl/2014/05/conflictos-por-el-agua-el-caso-de-el-laja/

13. **BioBioChile -- Contraloria investigation** (Jan 2014):
    https://www.biobiochile.cl/noticias/2014/01/05/en-riesgo-convenio-sobre-uso-del-lago-laja-por-investigacion-de-contraloria.shtml

14. **BioBioChile -- Canalistas criticism of 2016 modification**:
    https://www.biobiochile.cl/noticias/nacional/region-del-bio-bio/2016/12/27/canalistas-del-laja-tildaron-de-fracaso-firma-de-modificacion-transitoria-de-convenio-de-1958.shtml

15. **Mundoagro -- "El acuerdo de operacion del Lago Laja: ejemplo de coordinacion
    entre agricultura y energia"**:
    https://mundoagro.cl/el-acuerdo-de-operacion-del-lago-laja-ejemplo-de-coordinacion-entre-agricultura-y-energia/

16. **Mundoagro -- CNR ratification**:
    https://mundoagro.io/cl/consejo-de-ministros-de-cnr-ratifico-convenio-para-uso-eficiente-de-aguas-del-lago-laja/

17. **Diario Financiero -- Enel ends 10-year dispute**:
    https://www.df.cl/empresas/energia/enel-pone-fin-a-mas-de-10-anos-de-disputas-con-regantes-del-laja-por-uso

18. **CodeXVerde -- El Toro shutdown agreement** (2015):
    http://codexverde.cl/tras-acuerdo-con-agricultores-endesa-anuncia-que-en-2015-paralizara-operaciones-de-hidroelectrica-el-toro/

19. **Terram -- Endesa judicial route** (Nov 2013):
    https://www.terram.cl/2013/11/endesa_opta_por_via_judicial_en_conflicto_con_regantes_del_laja/

### Historical / Infrastructure

20. **Memoria Chilena -- El Toro Central Hidroelectrica y Sistema de Transmision (PDF)**:
    https://www.memoriachilena.gob.cl/archivos2/pdfs/MC0037324.pdf

21. **Memoria Chilena -- Central Hidroelectrica Antuco (PDF)**:
    https://www.memoriachilena.gob.cl/archivos2/pdfs/MC0037325.pdf

22. **Paisajeo -- "Clepsidra del Rio Laja" (infrastructure analysis)**:
    https://www.paisajeo.com/post/2019/12/16/clepsidra-del-r%C3%ADo-laja-infraestructura-hidr%C3%A1ulica-adaptativa-para-el-sistema-hidrol%C3%B3gic

23. **Wikipedia -- Laguna del Laja**:
    https://es.wikipedia.org/wiki/Laguna_de_La_Laja

### Real-Time Data

24. **Coordinador Electrico Nacional -- Cotas y Niveles Reales**:
    https://www.coordinador.cl/operacion/graficos/operacion-real/cotas-y-niveles-de-embalses-reales/

25. **Coordinador Electrico Nacional -- Operacion Programada**:
    https://www.coordinador.cl/operacion/graficos/operacion-programada/

---

## 9. Downloadable Documents

The following PDFs are available for download but could not be fetched automatically:

1. **El Toro Central Hidroelectrica** (Memoria Chilena):
   https://www.memoriachilena.gob.cl/archivos2/pdfs/MC0037324.pdf

2. **Central Hidroelectrica Antuco** (Memoria Chilena):
   https://www.memoriachilena.gob.cl/archivos2/pdfs/MC0037325.pdf

3. **DOH Official Response Document (ORD. DOH IM 7343)** to Camara de Diputados:
   https://www.camara.cl/verDoc.aspx?prmID=43050&prmTIPO=DOCUMENTOCOMISION

4. **UChile Thesis PDF** (full text):
   http://repositorio.uchile.cl/handle/2250/139279
   (Look for the PDF download link on the repository page)

5. **BCN Parliamentary Record**:
   https://www.bcn.cl/laborparlamentaria/participacion?idParticipacion=1317037

---

## 10. Notes on Original Agreement Text

The original text of the 1958 Convenio del Laja does not appear to be publicly
available online as a scanned document. The most likely sources for obtaining
the original text would be:

- **Direccion de Obras Hidraulicas (DOH)** -- as a party to the agreement
- **Enel Generacion Chile** -- as successor to ENDESA
- **Biblioteca del Congreso Nacional (BCN)** -- legislative archives
- **Contraloria General de la Republica** -- examined the agreement during
  the 2014 investigation
- **Asociacion de Canalistas del Laja** -- as a stakeholder

The DOH document submitted to the Camara de Diputados (source #3 above) may
contain excerpts or references to the original agreement text.

---

## 11. Current Status (2020-2025)

### Reservoir Deficit Levels

The prolonged drought affecting central-south Chile (megasequía) has kept
Lago Laja at critically low levels:

| Date          | Deficit Level |
|---------------|---------------|
| Feb 2017      | 80.4%         |
| Various years | 82.56%        |
| Dec 2020      | 61%           |
| 2022          | Coordinador formed hydric reserve: 104.63 GWh accumulated |
| Apr 2025      | Biobío region reservoirs at 39% of capacity |

### Coordinador Electrico Nacional Role

The Coordinador Electrico Nacional monitors Laja reservoir levels and
incorporates the convention's restrictions into dispatch optimization. In
2022, the Coordinador requested flexibilization of hydric reserves in
reservoirs following rains and snowmelt forecasts, coordinating the
formation of reserves with Lago Laja accumulating 104.63 GWh.

The Coordinador's operational summaries regularly report on Laja reservoir
levels as part of the national power system daily operation reports:
- https://www.coordinador.cl/wp-content/uploads/2024/09/Resumen-Ejecutivo-de-Operacion-05-09-2024.pdf

### Additional Sources Found (2026 Update)

26. **BioBioChile -- Optimismo para cambiar convenio 1958** (Aug 2014):
    https://www.biobiochile.cl/noticias/2014/08/07/optimismo-existe-para-cambiar-acuerdo-del-convenio-de-1958-del-lago-laja.shtml

27. **Diario Concepcion -- Lago Laja 61% deficit** (Dec 2020):
    https://www.diarioconcepcion.cl/economia/2020/12/12/lago-laja-mayor-embalse-natural-de-chile-presenta-un-61-de-deficit.html

28. **BioBioChile -- Lago Laja 80.4% deficit** (Feb 2017):
    https://www.biobiochile.cl/noticias/nacional/region-del-bio-bio/2017/02/07/lago-laja-vive-momento-critico-presenta-un-deficit-hidrico-de-un-804.shtml

29. **Revista Electricidad -- Embalses en estado critico (82.56%)**:
    https://www.revistaei.cl/embalses-generacion-hidroelectrica-estado-critico-laguna-del-laja-deficit-8256/

30. **TVU -- Gobierno y Enel firman acuerdo** (Nov 2017):
    https://www.tvu.cl/prensa/tvu-noticias/2017/11/17/gobierno-y-enel-firman-acuerdo-sobre-uso-de-aguas-del-lago-laja.html

31. **El Ciudadano -- Se retrasa fin del convenio (MOP y Enel)**:
    https://www.elciudadano.com/medio-ambiente/se-retrasa-fin-del-convenio-sobre-uso-de-lago-laja-mop-y-enel-no-llegan-a-acuerdo/05/06/

32. **Diario Concepcion -- Recurso hidrico Biobio al 39%** (Apr 2025):
    https://www.diarioconcepcion.cl/economia/2025/04/06/recurso-hidrico-para-sistemas-de-riego-en-el-biobio-principales-embalses-al-39-de-su-capacidad.html

33. **Terram -- Lago Laja en minimos historicos**:
    https://www.terram.cl/lago-laja-en-minimos-historicos-pone-presion-por-el-uso-de-sus-aguas/

34. **Terram -- Lago Laja corre riesgo de secarse**:
    https://www.terram.cl/lago_laja_corre_riesgo_de_secarse_si_justicia_no_ordena_a_endesa_detener_extraccion_de_aguas/

35. **Chile Desarrollo Sustentable -- Coordinador Electrico pide flexibilizar reserva**:
    https://www.chiledesarrollosustentable.cl/noticias/noticia-pais/coordinador-electrico-pide-flexibilizar-reserva-hidrica-de-embalses-tras-lluvias-y-pronosticos-de-deshielos/

36. **Canalistas del Laja -- especuladores que se enriquecen con el agua** (Jan 2020):
    https://www.diarioconcepcion.cl/economia/2020/01/21/canalistas-del-laja-lamentan-que-existan-especuladores-que-se-enriquecen-con-el-agua.html

37. **Plataforma Urbana -- Laguna del Maule y Lago Laja nivel critico** (Feb 2012):
    https://www.plataformaurbana.cl/archive/2012/02/06/laguna-del-maule-y-lago-laja-llegan-a-nivel-critico-y-agua-se-disputa-entre-riego-y-energia/

38. **La Segunda -- Saltos del Laja bajan de 20 a 6 m3/s** (Apr 2014):
    http://www.lasegunda.com/Noticias/Nacional/2014/04/926095/Dramatica-falta-de-agua-en-Salto-y-Laguna-del-Laja

### Related PDFs in This Directory

- `Actualizacion_Convenio_Laja_PLP.pdf` -- Update of the Laja convention
  for PLP modeling
- `Acuerdo_Lago_Laja_2017.pdf` -- The 2017 permanent agreement
- `CNR_Centrales_Hidroelectricas_Obras_Riego.pdf` -- CNR document on
  hydroelectric plants and irrigation works

---

*Research compiled on 2026-04-01 from web search results. Updated with
additional sources and current status information.*
