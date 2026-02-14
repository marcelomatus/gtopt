# gtopt GUI Service

A web-based graphical user interface for creating, editing, and visualizing
**gtopt** optimization cases, with integrated connectivity to the
[gtopt webservice](../webservice/README.md) for remote solving.

## Features

- **Case Creation** – Build cases from scratch with a modular form interface for
  all system elements (buses, generators, demands, lines, batteries, converters,
  junctions, waterways, reservoirs, turbines, flows, filtrations, profiles,
  reserve zones, and reserve provisions).
- **Simulation Setup** – Define blocks, stages, and scenarios through an
  interactive table editor.
- **JSON Preview** – Inspect the generated JSON configuration before downloading.
- **ZIP Download** – Download the complete case as a ZIP file ready for use with
  the gtopt solver or the gtopt webservice.
- **Case Upload** – Upload an existing case ZIP file for further editing.
- **Webservice Integration** – Submit cases directly to the gtopt webservice for
  solving, monitor job progress in real-time, and retrieve results automatically
  when the solver finishes.
- **Results Visualization** – View optimization results in spreadsheet tables
  and interactive time-series charts (Chart.js). Results can be loaded from
  uploaded ZIP files or retrieved directly from the webservice.

## Requirements

- Python 3.10+
- Dependencies listed in `requirements.txt`
- (Optional) A running [gtopt webservice](../webservice/README.md) instance for
  remote solving

For detailed installation, deployment, and production setup instructions, see
[INSTALL_GUISERVICE.md](../INSTALL_GUISERVICE.md).

## Quick Start

```bash
cd guiservice
pip install -r requirements.txt
python app.py
```

The GUI will be available at `http://localhost:5001`.

### Connecting to the Webservice

1. Start the gtopt webservice (default: `http://localhost:3000`)
2. Open the GUI and navigate to the **Solver → Webservice** panel
3. Enter the webservice URL and click **Test** to verify the connection
4. Use the **⚡ Solve** button in the header to submit cases

The webservice URL can also be set via environment variable:

```bash
GTOPT_WEBSERVICE_URL=http://my-server:3000 python app.py
```

## API Endpoints

| Method | Path                         | Description |
|--------|------------------------------|-------------|
| GET    | `/`                          | Main GUI page |
| GET    | `/api/schemas`               | Element field schemas |
| POST   | `/api/case/download`         | Generate and download case ZIP |
| POST   | `/api/case/upload`           | Upload a case ZIP for editing |
| POST   | `/api/case/preview`          | Preview the generated JSON |
| POST   | `/api/results/upload`        | Upload results ZIP for viewing |
| GET    | `/api/solve/config`          | Get webservice URL configuration |
| POST   | `/api/solve/config`          | Set webservice URL |
| POST   | `/api/solve/submit`          | Submit case to webservice for solving |
| GET    | `/api/solve/status/<token>`  | Poll job status from webservice |
| GET    | `/api/solve/results/<token>` | Retrieve and parse results from webservice |
| GET    | `/api/solve/jobs`            | List all jobs from webservice |

## Input Data Documentation

See [INPUT_DATA.md](INPUT_DATA.md) for a comprehensive description of the gtopt
input data structure, including all element types and their fields.
