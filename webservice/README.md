# gtopt Web Service

A web service for uploading, running, and downloading results from gtopt
optimization cases. Built with [Next.js](https://nextjs.org/) (React +
Node.js full-stack framework).

## Features

- **Upload** a case directory as a `.zip` archive containing the system JSON
  file and data files (e.g., Parquet files).
- **Submit** the job and receive a unique **token** for tracking.
- **Check status** of a running job using the token.
- **Download results** as a compressed `.zip` file once the job completes.
- **Recent jobs list** with auto-refresh on the web UI.

## Quick Start

### Prerequisites

- Node.js 18+ and npm
- A built `gtopt` binary (see the main project README)

### Development

```bash
cd webservice
npm install
npm run dev
```

The service starts at `http://localhost:3000`.

Set the `GTOPT_BIN` environment variable to point to your gtopt binary:

```bash
GTOPT_BIN=/path/to/gtopt npm run dev
```

### Production Build

```bash
npm run build
GTOPT_BIN=/path/to/gtopt npm run start
```

### Using CMake

From the repository root:

```bash
# Configure (from standalone build directory)
cmake -S standalone -B build

# Install web service dependencies and build
cmake --build build --target webservice-install
cmake --build build --target webservice-build

# Start in production mode
cmake --build build --target webservice-start
```

## API Reference

### POST /api/jobs

Upload a case archive and submit a new optimization job.

**Request** (multipart/form-data):
- `file` — `.zip` archive containing the case directory
- `systemFile` — name of the system JSON file inside the archive (e.g.,
  `system_c0.json`)

**Response** (201 Created):
```json
{
  "token": "550e8400-e29b-41d4-a716-446655440000",
  "status": "pending",
  "message": "Job submitted successfully..."
}
```

### GET /api/jobs

List all submitted jobs.

**Response** (200):
```json
{
  "jobs": [
    {
      "token": "...",
      "status": "completed",
      "createdAt": "2025-01-01T00:00:00.000Z",
      "completedAt": "2025-01-01T00:01:00.000Z",
      "systemFile": "system_c0.json"
    }
  ]
}
```

### GET /api/jobs/:token

Get the status of a specific job.

**Response** (200):
```json
{
  "token": "550e8400-e29b-41d4-a716-446655440000",
  "status": "completed",
  "createdAt": "2025-01-01T00:00:00.000Z",
  "completedAt": "2025-01-01T00:01:00.000Z",
  "systemFile": "system_c0.json"
}
```

### GET /api/jobs/:token/download

Download job results as a zip file.

**Response** (200): Binary `.zip` file containing:
- `output/` — solver output files
- `stdout.log` — solver standard output
- `stderr.log` — solver standard error
- `job.json` — job metadata

## Configuration

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `GTOPT_BIN` | `../build/gtopt` | Path to the gtopt binary |
| `GTOPT_DATA_DIR` | `./data` | Directory for job data storage |
| `PORT` | `3000` | HTTP port |

## Testing

An integration test script validates the full workflow using a mock gtopt
binary. It starts the server, submits a case, waits for completion, downloads
and verifies results, and tests error handling.

```bash
# Requires a prior build: npm run build
npm test
# or directly:
bash test/integration_test.sh [port]
```

## Project Structure

```
webservice/
├── CMakeLists.txt          # CMake build/install configuration
├── package.json            # Node.js dependencies
├── next.config.ts          # Next.js configuration
├── tsconfig.json           # TypeScript configuration
├── src/
│   ├── app/
│   │   ├── layout.tsx      # Root layout
│   │   ├── globals.css     # Global styles
│   │   ├── page.tsx        # Main UI page
│   │   └── api/
│   │       └── jobs/
│   │           ├── route.ts           # POST/GET /api/jobs
│   │           └── [token]/
│   │               ├── route.ts       # GET /api/jobs/:token
│   │               └── download/
│   │                   └── route.ts   # GET /api/jobs/:token/download
│   └── lib/
│       └── jobs.ts         # Job management logic
├── test/
│   └── integration_test.sh # End-to-end integration test
└── README.md
```
