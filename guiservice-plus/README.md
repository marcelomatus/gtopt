# gtopt GUI Plus

A modern Next.js 15 + React 19 + Tailwind v4 frontend for the gtopt planning
workbench.  Sits on top of the existing Flask `guiservice` — all API calls
are proxied via the Next.js rewrite `/flask/*` → `$GTOPT_GUISERVICE_URL/*`.

## Stack

| Concern | Technology |
|---|---|
| Framework | **Next.js 15 (App Router) + React 19 + TypeScript** |
| UI library | **shadcn-style** components on Radix primitives |
| Styling | **Tailwind CSS v3/v4 compatible** with HSL CSS custom properties |
| Charts | **Recharts** |
| Network graph | **ReactFlow v11** |
| State | **Zustand** (persisted to `localStorage`) |
| Data fetching | **TanStack Query v5** |
| Forms | **React Hook Form + Zod** |
| Command palette | **cmdk** (⌘K) |
| Icons | **Lucide React** |

## Development

```bash
cd guiservice-plus
npm install
GTOPT_GUISERVICE_URL=http://localhost:5000 npm run dev -- -p 5002
```

Then open <http://localhost:5002>.

Alternatively, launch both the Flask `guiservice` and GUI Plus together via:

```bash
gtopt_gui --ui-plus --ui-plus-port 5002
```

## Build & start (production)

```bash
npm run build
GTOPT_GUISERVICE_URL=http://localhost:5000 npm run start -- -p 5002
```

Or use the helper script:

```bash
./guiservice_plus.sh start
```

## Architecture

```
guiservice-plus/
├── package.json            Next.js + deps declaration
├── next.config.mjs         /flask/* → GTOPT_GUISERVICE_URL rewrite
├── tailwind.config.ts      Design tokens (colours, typography)
├── tsconfig.json           @/ → src/ path mapping
├── src/
│   ├── app/
│   │   ├── layout.tsx      AppShell, theme, query client
│   │   ├── page.tsx        Dashboard (KPIs, recent jobs, templates)
│   │   ├── case/page.tsx   Case editor (tabs + validation panel)
│   │   ├── topology/       ReactFlow topology viewer
│   │   ├── jobs/           Jobs dashboard (polling)
│   │   └── results/        Results dashboards (dispatch, SoC, expansion, SDDP, merit order)
│   ├── components/
│   │   ├── ui/             shadcn-style primitives (Button, Card, Table, Tabs, …)
│   │   ├── layout/         AppShell, Sidebar, TopBar, CommandPalette, providers
│   │   ├── case/           OptionsForm, SimulationEditor, ElementEditor, CaseHealthPanel, JsonPreviewDialog
│   │   └── results/        DispatchStack, StorageSoc, CapacityExpansion, SddpConvergence, MeritOrderTable, KpiStrip
│   ├── lib/
│   │   ├── api.ts          Typed fetch wrappers for the Flask API
│   │   ├── store.ts        Zustand store (caseData, selections, theme, undo/redo)
│   │   ├── schemas.ts      Zod schemas mirroring ELEMENT_SCHEMAS
│   │   ├── tech-colors.ts  Technology → colour mapping
│   │   └── results-tables.ts Helpers for extracting tabular data
│   └── styles/globals.css  Tailwind + CSS custom properties (light/dark)
```

## Backend endpoints consumed

The UI talks to these Flask endpoints (proxied via `/flask/*`):

| Endpoint | Purpose |
|---|---|
| `GET /api/templates`, `/api/templates/<slug>` | Bundled templates |
| `POST /api/case/validate`, `/api/case/check_refs` | Case validation |
| `POST /api/case/preview` | Planning preview (existing endpoint) |
| `GET /api/solve/jobs`, `/api/solve/status/<token>`, `/api/solve/monitor/<token>` | Job management |
| `POST /api/results/upload`, `/api/results/summary`, `/api/results/aggregate` | Results I/O |
| `POST /api/diagram/topology` with `format=reactflow` | Topology ReactFlow JSON |

## Status

This is the initial Phase 1–5 scaffold.  Further polish (Phase 7):
undo/redo keyboard shortcuts, per-field tooltips, emissions estimates,
Pareto scans, and the guided "new case" wizard — tracked in the main
`IMPLEMENTATION_PLAN.md`.
