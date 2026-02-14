# gtopt GUI Service

A web-based graphical user interface for creating, editing, and visualizing
**gtopt** optimization cases.

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
- **Results Visualization** – Upload results ZIP files to view output data in
  spreadsheet tables and interactive time-series charts.

## Requirements

- Python 3.10+
- Dependencies listed in `requirements.txt`

## Quick Start

```bash
cd guiservice
pip install -r requirements.txt
python app.py
```

The GUI will be available at `http://localhost:5001`.

## API Endpoints

| Method | Path                   | Description |
|--------|------------------------|-------------|
| GET    | `/`                    | Main GUI page |
| GET    | `/api/schemas`         | Element field schemas |
| POST   | `/api/case/download`   | Generate and download case ZIP |
| POST   | `/api/case/upload`     | Upload a case ZIP for editing |
| POST   | `/api/case/preview`    | Preview the generated JSON |
| POST   | `/api/results/upload`  | Upload results ZIP for viewing |

## Input Data Documentation

See [INPUT_DATA.md](INPUT_DATA.md) for a comprehensive description of the gtopt
input data structure, including all element types and their fields.
