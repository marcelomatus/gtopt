# gtopt_websrv - Web Service Launcher

A command-line launcher for the gtopt web service that provides a REST API for
submitting optimization cases and downloading results.

## Installation

### Option 1: Install webservice only (Recommended)

Install just the web service without the gtopt binary:

```bash
# Build and install webservice
cd gtopt
cmake -S webservice -B build-web
sudo cmake --install build-web

# Install Node.js dependencies (including build dependencies)
cd /usr/local/share/gtopt/webservice
npm install
npm run build
```

**Note**: We use `npm install` (not `--production`) because TypeScript and other
devDependencies are required for the build step (`npm run build`).

### Option 2: Install with standalone gtopt binary

If you've already built the standalone gtopt binary, you can install it first,
then install webservice separately:

```bash
# Build and install gtopt standalone binary
CC=gcc-14 CXX=g++-14 cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build

# Then install webservice
cmake -S webservice -B build-web
sudo cmake --install build-web

# Install Node.js dependencies (including build dependencies)
cd /usr/local/share/gtopt/webservice
npm install
npm run build
```

After installation:
- The `gtopt_websrv` launcher is installed to `/usr/local/bin/gtopt_websrv`
- The web service files are installed to `/usr/local/share/gtopt/webservice`
- The webservice installation is completely independent of the gtopt binary
- A systemd service file template is provided for production deployments

## Requirements

- Node.js 18 or later
- npm 9 or later
- gtopt binary (required to actually run cases)

## Usage

### Basic usage

Launch the web service on default port 3000:

```bash
gtopt_websrv
```

The service will be available at `http://localhost:3000`.

### With options

```bash
# Use custom port
gtopt_websrv --port 8080

# Specify gtopt binary location
gtopt_websrv --gtopt-bin /usr/local/bin/gtopt

# Set data directory
gtopt_websrv --data-dir /var/lib/gtopt/jobs

# Run in development mode (with hot reload)
gtopt_websrv --dev
```

### Environment Variables

You can also use environment variables:

```bash
# Set port
PORT=8080 gtopt_websrv

# Set gtopt binary
GTOPT_BIN=/usr/local/bin/gtopt gtopt_websrv

# Set data directory
GTOPT_DATA_DIR=/var/lib/gtopt/jobs gtopt_websrv

# Set log directory
GTOPT_LOG_DIR=/var/log/gtopt gtopt_websrv
```

## Options

```
gtopt_websrv [options]

Options:
  --port PORT          Port for the web service (default: 3000)
  --gtopt-bin PATH     Path to gtopt binary (default: auto-detect)
  --data-dir PATH      Directory for job data storage (default: ./data)
  --log-dir PATH       Directory for log files (default: console only)
  --dev                Run in development mode with hot reload
  --help               Show help message
```

## Systemd Service

A systemd service file template is included for running the web service as a
system daemon.

### Installation

```bash
# Copy the service file to systemd
sudo cp /usr/local/share/gtopt/webservice/gtopt-webservice.service \
        /etc/systemd/system/

# Edit the service file if needed (e.g., change port, user, paths)
sudo nano /etc/systemd/system/gtopt-webservice.service

# Reload systemd
sudo systemctl daemon-reload

# Enable the service to start on boot
sudo systemctl enable gtopt-webservice

# Start the service
sudo systemctl start gtopt-webservice

# Check status
sudo systemctl status gtopt-webservice
```

### Service Configuration

The default service file uses these settings:
- Port: 3000
- User: gtopt (create this user first)
- Working directory: /usr/local/share/gtopt/webservice
- GTOPT_BIN: /usr/local/bin/gtopt

Edit `/etc/systemd/system/gtopt-webservice.service` to customize these settings.

## Features

The web service provides:
- **Case Upload**: Upload gtopt cases as ZIP files
- **Job Submission**: Submit cases for solving with unique job tokens
- **Status Polling**: Check the status of running jobs
- **Result Download**: Download solver results as ZIP files
- **Job Management**: List all submitted jobs
- **Web UI**: Simple web interface for manual case submission

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/jobs` | Submit a new optimization job |
| `GET` | `/api/jobs` | List all submitted jobs |
| `GET` | `/api/jobs/:token` | Get status of a specific job |
| `GET` | `/api/jobs/:token/download` | Download job results |

See [webservice/README.md](README.md) for detailed API documentation.

## Examples

### Basic workflow

```bash
# 1. Start the web service
gtopt_websrv

# 2. Submit a case (in another terminal)
curl -X POST http://localhost:3000/api/jobs \
  -F "file=@mycase.zip" \
  -F "systemFile=system.json"
# Returns: {"token":"abc123...","status":"pending"}

# 3. Check status
curl http://localhost:3000/api/jobs/abc123...
# Returns: {"status":"running"} or {"status":"completed"}

# 4. Download results
curl http://localhost:3000/api/jobs/abc123.../download \
  -o results.zip
```

### Production deployment

```bash
# Install as system service
sudo cp /usr/local/share/gtopt/webservice/gtopt-webservice.service \
        /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable gtopt-webservice
sudo systemctl start gtopt-webservice

# Monitor logs
sudo journalctl -u gtopt-webservice -f

# Stop service
sudo systemctl stop gtopt-webservice
```

## Troubleshooting

### "Error: gtopt binary not found"

The launcher tries to find the gtopt binary automatically. If it fails:

```bash
# Specify the binary location explicitly
gtopt_websrv --gtopt-bin /path/to/gtopt

# Or set environment variable
GTOPT_BIN=/path/to/gtopt gtopt_websrv
```

### "Error: node_modules not found"

Install Node.js dependencies first:

```bash
cd /usr/local/share/gtopt/webservice
npm install
```

**Note**: Use `npm install` (not `--production`) to include TypeScript and other
devDependencies required for building.

### "Error: Production build not found"

Build the web service first:

```bash
cd /usr/local/share/gtopt/webservice
npm run build
```

Or run in development mode:

```bash
gtopt_websrv --dev
```

### Port already in use

Use a different port:

```bash
gtopt_websrv --port 8080
```

### WSL: API verification fails even when Next.js is "Ready"

If running on **WSL2 Ubuntu**, `localhost` networking can be affected by
Windows-side port forwarding, existing host services, or firewall policy.

Recommended checks:

```bash
# From WSL, verify API endpoints directly
curl -i http://127.0.0.1:3000/api
curl -i http://127.0.0.1:3000/api/ping

# If needed, avoid port collisions
gtopt_websrv --port 3001
```

If you access the service from Windows browser/tools, also verify:
- `localhostForwarding=true` in `%UserProfile%\.wslconfig`
- Windows Defender Firewall is not blocking WSL forwarded ports
- No Windows process is already using the same port (3000 is common for dev servers)

## Platform Support

- ✅ Ubuntu/Debian
- ✅ macOS
- ✅ WSL
- ✅ Windows (via gtopt_websrv.bat)

## See Also

- [README.md](README.md) - Full web service documentation
- [INSTALL.md](INSTALL.md) - Detailed installation guide
- [../README.md](../README.md) - gtopt main documentation
