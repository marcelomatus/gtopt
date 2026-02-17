# Installing and Running the gtopt Web Service

This guide covers setting up a server, installing the gtopt solver, deploying
the web service, and running it in production.

## Table of Contents

- [Prerequisites](#prerequisites)
- [1. Server Setup](#1-server-setup)
- [2. Install gtopt Dependencies](#2-install-gtopt-dependencies)
- [3. Build and Install gtopt](#3-build-and-install-gtopt)
- [4. Install the Web Service](#4-install-the-web-service)
- [5. Configure the Web Service](#5-configure-the-web-service)
- [6. Run the Web Service](#6-run-the-web-service)
- [7. Run as a System Service](#7-run-as-a-system-service)
- [8. Reverse Proxy (nginx)](#8-reverse-proxy-nginx)
- [9. Testing](#9-testing)
- [10. API Reference](#10-api-reference)
- [11. Troubleshooting](#11-troubleshooting)

---

## Prerequisites

| Component | Minimum Version | Purpose |
|-----------|----------------|---------|
| Ubuntu | 22.04 LTS | Server OS (other Linux distros work with adjustments) |
| GCC | 14 | C++26 compiler for building gtopt |
| CMake | 3.31 | Build system |
| Node.js | 18 | Web service runtime |
| npm | 9 | Node.js package manager |

## 1. Server Setup

Start with a fresh Ubuntu server (e.g., an AWS EC2 instance, a DigitalOcean
droplet, or a GitHub-hosted runner).

### Update the system

```bash
sudo apt-get update && sudo apt-get upgrade -y
```

### Install basic tools

```bash
sudo apt-get install -y build-essential curl git zip unzip wget
```

### Configure the firewall (optional)

If using `ufw`, allow SSH and the web service port:

```bash
sudo ufw allow ssh
sudo ufw allow 3000/tcp   # or your chosen port
sudo ufw enable
```

### Install Node.js

Using the NodeSource repository (recommended for up-to-date versions):

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
node --version   # should be v20.x
npm --version
```

## 2. Install gtopt Dependencies

### GCC 14

```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14
```

### Apache Arrow / Parquet

```bash
sudo apt-get install -y -V ca-certificates lsb-release wget
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short \
  | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release \
  --codename --short).deb
sudo apt-get update
sudo apt-get install -y -V libarrow-dev libparquet-dev
```

### CBC Solver

```bash
sudo apt-get install -y -V coinor-libcbc-dev
```

### Boost

```bash
sudo apt-get install -y -V libboost-container-dev
```

## 3. Build and Install gtopt

Clone the repository, build, and install:

```bash
git clone https://github.com/marcelomatus/gtopt.git /tmp/gtopt-build
cd /tmp/gtopt-build

# Configure
export CC=gcc-14
export CXX=g++-14
cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Install to /usr/local (makes `gtopt` available system-wide)
sudo cmake --install build
```

Verify the installation:

```bash
gtopt --version
gtopt --help
```

To install to a custom prefix instead:

```bash
cmake --install build --prefix /opt/gtopt
# Binary will be at /opt/gtopt/bin/gtopt
```

## 4. Install the Web Service

From the cloned repository (the source tree is only needed for installation,
not for running the service):

```bash
cd /tmp/gtopt-build

# Configure and install the webservice via CMake
cmake -S webservice -B build-web
sudo cmake --install build-web
```

The install step automatically runs `npm install` and `npm run build` in the
destination directory. If npm is not available or the build fails, a warning is
printed with manual instructions.

The source tree (`/tmp/gtopt-build`) can be removed after installation.

After installation:
- The `gtopt_websrv` launcher is at `/usr/local/bin/gtopt_websrv`
- The web service files are at `/usr/local/share/gtopt/webservice/`

## 5. Configure the Web Service

The `gtopt_websrv` launcher accepts command-line options and environment
variables:

| Option / Variable | Default | Description |
|-------------------|---------|-------------|
| `--port` / `PORT` | `3000` | HTTP port the web service listens on |
| `--gtopt-bin` / `GTOPT_BIN` | auto-detect | Absolute path to the gtopt binary |
| `--data-dir` / `GTOPT_DATA_DIR` | `./data` | Directory for uploaded cases and results |
| `--log-dir` / `GTOPT_LOG_DIR` | *(console only)* | Directory for log files |

Ensure the data directory exists and is writable:

```bash
sudo mkdir -p /var/lib/gtopt/data
sudo chown $USER:$USER /var/lib/gtopt/data
```

Optionally create a log directory:

```bash
sudo mkdir -p /var/log/gtopt
sudo chown $USER:$USER /var/log/gtopt
```

## 6. Run the Web Service

### Using gtopt_websrv (recommended)

```bash
# Basic start (auto-detects gtopt binary)
gtopt_websrv

# With explicit options
gtopt_websrv --port 8080 --gtopt-bin /usr/local/bin/gtopt

# With data and log directories
gtopt_websrv --data-dir /var/lib/gtopt/data --log-dir /var/log/gtopt
```

### Development mode (with hot reload)

```bash
gtopt_websrv --dev
```

### Using npm directly from the installed location

```bash
cd /usr/local/share/gtopt/webservice
GTOPT_BIN=/usr/local/bin/gtopt npm run start
```

The service will be available at `http://localhost:3000` (or whatever port you
configured).

## 7. Run as a System Service

For production deployments, use systemd to run the web service as a background
service that starts on boot.

### Copy the systemd unit file

A service file template is included with the installation:

```bash
sudo cp /usr/local/share/gtopt/webservice/gtopt-webservice.service \
        /etc/systemd/system/

# Edit the service file if needed (e.g., change port, user, paths)
sudo nano /etc/systemd/system/gtopt-webservice.service
```

### Create a dedicated service user (recommended)

```bash
sudo useradd -r -m -s /bin/bash gtopt
sudo mkdir -p /var/lib/gtopt/data
sudo mkdir -p /var/log/gtopt
sudo chown -R gtopt:gtopt /var/lib/gtopt /var/log/gtopt
```

### Enable and start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable gtopt-webservice
sudo systemctl start gtopt-webservice

# Check status
sudo systemctl status gtopt-webservice

# View logs
sudo journalctl -u gtopt-webservice -f
```

## 8. Reverse Proxy (nginx)

For production, place nginx in front of the Node.js service to handle TLS,
caching, and standard HTTP port mapping.

### Install nginx

```bash
sudo apt-get install -y nginx
```

### Configure a site

```bash
sudo tee /etc/nginx/sites-available/gtopt << 'EOF'
server {
    listen 80;
    server_name your-domain.com;

    client_max_body_size 100M;  # Allow large case file uploads

    location / {
        proxy_pass http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection 'upgrade';
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_cache_bypass $http_upgrade;
    }
}
EOF

sudo ln -sf /etc/nginx/sites-available/gtopt /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

### Add TLS with Let's Encrypt (optional)

```bash
sudo apt-get install -y certbot python3-certbot-nginx
sudo certbot --nginx -d your-domain.com
```

## 9. Testing

### Mock integration tests (no gtopt binary needed)

These tests use a mock solver to validate the web service API:

```bash
cd /usr/local/share/gtopt/webservice
npm test
```

### End-to-end tests (requires built gtopt)

These tests use the real gtopt binary and validate solver output against
reference results from `cases/c0`:

```bash
cd /usr/local/share/gtopt/webservice
GTOPT_BIN=/usr/local/bin/gtopt npm run test:e2e
```

### Manual test with curl

```bash
# Create a test archive from the included sample case
cd cases/c0
zip -r /tmp/case_c0.zip system_c0.json system_c0/
cd ../..

# Submit the case
curl -X POST http://localhost:3000/api/jobs \
  -F "file=@/tmp/case_c0.zip" \
  -F "systemFile=system_c0.json"
# Returns: {"token":"<job-token>","status":"pending",...}

# Check status (replace <job-token> with actual token)
curl http://localhost:3000/api/jobs/<job-token>

# Download results when completed
curl -o results.zip http://localhost:3000/api/jobs/<job-token>/download
unzip -l results.zip
```

## 10. API Reference

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/jobs` | Upload a `.zip` case archive and start an optimization job |
| `GET` | `/api/jobs` | List all submitted jobs |
| `GET` | `/api/jobs/:token` | Get the status of a specific job |
| `GET` | `/api/jobs/:token/download` | Download results as a `.zip` file |

### POST /api/jobs

**Request** (multipart/form-data):
- `file` — `.zip` archive containing the case directory
- `systemFile` — name of the system JSON file inside the archive

**Response** (201):
```json
{
  "token": "550e8400-e29b-41d4-a716-446655440000",
  "status": "pending",
  "message": "Job submitted successfully. Use the token to check status and download results."
}
```

### GET /api/jobs/:token

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

Job statuses: `pending`, `running`, `completed`, `failed`.

### GET /api/jobs/:token/download

Returns a binary `.zip` file containing:
- `output/` — solver output files (CSV/Parquet)
- `stdout.log` — solver standard output
- `stderr.log` — solver standard error
- `job.json` — job metadata

## 11. Troubleshooting

### "gtopt binary not found"

Ensure `GTOPT_BIN` points to the correct path or is installed in `/usr/local/bin`:

```bash
which gtopt                    # if installed system-wide
ls -la /usr/local/bin/gtopt    # check the binary exists and is executable
gtopt_websrv --gtopt-bin /usr/local/bin/gtopt  # specify explicitly
```

### Web service won't start

Check Node.js and npm versions:

```bash
node --version   # needs 18+
npm --version
```

Rebuild if needed:

```bash
cd /usr/local/share/gtopt/webservice
rm -rf node_modules .next
npm install
npm run build
```

### Upload fails or times out

- Check `client_max_body_size` in nginx configuration (default is 1MB).
- Ensure the data directory is writable: `ls -la $GTOPT_DATA_DIR`.

### Job stays in "pending" or "running"

Check server logs:

```bash
# If running with systemd:
sudo journalctl -u gtopt-webservice -n 50

# If using --log-dir:
tail -f /var/log/gtopt/gtopt-webservice.log
```

### Port already in use

```bash
# Find what's using the port
sudo lsof -i :3000
# Kill it or use a different port via the PORT environment variable
```
