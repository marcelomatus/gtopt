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

Clone the repository and build the standalone solver:

```bash
git clone https://github.com/marcelomatus/gtopt.git
cd gtopt

# Configure
cmake -S standalone -B build -DCMAKE_BUILD_TYPE=Release
export CC=gcc-14
export CXX=g++-14

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

From the repository root:

```bash
cd webservice

# Install Node.js dependencies
npm ci

# Build the Next.js application for production
npm run build
```

### Using CMake (alternative)

If building from the standalone CMake tree:

```bash
# From repository root
cmake --build build --target webservice-install
cmake --build build --target webservice-build
```

## 5. Configure the Web Service

The web service is configured through environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `GTOPT_BIN` | `../build/gtopt` | Absolute path to the gtopt binary |
| `GTOPT_DATA_DIR` | `./data` | Directory for uploaded cases and results |
| `PORT` | `3000` | HTTP port the web service listens on |

Create a `.env.local` file in the `webservice/` directory for persistent
configuration:

```bash
cat > webservice/.env.local << 'EOF'
GTOPT_BIN=/usr/local/bin/gtopt
GTOPT_DATA_DIR=/var/lib/gtopt/data
PORT=3000
EOF
```

Ensure the data directory exists and is writable:

```bash
sudo mkdir -p /var/lib/gtopt/data
sudo chown $USER:$USER /var/lib/gtopt/data
```

## 6. Run the Web Service

### Development mode (with hot reload)

```bash
cd webservice
GTOPT_BIN=/usr/local/bin/gtopt npm run dev
```

### Production mode

```bash
cd webservice
GTOPT_BIN=/usr/local/bin/gtopt npm run start
```

The service will be available at `http://localhost:3000` (or whatever port you
configured).

### Using CMake

```bash
cmake --build build --target webservice-start
```

## 7. Run as a System Service

For production deployments, use systemd to run the web service as a background
service that starts on boot.

### Create a systemd unit file

```bash
sudo tee /etc/systemd/system/gtopt-webservice.service << 'EOF'
[Unit]
Description=gtopt Web Service
After=network.target

[Service]
Type=simple
User=gtopt
Group=gtopt
WorkingDirectory=/home/gtopt/gtopt/webservice
Environment=NODE_ENV=production
Environment=GTOPT_BIN=/usr/local/bin/gtopt
Environment=GTOPT_DATA_DIR=/var/lib/gtopt/data
Environment=PORT=3000
ExecStart=/usr/bin/npm run start
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF
```

### Create a dedicated service user (recommended)

```bash
sudo useradd -r -m -s /bin/bash gtopt
sudo mkdir -p /var/lib/gtopt/data
sudo chown -R gtopt:gtopt /var/lib/gtopt
# Copy or clone the repo under /home/gtopt/gtopt and build there
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
cd webservice
npm test
```

### End-to-end tests (requires built gtopt)

These tests use the real gtopt binary and validate solver output against
reference results from `cases/c0`:

```bash
cd webservice
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

Ensure `GTOPT_BIN` points to the correct path:

```bash
which gtopt                    # if installed system-wide
ls -la /usr/local/bin/gtopt    # check the binary exists and is executable
```

### Web service won't start

Check Node.js and npm versions:

```bash
node --version   # needs 18+
npm --version
```

Rebuild if needed:

```bash
cd webservice
rm -rf node_modules .next
npm ci
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

# If running directly:
# Check the terminal output or server.log
```

### Port already in use

```bash
# Find what's using the port
sudo lsof -i :3000
# Kill it or use a different port via the PORT environment variable
```
