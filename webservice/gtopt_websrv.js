#!/usr/bin/env node
/**
 * gtopt_websrv - Standalone launcher for the gtopt web service.
 * 
 * This script starts the Next.js web service that provides a REST API for
 * uploading gtopt cases, running the solver, and downloading results.
 * 
 * Usage:
 *     gtopt_websrv [options]
 * 
 * Options:
 *     --port PORT          Port for the web service (default: 3000)
 *     --hostname HOST      Hostname/IP to bind to (default: 0.0.0.0)
 *     --gtopt-bin PATH     Path to gtopt binary (default: auto-detect)
 *     --data-dir PATH      Directory for job data storage (default: ./data)
 *     --log-dir PATH       Directory for log files (default: logs to console only)
 *     --help               Show this help message
 * 
 * Environment Variables:
 *     PORT                 Web service port (default: 3000)
 *     GTOPT_HOSTNAME       Hostname/IP to bind to (default: 0.0.0.0)
 *     GTOPT_BIN            Path to gtopt binary
 *     GTOPT_DATA_DIR       Directory for job data storage
 *     GTOPT_LOG_DIR        Directory for log files
 */

const fs = require('fs');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

function showHelp() {
  console.log(`
gtopt_websrv - Launcher for gtopt web service

Usage:
  gtopt_websrv [options]

Options:
  --port PORT          Port for the web service (default: 3000)
  --hostname HOST      Hostname/IP to bind to (default: 0.0.0.0)
  --gtopt-bin PATH     Path to gtopt binary (default: auto-detect)
  --data-dir PATH      Directory for job data storage (default: ./data)
  --log-dir PATH       Directory for log files (default: console only)
  --dev                Run in development mode
  --check-api          Check if the API is responding on the given port and exit
  --help               Show this help message

Environment Variables:
  PORT                 Web service port (default: 3000)
  GTOPT_HOSTNAME       Hostname/IP to bind to (default: 0.0.0.0)
  GTOPT_BIN            Path to gtopt binary
  GTOPT_DATA_DIR       Directory for job data storage
  GTOPT_LOG_DIR        Directory for log files

Examples:
  gtopt_websrv                           # Start on default port 3000
  gtopt_websrv --port 8080               # Start on port 8080
  gtopt_websrv --hostname 127.0.0.1      # Bind to localhost only
  gtopt_websrv --gtopt-bin /usr/bin/gtopt  # Use specific gtopt binary
  gtopt_websrv --log-dir /var/log/gtopt  # Write logs to directory
  gtopt_websrv --dev                     # Run in development mode
  gtopt_websrv --check-api               # Check API on port 3000
  gtopt_websrv --check-api --port 8080   # Check API on port 8080
`);
}

function getWebserviceDir() {
  // This script is in bin/, webservice is in ../share/gtopt/webservice/
  const scriptDir = path.dirname(__dirname);
  
  // Check common installation locations
  const possibleLocations = [
    path.join(scriptDir, 'share', 'gtopt', 'webservice'),
    path.join(scriptDir, '..', 'share', 'gtopt', 'webservice'),
    __dirname,           // Script is inside the webservice directory itself
    path.join(__dirname, '..'), // During development
  ];
  
  for (const location of possibleLocations) {
    const packageJson = path.join(location, 'package.json');
    if (fs.existsSync(packageJson)) {
      return location;
    }
  }
  
  console.error('Error: Cannot find webservice directory');
  console.error('Searched locations:', possibleLocations);
  process.exit(1);
}

function findGtoptBinary() {
  // Try to find gtopt in PATH or common locations
  const { execSync } = require('child_process');
  
  // First try the installed location relative to this script
  const scriptDir = path.dirname(__dirname);
  const installedBin = path.join(scriptDir, 'bin', 'gtopt');
  if (fs.existsSync(installedBin)) {
    return installedBin;
  }
  
  // Try to find in PATH
  try {
    const result = execSync('which gtopt', { encoding: 'utf8' });
    return result.trim();
  } catch (e) {
    // Not found in PATH
  }
  
  // Try common locations
  const commonLocations = [
    '/usr/local/bin/gtopt',
    '/usr/bin/gtopt',
    path.join(process.env.HOME || '', '.local', 'bin', 'gtopt'),
  ];
  
  for (const location of commonLocations) {
    if (fs.existsSync(location)) {
      return location;
    }
  }
  
  return null;
}

