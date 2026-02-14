# Installing and Running the gtopt GUI Service

This guide covers setting up a server, installing the gtopt GUI service, and
running it in development or production. The GUI service provides a web-based
graphical interface for creating, editing, and visualizing gtopt optimization
cases, with integrated connectivity to the
[gtopt webservice](INSTALL_WEBSERVICE.md) for remote solving.

## Table of Contents

- [Prerequisites](#prerequisites)
- [1. Server Setup](#1-server-setup)
- [2. Install Python and Dependencies](#2-install-python-and-dependencies)
- [3. Install the GUI Service](#3-install-the-gui-service)
- [4. Configure the GUI Service](#4-configure-the-gui-service)
- [5. Run the GUI Service](#5-run-the-gui-service)
- [6. Run as a System Service](#6-run-as-a-system-service)
- [7. Reverse Proxy (nginx)](#7-reverse-proxy-nginx)
- [8. Connecting to the gtopt Webservice](#8-connecting-to-the-gtopt-webservice)
- [9. Testing](#9-testing)
- [10. API Reference](#10-api-reference)
- [11. Troubleshooting](#11-troubleshooting)

---

## Prerequisites

| Component | Minimum Version | Purpose |
|-----------|----------------|---------|
| Ubuntu | 22.04 LTS | Server OS (other Linux distros work with adjustments) |
| Python | 3.10 | Runtime for the Flask application |
| pip | 22 | Python package manager |
| (Optional) gtopt webservice | — | Remote solver (see [INSTALL_WEBSERVICE.md](INSTALL_WEBSERVICE.md)) |

## 1. Server Setup

Start with a fresh Ubuntu server (e.g., an AWS EC2 instance, a DigitalOcean
droplet, or a local machine).

### Update the system

```bash
sudo apt-get update && sudo apt-get upgrade -y
```

### Install basic tools

```bash
sudo apt-get install -y build-essential curl git zip unzip wget
```

### Configure the firewall (optional)

If using `ufw`, allow SSH and the GUI service port:

```bash
sudo ufw allow ssh
sudo ufw allow 5001/tcp   # or your chosen port
sudo ufw enable
```

## 2. Install Python and Dependencies

### Install Python 3.12 (recommended)

On Ubuntu 24.04, Python 3.12 is available by default:

```bash
sudo apt-get install -y python3 python3-pip python3-venv
python3 --version   # should be 3.10+
```

On older Ubuntu versions, use the deadsnakes PPA:

```bash
sudo add-apt-repository -y ppa:deadsnakes/ppa
sudo apt-get update
sudo apt-get install -y python3.12 python3.12-venv python3.12-dev
```

### Create a virtual environment (recommended)

```bash
python3 -m venv /opt/guiservice/venv
source /opt/guiservice/venv/bin/activate
```

## 3. Install the GUI Service

### Clone the repository

```bash
git clone https://github.com/marcelomatus/gtopt.git
cd gtopt
```

### Install Python dependencies

```bash
pip install -r guiservice/requirements.txt
```

The dependencies are:

| Package | Version | Purpose |
|---------|---------|---------|
| Flask | ≥ 3.1.1 | Web framework |
| pandas | ≥ 2.2.3 | Data manipulation for CSV/Parquet handling |
| pyarrow | ≥ 19.0.1 | Parquet file support |
| requests | ≥ 2.32.3 | HTTP client for webservice communication |

### Verify the installation

```bash
cd guiservice
python app.py &
curl -s http://localhost:5001/api/schemas | python3 -m json.tool | head -5
# Should output JSON with element schemas (bus, generator, demand, etc.)
kill %1
```

## 4. Configure the GUI Service

The GUI service is configured through environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `GTOPT_WEBSERVICE_URL` | `http://localhost:3000` | URL of the gtopt webservice for remote solving |
| `FLASK_DEBUG` | `0` | Set to `1` for debug mode with auto-reload |

Create a configuration file for persistent settings:

```bash
cat > /opt/guiservice/.env << 'EOF'
GTOPT_WEBSERVICE_URL=http://localhost:3000
FLASK_DEBUG=0
EOF
```

The webservice URL can also be changed at runtime through the GUI (Solver →
Webservice panel) or via the API:

```bash
curl -X POST http://localhost:5001/api/solve/config \
  -H "Content-Type: application/json" \
  -d '{"webservice_url": "http://my-server:3000"}'
```

## 5. Run the GUI Service

### Development mode (with hot reload)

```bash
cd guiservice
FLASK_DEBUG=1 python app.py
```

The GUI will be available at `http://localhost:5001`.

### Production mode with gunicorn

For production deployments, use a WSGI server like gunicorn:

```bash
pip install gunicorn
cd guiservice
gunicorn -w 4 -b 0.0.0.0:5001 app:app
```

### Quick start (no webservice required)

The GUI service works standalone for case creation, editing, and download.
No gtopt webservice is required for these features:

```bash
cd guiservice
python app.py
# Open http://localhost:5001 in your browser
```

## 6. Run as a System Service

For production deployments, use systemd to run the GUI service as a background
service that starts on boot.

### Create a systemd unit file

```bash
sudo tee /etc/systemd/system/gtopt-guiservice.service << 'EOF'
[Unit]
Description=gtopt GUI Service
After=network.target

[Service]
Type=simple
User=gtopt
Group=gtopt
WorkingDirectory=/home/gtopt/gtopt/guiservice
Environment=GTOPT_WEBSERVICE_URL=http://localhost:3000
ExecStart=/opt/guiservice/venv/bin/gunicorn -w 4 -b 0.0.0.0:5001 app:app
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF
```

### Create a dedicated service user (recommended)

```bash
sudo useradd -r -m -s /bin/bash gtopt
# Copy or clone the repo under /home/gtopt/gtopt and install there
```

### Enable and start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable gtopt-guiservice
sudo systemctl start gtopt-guiservice

# Check status
sudo systemctl status gtopt-guiservice

# View logs
sudo journalctl -u gtopt-guiservice -f
```

## 7. Reverse Proxy (nginx)

For production, place nginx in front of the Flask application to handle TLS,
static file serving, and standard HTTP port mapping.

### Install nginx

```bash
sudo apt-get install -y nginx
```

### Configure a site

```bash
sudo tee /etc/nginx/sites-available/gtopt-gui << 'EOF'
server {
    listen 80;
    server_name your-domain.com;

    client_max_body_size 100M;  # Allow large case file uploads

    location / {
        proxy_pass http://127.0.0.1:5001;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
EOF

sudo ln -sf /etc/nginx/sites-available/gtopt-gui /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

### Add TLS with Let's Encrypt (optional)

```bash
sudo apt-get install -y certbot python3-certbot-nginx
sudo certbot --nginx -d your-domain.com
```

## 8. Connecting to the gtopt Webservice

The GUI service can submit cases to the gtopt webservice for solving. This
requires a running webservice instance (see
[INSTALL_WEBSERVICE.md](INSTALL_WEBSERVICE.md)).

### From the GUI

1. Start the gtopt webservice (default: `http://localhost:3000`)
2. Open the GUI at `http://localhost:5001`
3. Navigate to **Solver → Webservice** in the left panel
4. Enter the webservice URL and click **Test** to verify the connection
5. Use the **⚡ Solve** button in the header to submit cases

### From the command line

```bash
# Set the webservice URL
curl -X POST http://localhost:5001/api/solve/config \
  -H "Content-Type: application/json" \
  -d '{"webservice_url": "http://localhost:3000"}'

# Submit a case (requires JSON case data)
curl -X POST http://localhost:5001/api/solve/submit \
  -H "Content-Type: application/json" \
  -d @case_data.json
# Returns: {"token":"<job-token>","status":"pending",...}

# Check status
curl http://localhost:5001/api/solve/status/<job-token>

# Get results when completed
curl http://localhost:5001/api/solve/results/<job-token>
```

## 9. Testing

### Run the test suite

```bash
pip install pytest
python -m pytest guiservice/tests/test_app.py -v
```

The test suite includes:

| Test class | Tests | Description |
|------------|-------|-------------|
| `TestBuildCaseJson` | 5 | Unit tests for JSON case building |
| `TestBuildZip` | 4 | Unit tests for ZIP archive generation |
| `TestParseUploadedZip` | 2 | Unit tests for ZIP parsing |
| `TestRoutes` | 8 | Integration tests for Flask routes |
| `TestRoundTrip` | 1 | Download and re-upload round-trip |
| `TestSolveConfig` | 5 | Webservice URL configuration |
| `TestSolveSubmit` | 3 | Job submission to webservice |
| `TestSolveStatus` | 2 | Job status polling |
| `TestSolveResults` | 2 | Results retrieval and parsing |
| `TestSolveJobsList` | 2 | Jobs listing from webservice |
| `TestWebserviceEndToEnd` | 6 | Full workflow: submit → poll → results |

### Run tests in CI

The GitHub Actions workflow `.github/workflows/guiservice.yml` runs the test
suite automatically on push and pull request:

```yaml
- name: Run tests
  run: python -m pytest guiservice/tests/test_app.py -v
```

## 10. API Reference

### Case Management

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/` | Main GUI page |
| `GET` | `/api/schemas` | Element field schemas |
| `POST` | `/api/case/download` | Generate and download case ZIP |
| `POST` | `/api/case/upload` | Upload a case ZIP for editing |
| `POST` | `/api/case/preview` | Preview the generated JSON |
| `POST` | `/api/results/upload` | Upload results ZIP for viewing |

### Webservice Integration

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/solve/config` | Get webservice URL configuration |
| `POST` | `/api/solve/config` | Set webservice URL |
| `POST` | `/api/solve/submit` | Submit case to webservice for solving |
| `GET` | `/api/solve/status/<token>` | Poll job status from webservice |
| `GET` | `/api/solve/results/<token>` | Retrieve and parse results from webservice |
| `GET` | `/api/solve/jobs` | List all jobs from webservice |

## 11. Troubleshooting

### GUI service won't start

Check Python version and dependencies:

```bash
python3 --version   # needs 3.10+
pip list | grep -E "flask|pandas|pyarrow|requests"
```

Reinstall if needed:

```bash
pip install -r guiservice/requirements.txt
```

### "Cannot connect to webservice"

Ensure the gtopt webservice is running and accessible:

```bash
curl http://localhost:3000/api/jobs
```

If running on a different host, update the webservice URL via the GUI or API.

### Upload fails or times out

- Check file size: the default limit is 100 MB.
- If using nginx, set `client_max_body_size 100M;` in the server block.
- Ensure the server has enough disk space for temporary files.

### Port already in use

```bash
# Find what's using the port
sudo lsof -i :5001
# Kill it or use a different port
```

To use a different port, modify the `app.run()` call in `app.py` or set it
via gunicorn:

```bash
gunicorn -w 4 -b 0.0.0.0:8080 app:app
```

### Tests fail with import errors

Make sure you run tests from the repository root:

```bash
cd /path/to/gtopt
python -m pytest guiservice/tests/test_app.py -v
```
