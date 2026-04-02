# Convenios de Riego: Laja and Maule Systems
# Academic Papers, Presentations, and Educational Materials

Research compiled: 2026-04-01

---

## Table of Contents

1. [Overview](#overview)
2. [Academic Theses (Universidad de Chile)](#academic-theses)
3. [Coordinador Electrico Nacional Documents](#coordinador-documents)
4. [University Course Materials](#university-course-materials)
5. [PLP Model and Irrigation Agreement Modeling](#plp-model)
6. [Legal and Congressional Documents](#legal-documents)
7. [Original Agreements](#original-agreements)
8. [News and Industry Sources](#news-sources)
9. [Engineer Munoz Search Results](#engineer-munoz)
10. [Downloaded PDFs Inventory](#downloaded-pdfs)
11. [Key Technical Details](#key-technical-details)

---

## 1. Overview <a name="overview"></a>

Chile's two most important hydroelectric reservoirs -- Lago Laja and Laguna del
Maule -- are both subject to "convenios de riego" (irrigation agreements) that
regulate the allocation of water between hydroelectric generation and
agricultural irrigation. These agreements date from the mid-20th century (Laja:
1958, Maule: 1947) and have been central to power system operations, water
management disputes, and optimization modeling in Chile.

### Key Numbers

| System | Agreement Year | Storage Capacity | Installed Capacity | Irrigated Area |
|--------|---------------|-----------------|-------------------|----------------|
| Lago Laja | 1958 | 6,000 Mm3 | >1,000 MW (El Toro, Abanico, Antuco, Rucue, Quilleco) | 117,000 ha |
| Laguna del Maule | 1947 | ~670 Mm3 | Pehuenche/Colbun system | Major Maule valley agriculture |

---

## 2. Academic Theses (Universidad de Chile) <a name="academic-theses"></a>

### 2.1. Laja System -- Effect on Long-Term Programming (PLP)

**Title**: "Efecto del convenio de riego del sistema hidroelectrico Laja sobre
la programacion de largo plazo del sistema interconectado central de Chile"

- **URL**: <http://repositorio.uchile.cl/handle/2250/139279>
- **Type**: Civil Engineering thesis (Hidraulica Sanitaria Ambiental)
- **Repository**: Universidad de Chile

**Key Findings**:
- Analyzes technical-economic impacts of modifications to the Laja irrigation
  agreement on both basin-level irrigators and the SIC (Central Interconnected
  System)
- Defined new operation alternatives for Lago Laja and implemented them within
  the Long-Term Programming Model (PLP) of the SIC
- Found that adding a constraint affecting only one reservoir does not
  considerably alter the system's functioning at a systemic level, but can
  alter its local environment at the basin level
- The sustained increase in electrical demand in the SIC, combined with water
  scarcity due to drought, motivated the study

**Modeling Approach**:
- Implemented operational modifications within PLP
- Used SDDP (Stochastic Dual Dynamic Programming) algorithm
- Irrigation agreements modeled by subtracting from water balances of
  hydroelectric plants
- Slack variables allow flexibility, discriminating priority irrigation/production

### 2.2. Maule System -- Operational Interference Modeling

**Title**: "Modelacion para el analisis de la interferencia operacional entre
hidroelectricidad y riego en la cuenca del Maule, Chile"

- **URL**: <https://repositorio.uchile.cl/handle/2250/196765>
- **Type**: Master's thesis
- **Repository**: Universidad de Chile

**Key Findings**:
- Laguna del Maule is a multipurpose reservoir subject to the 1947
  ENDESA-Irrigation agreement
- Under future climate scenarios (RCP8.5, 2022-2046), the reservoir would not
  supply total projected demand
- Found periods of failure in both irrigation and hydroelectric supply
- Recommended future analysis to explore modifications to the agreement to
  reduce operational interference

### 2.3. Tradeoffs Between Hydroelectricity and Irrigation

**Title**: "Tradeoffs entre hidroelectricidad y riego en un sistema electrico
hidrotermico multi-cuenca"

- **URL**: <https://repositorio.uchile.cl/handle/2250/165722>
- **Full text PDF**: <https://repositorio.uchile.cl/bitstream/handle/2250/165722/Tradeoffs-entre-hidroelectricidad-y-riego-en-un-sistema-el%C3%A9ctrico-hidrot%C3%A9rmico-multi-cuenca.pdf>
- **Type**: Engineering thesis
- **Repository**: Universidad de Chile
- **Downloaded**: Yes (Tesis_Tradeoffs_Hidroelectricidad_Riego.pdf)

**Key Findings**:
- In Chile, hydrothermal coordination interferes with water use for irrigation
  because there is a mismatch in demands (temporal conflict)
- As the weighting of irrigation costs increases in the optimization model,
  irrigation security increases proportionally
- Depending on the weighting, gains/losses of both sectors can reach the same
  magnitude in dollars, up to 90 MUSD
- The irrigation sector can be highly benefited by being included in the
  hydrothermal coordination formulation
- Demonstrates a MILP formulation applied to a real representation of the
  Chilean power system to model nation-wide irrigation agreements

### 2.4. Tinguiririca River -- Hydroelectric Impact on Irrigation

**Title**: "Estudio del impacto de la operacion hidroelectrica en el uso del
agua para riego en la cuenca del rio Tinguiririca, Chile"

- **URL**: <https://repositorio.uchile.cl/handle/2250/184623>
- **Type**: Engineering thesis
- **Repository**: Universidad de Chile

**Key Findings**:
- Studies impact of run-of-river hydroelectric operations (La Confluencia and
  La Higuera) on downstream irrigation users
- Found significant increase in hydrological alteration since the plants began
  operations, particularly outside the irrigation season
- Historical operational data shows plants prioritized electrical generation
  Monday-Friday vs weekends
- Explores operation schemes that reconcile both water uses

---

## 3. Coordinador Electrico Nacional Documents <a name="coordinador-documents"></a>

### 3.1. Convenios de Riego Section

The Coordinador Electrico Nacional maintains a dedicated section for irrigation
agreements within its SEN (National Electric System) modeling documentation:

- **Main page**: <https://www.coordinador.cl/operacion/documentos/modelacion-del-sen/convenios-de-riego/>
- **Maule agreements**: <https://www.coordinador.cl/operacion/documentos/modelacion-del-sen/convenios-de-riego/convenios-rio-maule/>

### 3.2. Actualizacion Convenio del Lago Laja e Implementacion en PLP

- **URL**: <https://www.coordinador.cl/wp-content/uploads/2019/08/Actualizaci%C3%B3n-Convenio-Laja-en-PLP.pdf>
- **Downloaded**: Yes (Actualizacion_Convenio_Laja_PLP.pdf, 4.5 MB)

This document describes how the updated Lago Laja irrigation agreement is
implemented within the PLP model used by the Coordinador for hydrothermal
coordination. Key aspects include:

- Implementation of feasibility cuts in the SDDP program PLP
  (forward/backward)
- The Laja Lake irrigation agreement has 7 state variables
- Modeling the restrictions provided by the agreements requires adding binary
  variables, making it infeasible to solve via standard SDDP

### 3.3. Acuerdo Lago Laja 2017

- **URL**: <https://www.coordinador.cl/wp-content/uploads/2018/12/Acuerdo_Lago_Laja_2017.pdf>
- **Downloaded**: Yes (Acuerdo_Lago_Laja_2017.pdf, 12.6 MB)

The 2017 agreement is a comprehensive document that was signed following
three years of negotiation. It complements the 1958 agreement and includes:

- Recognition of users without formal Water Use Rights (DAA)
- Inclusion of ecosystem services (first case in Chile where ecosystem
  services of Rio Laja are valued -- specifically Salto del Laja waterfall)
- Adaptation measures for climate change
- Ensured irrigation for 117,000 hectares
- Created water reserve mechanisms

### 3.4. PLP Model Documentation

- **PLP information**: <https://www.coordinador.cl/operacion/documentos/modelacion-del-sen/modelos-para-la-planificacion-y-programacion-de-la-operacion/informacion-relacionada-con-el-modelo-plp/>
- **PLP documentation**: <https://www.coordinador.cl/operacion/documentos/modelacion-del-sen/modelos-para-la-planificacion-y-programacion-de-la/informacion-relacionada-con-el-modelo-plp/documentacion/>

---

## 4. University Course Materials <a name="university-course-materials"></a>

### 4.1. Prof. Hugh Rudnick -- PUC Course Materials

Professor Hugh Rudnick (Pontifical Catholic University of Chile) maintains
educational materials on his website covering irrigation agreements and their
role in power system operations:

- **Models of Operation (PLP/SDDP)**: <https://hrudnick.sitios.ing.uc.cl/alumno15/rieg/sddps.html>
- **Cuenca del Maule**: <https://hrudnick.sitios.ing.uc.cl/alumno15/rieg/cmaule.html>
- **Cuenca del Laja**: <http://hrudnick.sitios.ing.uc.cl/alumno15/rieg/claja.html>

**Key Technical Content from Course Materials**:

- Irrigation agreements are incorporated into the optimization problem by
  subtracting them from the water balances of hydroelectric plants
- A slack variable allows flexibility, discriminating priority
  irrigation/production
- Modeling the restrictions provided by the agreements requires adding binary
  variables, which makes it infeasible to solve with SDDP
- PLP was specifically developed for the SIC and includes irrigation
  agreements
- Since 2001, PLP has been used by the CNE (National Energy Commission) to set
  regulated tariff prices

### 4.2. Centro de Energia (Universidad de Chile)

- **PLP Model page**: <https://centroenergia.cl/en/seleccionados/plp/>
- **PLP projects**: <https://centroenergia.cl/en/proyectos/seleccion-de-proyectos/plp/>
- **PLP training course**: <https://centroenergia.cl/en/curso-modelo-plp/>

The Centro de Energia develops and maintains the PLP hydrothermal coordination
model, which is the official tool used by the Coordinador Electrico Nacional.
They offer training courses where students learn to use the PLP model.

### 4.3. PLP Model Technical Document (UFRO)

- **URL**: <https://bibliotecadigital.ufro.cl/v2/files/original/21f5b30217264574f736a2c93517725b71f74b6a.pdf>
- **Downloaded**: Yes (Modelo_PLP_Coordinacion_Hidrotermica.pdf, 4.6 MB)

Technical documentation of the PLP hydrothermal coordination model, describing
how irrigation agreements are handled as constraints in the optimization.

### 4.4. Diploma Program

- **Title**: "Diploma de Postitulo en Coordinacion de Sistemas Electricos
  Hidrotermicos e Integracion de ERNC"
- **URL**: <https://facso.uchile.cl/cursos/110045/diploma-sistemas-electricos-hidrotermicos-e-integracion-de-ernc>

Graduate-level diploma program covering hydrothermal system coordination,
including irrigation agreement modeling.

---

## 5. PLP Model and Irrigation Agreement Modeling <a name="plp-model"></a>

### How Irrigation Agreements Are Modeled in PLP

The PLP (Programacion de Largo Plazo) model is a computational/mathematical
tool that solves the medium-to-long-term operation planning of hydrothermal
electric systems using Stochastic Dual Dynamic Programming (SDDP).

**Irrigation agreement modeling approach**:

1. Agreements are incorporated by **subtracting irrigation demands from the
   water balances** of hydroelectric plants
2. A **slack variable** allows flexibility, discriminating between priority
   irrigation or production
3. This approach does not include irrigation models as part of the model's
   constraints proper
4. Modeling the full restrictions provided by the agreements would require
   **binary variables**, making the SDDP solution infeasible
5. **Feasibility cuts** were implemented in the PLP SDDP
   (forward/backward) to handle the Laja agreement
6. The Laja Lake irrigation agreement has **7 state variables**

**Alternative approaches studied**:
- MILP formulations for a real representation of the Chilean power system
- WEAP (Water Evaluation and Planning) models for basin-level analysis
- Combined LP/MILP with endogenous irrigation agreement constraints

### Key Challenge

The fundamental challenge is that irrigation agreements create **non-convex
constraints** (due to conditional rules based on reservoir levels, time of
year, and drought conditions). These cannot be directly handled by SDDP, which
requires convexity. The workaround involves either:
- Simplifying the constraints and using slack variables
- Adding feasibility cuts
- Switching to MILP formulations (computationally expensive)
- WEAP modeling for basin-level water balance

---

## 6. Legal and Congressional Documents <a name="legal-documents"></a>

### 6.1. Chamber of Deputies Investigative Commission (2023)

**Title**: "Comision Especial Investigadora encargada de fiscalizar los actos
del Gobierno relativos al cumplimiento del Convenio de Riego ENDESA de 1947"

- **Commission page**: <https://www.camara.cl/cms/noticias/2023/07/27/comision-investigadora-sobre-convenio-de-riego-endesa-aprobo-conclusiones/>
- **Final report**: <https://camara.cl/verDoc.aspx?prmID=277624&prmTipo=DOCUMENTO_COMISION>
- **Downloaded**: Yes (Informe_Comision_Investigadora_Maule.pdf, 259 KB, 19 pages)

**Key Findings**:
- Investigated possible non-compliance with the 1947 ENDESA irrigation
  agreement for the Laguna del Maule
- Examined who authorized ENEL to use water resources for purposes other than
  irrigation
- **Conclusion**: No non-compliance found with the 1947 agreement
- Detailed the three-portion division of Laguna del Maule:
  - First portion: ~170 Mm3 (specific allocation)
  - Second portion (intermediate/ordinary reserve): ~500 Mm3 (80% irrigation,
    20% ENDESA generation)
  - Third portion: generation use subject to irrigation priority

### 6.2. Sistema Maule -- Operacion de la Cuenca

- **URL**: <https://www.camara.cl/verDoc.aspx?prmID=163367&prmTIPO=DOCUMENTOCOMISION>

Presentation to the Chilean Congress about the Maule basin operations:
- Maule system storage capacity: 3,300 Mm3 across four reservoirs
- Only one (Laguna del Maule) designed specifically for irrigation
- Functions as a multipurpose reservoir under the 1947 agreement

### 6.3. Supreme Court Ruling (2019/2021)

- **URL**: <https://www.revistaei.cl/2021/03/17/corte-suprema-mantiene-fallo-que-acogio-demanda-por-uso-de-aguas-de-la-laguna-del-maule/>

The Supreme Court recognized irrigation priority over electricity generation
in water uses of Laguna del Maule, according to rules established by the 1947
agreement.

---

## 7. Original Agreements <a name="original-agreements"></a>

### 7.1. Convenio de Riego Laja (1958)

- Signed between: Direccion de Riego (now Direccion de Obras Hidraulicas, DOH)
  and Empresa Nacional de Electricidad (ENDESA)
- Purpose: Ensure water supply for irrigation through proper management of
  Lago Laja extractions
- Parties: State (irrigation) and ENDESA (then state-owned)
- Key flexibilizations: 1998 (after severe drought reaching lowest level in
  50 years)
- Updated: 2015 (complementary agreement), 2017 (major update), 2022
  (indefinite agreement)

### 7.2. Convenio ENDESA-Riego Maule (1947)

- **URL**: <http://www.canalmaule.cl/wp-content/uploads/2016/02/convenio_endesa_riego.pdf>
- **Downloaded**: Yes (Convenio_Endesa_Riego_Maule.pdf, 75 KB)
- Signed between: Director of Irrigation Department and General Manager of
  ENDESA
- Key provisions:
  - Three-portion division of Laguna del Maule
  - Ordinary reserve of 500 Mm3 (80% irrigation, 20% generation)
  - Electric generation extractions cannot exceed monthly average of 25 m3/s
  - ENDESA could use waters "without altering the proposed development for
    irrigation"
- Guaranteed ENDESA the future construction of Los Condores hydroelectric plant

### 7.3. CNR Document on Hydroelectric Plants and Irrigation Works

- **URL**: <https://www.cnr.gob.cl/wp-content/uploads/2021/11/RGO5124-Centrales-Hidroele%CC%81ctricas-asociadas-a-obras-de-riego..pdf>
- **Downloaded**: Yes (CNR_Centrales_Hidroelectricas_Obras_Riego.pdf, 7.6 MB)

National Irrigation Commission document on hydroelectric plants associated
with irrigation works.

---

## 8. News and Industry Sources <a name="news-sources"></a>

### 8.1. Mundoagro -- Lago Laja Operation Agreement

- **URL**: <https://mundoagro.cl/el-acuerdo-de-operacion-del-lago-laja-ejemplo-de-coordinacion-entre-agricultura-y-energia/>

Describes the 2017 Lago Laja agreement as a model for coordination between
agriculture and energy. Key aspects:
- The agreement was developed with advanced aspects including recognition of
  users without Water Use Rights (DAA)
- First case in Chile where ecosystem services are valued (Salto del Laja)
- Adaptation measures for climate change
- Farmers ceded part of water rights to ensure Salto del Laja flow
- Agreement survived five more years of drought, including the 2019 crisis

### 8.2. Enel Press Release -- Laja Agreement

- **URL**: <https://www.enel.cl/es/conoce-enel/prensa/press-enel-generacion/d201711-ministerio-de-obras-publicas-y-enel-generacion-chile-firman-convenio-permanente-para-operacion-y-recuperacion-del-embalse-laja.html>

MOP and Enel Generacion Chile signed a permanent agreement for the operation
and recovery of the Laja reservoir (November 2017).

### 8.3. Diario Financiero -- End of Laja Disputes

- **URL**: <https://www.df.cl/empresas/energia/enel-pone-fin-a-mas-de-10-anos-de-disputas-con-regantes-del-laja-por-uso>

Enel ended more than 10 years of disputes with Laja irrigators, signing an
indefinite agreement complementing the 1958 accord.

### 8.4. Revista Electricidad -- Laja Dispute

- **URL**: <https://www.revistaei.cl/2013/10/21/la-disputa-que-enfrenta-a-canalistas-y-endesa-por-uso-de-laguna-del-laja/>

### 8.5. Linares en Linea -- Maule Water Risk (2026)

- **URL**: <https://www.linaresenlinea.cl/2026/03/28/advierten-riesgo-hidrico-en-el-maule-y-llaman-a-acuerdos-para-proteger-el-embalse-laguna-del-maule/>

Recent (March 2026) article warning about water risk in Maule and calling for
agreements to protect the Laguna del Maule reservoir.

### 8.6. Asociacion de Canalistas del Laja

- **URL**: <http://www.canalistasdellaja.cl/?p=274>

Irrigators' association page documenting the 2015 signing of the convention for
better use of Lago Laja waters.

### 8.7. Junta de Vigilancia Rio Maule

- **URL**: <https://jvriomaule.cl/>
- Priority ruling: <http://jvriomaule.cl/laguna-del-maule-prioritariamente-riego/>

The JVRM has emphasized that recovery of Laguna del Maule, together with
operational agreements with Colbun-Pehuenche and ENEL-JVRM, has been critical
for sustaining irrigation supply.

---

## 9. Engineer Munoz Search Results <a name="engineer-munoz"></a>

Extensive web searches were conducted using multiple first name variants (Jose,
Juan, Carlos, Pedro, Ricardo, Fernando, Rodrigo) combined with Endesa, Enel,
Laja, Maule, and related terms. **No specific engineer named Munoz was found
in publicly available materials** related to the convenios de riego for Laja
or Maule systems.

The following names appeared in search results as key individuals in the Laja
and Maule water management:
- **Valter Moro** (Enel executive)
- **Ramon Alfonsin** (Endesa Chile representative)
- **Claudio Betti Pruzzo** (Endesa, project programming)
- **Rafael Errazuriz Ruiz-Tagle** (Endesa, Energy Planning executive)
- **Cristian Marcelo Munoz** -- Found as Director of BdE and professor of
  Energy Economics and Environment at the Department of Electrical Engineering,
  PUC (Pontifical Catholic University of Chile). This could potentially be the
  engineer referenced, but no specific presentations about convenios de riego
  were found associated with him.

### Suggestion

If the referenced engineer Munoz gave internal presentations at Endesa/Enel,
these may not be publicly available. Consider checking:
- Internal Endesa/Enel technical archives
- Chilean IEEE PES chapter proceedings
- CIGRE Chile proceedings
- CDEC-SIC (now Coordinador) technical meeting minutes
- Universidad de Chile or PUC thesis advisor records

---

## 10. Downloaded PDFs Inventory <a name="downloaded-pdfs"></a>

All files stored in:
`/home/marce/git/gtopt/docs/analysis/irrigation_agreements/`

| File | Size | Source | Description |
|------|------|--------|-------------|
| `Actualizacion_Convenio_Laja_PLP.pdf` | 4.5 MB | coordinador.cl | Implementation of updated Laja agreement in PLP model |
| `Acuerdo_Lago_Laja_2017.pdf` | 12.6 MB | coordinador.cl | Complete 2017 Lago Laja operation agreement |
| `CNR_Centrales_Hidroelectricas_Obras_Riego.pdf` | 7.6 MB | cnr.gob.cl | Hydroelectric plants associated with irrigation works |
| `Convenio_Endesa_Riego_Maule.pdf` | 75 KB | canalmaule.cl | Original Endesa-Irrigation agreement for Maule |
| `Informe_Comision_Investigadora_Maule.pdf` | 259 KB | camara.cl | Congressional investigative commission final report |
| `Modelo_PLP_Coordinacion_Hidrotermica.pdf` | 4.6 MB | ufro.cl | PLP hydrothermal coordination model documentation |
| `Tesis_Tradeoffs_Hidroelectricidad_Riego.pdf` | 1.9 MB | uchile.cl | Multi-basin hydroelectricity-irrigation tradeoffs thesis |

---

## 11. Key Technical Details <a name="key-technical-details"></a>

### Laja System Components

| Plant | Type | Capacity |
|-------|------|----------|
| El Toro | Reservoir (Lago Laja) | 450 MW |
| Abanico | Run-of-river + regulation | 136 MW |
| Antuco | Run-of-river + regulation | 320 MW |
| Rucue | Run-of-river | 178 MW |
| Quilleco | Run-of-river | 70 MW |

- Lago Laja: >6,000 Mm3 capacity, only multi-year regulation reservoir in Chile
- The 1958 agreement includes water table management rules for extraction
  limits based on lake levels

### Maule System Components

- Laguna del Maule: ~670 Mm3 (three-portion division per 1947 agreement)
- Colbun reservoir: used for storing irrigation water in spring
- Melado reservoir: part of the Pehuenche system
- La Invernada reservoir: regulation
- Key plants: Pehuenche (Colbun subsidiary), Colbun

### Modeling the Agreements in Optimization

**In the PLP/SDDP framework**:
```
Water balance:  V(t+1) = V(t) + Inflow(t) - Turbined(t) - Spill(t) - Irrigation(t)
```

Where `Irrigation(t)` is the irrigation demand subtracted from the balance.
The agreement constraints include:

- Minimum/maximum reservoir levels by month
- Conditional extraction rules based on lake level
- Priority rules (irrigation > generation in specific portions)
- Seasonal flow requirements (irrigation season vs generation season)

**Challenge for SDDP**: The conditional rules create **non-convex constraints**
that require binary variables (MILP), which are incompatible with standard SDDP.
Solutions include:
- Slack variable approximation
- Feasibility cuts
- Endogenous MILP formulation (computationally expensive)
- WEAP modeling for basin-level water balance

### Relevance to gtopt

The irrigation agreements affect the optimization formulation by adding:
- Conditional reservoir operating constraints
- Time-varying minimum/maximum flow requirements
- Multi-use water allocation constraints
- Seasonal demand profiles for irrigation
- Priority rules between competing water uses

These are relevant for any GTEP model that includes hydro reservoirs in basins
with irrigation agreements.

---

## Additional Resources Not Yet Downloaded

The following resources were identified but not downloaded (require direct
website access or are behind authentication):

1. **Hrudnick course pages** (UC) -- HTML content on PLP/SDDP and irrigation
   agreements for Laja and Maule basins
2. **Coordinador.cl Maule agreements page** -- may contain additional
   downloadable PDFs
3. **Sistema Maule congressional presentation** -- requires camara.cl document
   viewer
4. **Carl Bauer's book chapter** on dams and markets in Chilean rivers
   (academic publication)
5. **Fundacion Terram reports** on Lago Laja water conflicts