function parseArgs() {
  const args = process.argv.slice(2);
  const config = {
    port: process.env.PORT || '3000',
    hostname: process.env.GTOPT_HOSTNAME || '0.0.0.0',
    gtoptBin: process.env.GTOPT_BIN || null,
    dataDir: process.env.GTOPT_DATA_DIR || null,
    logDir: process.env.GTOPT_LOG_DIR || null,
    dev: false,
    help: false,
    checkApi: false,
  };
  
  for (let i = 0; i < args.length; i++) {
    switch (args[i]) {
      case '--help':
      case '-h':
        config.help = true;
        break;
      case '--port':
        config.port = args[++i];
        break;
      case '--hostname':
        config.hostname = args[++i];
        break;
      case '--gtopt-bin':
        config.gtoptBin = args[++i];
        break;
      case '--data-dir':
        config.dataDir = args[++i];
        break;
      case '--log-dir':
        config.logDir = args[++i];
        break;
      case '--dev':
        config.dev = true;
        break;
      case '--check-api':
        config.checkApi = true;
        break;
      default:
        console.error(`Unknown option: ${args[i]}`);
        process.exit(1);
    }
  }
  
  return config;
}

function logMessage(msg, logDir) {
  console.log(msg);
  if (logDir) {
    const logFile = path.join(logDir, 'gtopt-webservice.log');
    try {
      fs.mkdirSync(path.dirname(logFile), { recursive: true });
      fs.appendFileSync(logFile, msg + '\n');
    } catch (_) {
      // ignore write errors
    }
  }
}

function httpGet(url, timeout) {
  return new Promise((resolve) => {
    // Force IPv4 for 'localhost' to avoid IPv6 ECONNREFUSED on systems where
    // localhost resolves to ::1 but the server only listens on 0.0.0.0 (IPv4).
    const opts = { timeout };
    try {
      if (new URL(url).hostname === 'localhost') {
        opts.family = 4;
      }
    } catch (_) {
      // malformed URL — let http.get handle the error
    }
    const req = http.get(url, opts, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try {
          resolve({ status: res.statusCode, body: JSON.parse(data), raw: data });
        } catch (_) {
          resolve({ status: res.statusCode, body: null, raw: data });
        }
      });
    });
    req.on('error', (err) => resolve({ error: err.message }));
    req.on('timeout', () => { req.destroy(); resolve({ error: 'timeout' }); });
  });
}

async function verifyApi(port, logDir, timeout) {
  const maxWait = timeout || 30;
  const baseUrls = [`http://127.0.0.1:${port}`, `http://localhost:${port}`];
  const startTime = Date.now();

  logMessage('Verifying API endpoints...', logDir);

  // Poll until the API responds or timeout
  let apiResult = null;
  let apiBaseUrl = null;
  let lastError = null;
  while ((Date.now() - startTime) / 1000 < maxWait) {
    for (const baseUrl of baseUrls) {
      const result = await httpGet(`${baseUrl}/api`, 5000);
      if (result && result.status === 200 && result.body && result.body.status === 'ok') {
        apiResult = result;
        apiBaseUrl = baseUrl;
        break;
      }
      lastError = result;
    }
    if (apiResult) {
      break;
    }
    const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
    const errDetail = lastError
      ? (lastError.error ? `error: ${lastError.error}` : `HTTP ${lastError.status}`)
      : 'no response';
    logMessage(`  ... waiting for API (${elapsed}s elapsed, last attempt: ${errDetail})`, logDir);
    await new Promise((r) => setTimeout(r, 1000));
  }

  if (apiResult) {
    logMessage(`API verification PASSED: GET ${apiBaseUrl}/api returned status "ok"`, logDir);
  } else {
    const errDetail = lastError
      ? (lastError.error
          ? `last error: ${lastError.error}`
          : `last HTTP status: ${lastError.status}, body: ${(lastError.raw || '').slice(0, 200)}`)
      : 'no response received';
    logMessage(`API verification FAILED: GET /api did not respond within ${maxWait}s at ${baseUrls.join(' or ')}`, logDir);
    logMessage(`  ${errDetail}`, logDir);
    logMessage('Hint (WSL): this check already tried 127.0.0.1 and localhost. If it still fails, check firewall/port-forwarding/port-collision settings (see GTOPT_WEBSRV.md).', logDir);
    return false;
  }

  // Also check /api/ping — accept the response as long as the endpoint
  // responds with HTTP 200 and the body contains service: "gtopt-webservice".
  // The "status" field may be "ok" or "error" (e.g. when the gtopt binary is
  // not found) — either way the *webservice* is healthy.
  const pingResult = await httpGet(`${apiBaseUrl}/api/ping`, 5000);
  if (pingResult && pingResult.status === 200 && pingResult.body && pingResult.body.service === 'gtopt-webservice') {
    const info = pingResult.body;
    logMessage(`API verification PASSED: GET ${apiBaseUrl}/api/ping returned status "${info.status}", service="${info.service}"`, logDir);
    if (info.gtopt_version) {
      logMessage(`  gtopt version: ${info.gtopt_version}`, logDir);
    }
    if (info.gtopt_bin) {
      logMessage(`  gtopt binary: ${info.gtopt_bin}`, logDir);
    } else {
      logMessage(`  gtopt binary: not found (solving will not be available until configured)`, logDir);
    }
    if (info.log_file) {
      logMessage(`  log file: ${info.log_file}`, logDir);
    }
  } else {
    logMessage(`API verification WARNING: GET ${apiBaseUrl}/api/ping did not return expected response`, logDir);
    if (pingResult) {
      if (pingResult.error) {
        logMessage(`  Error: ${pingResult.error}`, logDir);
      } else {
        logMessage(`  HTTP status: ${pingResult.status}, body: ${(pingResult.raw || '').slice(0, 200)}`, logDir);
      }
    } else {
      logMessage(`  No response received from /api/ping`, logDir);
    }
  }

  logMessage('API verification complete.', logDir);
  return true;
}

function main() {
  const config = parseArgs();
  
  if (config.help) {
    showHelp();
    process.exit(0);
  }

  // --check-api mode: verify API on the given port and exit
  if (config.checkApi) {
    verifyApi(config.port, config.logDir, 5).then((ok) => {
      process.exit(ok ? 0 : 1);
    });
    return;
  }
  
  // Find webservice directory
  const webserviceDir = getWebserviceDir();
  console.log(`Using webservice from: ${webserviceDir}`);
  
  // Find or verify gtopt binary
  let gtoptBin = config.gtoptBin;
  if (!gtoptBin) {
    gtoptBin = findGtoptBinary();
  }
  
  if (gtoptBin && !fs.existsSync(gtoptBin)) {
    console.warn(`Warning: gtopt binary not found at: ${gtoptBin}`);
    gtoptBin = null;
  }

  if (gtoptBin) {
    console.log(`Using gtopt binary: ${gtoptBin}`);
  } else {
    console.warn('Warning: gtopt binary not found. The webservice will start but solving will not be available.');
    console.warn('  Specify the gtopt binary location with: gtopt_websrv --gtopt-bin /path/to/gtopt');
    console.warn('  or set the GTOPT_BIN environment variable.');
  }

  console.log(`Starting web service on port ${config.port}...`);
  
  // Set up environment — remove the system HOSTNAME variable so that
  // Next.js does not accidentally use it as the listen address (on many
  // Linux systems HOSTNAME is set to the machine name which is not a
  // valid bind address).
  const env = {
    ...process.env,
    PORT: config.port,
  };
  delete env.HOSTNAME;
  
  if (gtoptBin) {
    env.GTOPT_BIN = gtoptBin;
  }
  
  if (config.dataDir) {
    env.GTOPT_DATA_DIR = config.dataDir;
  }

  if (config.logDir) {
    env.GTOPT_LOG_DIR = config.logDir;
    console.log(`Log directory: ${config.logDir}`);
  }
  
  // Check if node_modules exists
  const nodeModules = path.join(webserviceDir, 'node_modules');
  if (!fs.existsSync(nodeModules)) {
    console.error('Error: node_modules not found in webservice directory');
    console.error('');
    console.error('Please install dependencies first:');
    console.error(`  cd ${webserviceDir}`);
    console.error('  npm install --production');
    process.exit(1);
  }
  
  // Check if .next directory exists (production build)
  const nextDir = path.join(webserviceDir, '.next');
  if (!config.dev && !fs.existsSync(nextDir)) {
    console.error('Error: Production build not found (.next directory)');
    console.error('');
    console.error('Please build the webservice first:');
    console.error(`  cd ${webserviceDir}`);
    console.error('  npm run build');
    console.error('');
    console.error('Or run in development mode:');
    console.error('  gtopt_websrv --dev');
    process.exit(1);
  }
  
  // Start the web service — call next directly with --hostname so the
  // server binds to the requested interface.  Using next directly (rather
  // than npm run start) avoids an extra process layer and allows us to
  // pass --hostname and --port reliably on all platforms including WSL.
  const nextBinBase = path.join(webserviceDir, 'node_modules', '.bin', 'next');
  const nextBin = process.platform === 'win32' ? nextBinBase + '.cmd' : nextBinBase;
  const npmScript = config.dev ? 'dev' : 'start';
  
  console.log('');
  console.log('='.repeat(60));
  console.log(`gtopt web service is starting (${config.dev ? 'development' : 'production'} mode)`);
  console.log(`URL: http://localhost:${config.port}`);
  console.log(`Hostname: ${config.hostname}`);
  console.log('Press Ctrl+C to stop');
  console.log('='.repeat(60));
  console.log('');
  
  const nextArgs = [npmScript, '--hostname', config.hostname, '--port', String(config.port)];
  logMessage(`Launching: ${nextBin} ${nextArgs.join(' ')}`, config.logDir);
  logMessage(`Working directory: ${webserviceDir}`, config.logDir);
  logMessage(`Node.js: ${process.version}, platform: ${process.platform}, arch: ${process.arch}`, config.logDir);
  const proc = spawn(nextBin, nextArgs, {
    cwd: webserviceDir,
    env: env,
    stdio: ['inherit', 'pipe', 'pipe'],
  });

  // Detect when Next.js signals readiness (prints "Ready in" to stdout).
  // Forward all output to the parent process so the user still sees it.
  let serverReady = false;
  let onServerReady = () => {};
  const serverReadyPromise = new Promise((resolve) => { onServerReady = resolve; });
  const launchTime = Date.now();

  proc.stdout.on('data', (chunk) => {
    process.stdout.write(chunk);
    if (!serverReady && chunk.toString().includes('Ready in')) {
      serverReady = true;
      logMessage(`Server ready (${((Date.now() - launchTime) / 1000).toFixed(1)}s after launch)`, config.logDir);
      onServerReady();
    }
  });

  proc.stderr.on('data', (chunk) => {
    process.stderr.write(chunk);
  });
  
  proc.on('error', (err) => {
    console.error('Failed to start web service:', err);
    process.exit(1);
  });
  
  proc.on('exit', (code) => {
    console.log(`\nWeb service exited with code ${code}`);
    process.exit(code || 0);
  });
  
  // Handle shutdown signals
  process.on('SIGINT', () => {
    console.log('\nReceived SIGINT, shutting down...');
    proc.kill('SIGINT');
  });
  
  process.on('SIGTERM', () => {
    console.log('\nReceived SIGTERM, shutting down...');
    proc.kill('SIGTERM');
  });

  // Verify API is working after server signals readiness.
  // Wait for the "Ready in" message from Next.js (up to 30 s) before
  // polling the HTTP endpoints, so we avoid noisy ECONNREFUSED messages
  // during startup.
  const readyTimeout = new Promise((resolve) => setTimeout(() => resolve('timeout'), 30000));
  Promise.race([serverReadyPromise, readyTimeout]).then((result) => {
    if (result === 'timeout') {
      logMessage('Warning: server did not signal readiness within 30s, attempting API verification anyway', config.logDir);
    }
    return verifyApi(config.port, config.logDir, 30);
  }).then((ok) => {
    if (!ok) {
      console.error('Warning: API verification failed. The service may not be working correctly.');
    }
  });
}

if (require.main === module) {
  main();
}
