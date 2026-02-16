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
 *     --gtopt-bin PATH     Path to gtopt binary (default: auto-detect)
 *     --data-dir PATH      Directory for job data storage (default: ./data)
 *     --log-dir PATH       Directory for log files (default: logs to console only)
 *     --help               Show this help message
 * 
 * Environment Variables:
 *     PORT                 Web service port (default: 3000)
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
  --gtopt-bin PATH     Path to gtopt binary (default: auto-detect)
  --data-dir PATH      Directory for job data storage (default: ./data)
  --log-dir PATH       Directory for log files (default: console only)
  --dev                Run in development mode
  --check-api          Check if the API is responding on the given port and exit
  --help               Show this help message

Environment Variables:
  PORT                 Web service port (default: 3000)
  GTOPT_BIN            Path to gtopt binary
  GTOPT_DATA_DIR       Directory for job data storage
  GTOPT_LOG_DIR        Directory for log files

Examples:
  gtopt_websrv                           # Start on default port 3000
  gtopt_websrv --port 8080               # Start on port 8080
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
    const req = http.get(url, { timeout }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try {
          resolve({ status: res.statusCode, body: JSON.parse(data) });
        } catch (_) {
          resolve({ status: res.statusCode, body: null });
        }
      });
    });
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
  });
}

async function verifyApi(port, logDir, timeout) {
  const maxWait = timeout || 30;
  const baseUrl = `http://localhost:${port}`;
  const startTime = Date.now();

  logMessage('Verifying API endpoints...', logDir);

  // Poll until the API responds or timeout
  let apiResult = null;
  while ((Date.now() - startTime) / 1000 < maxWait) {
    apiResult = await httpGet(`${baseUrl}/api`, 5000);
    if (apiResult && apiResult.status === 200 && apiResult.body && apiResult.body.status === 'ok') {
      break;
    }
    apiResult = null;
    await new Promise((r) => setTimeout(r, 1000));
  }

  if (apiResult) {
    logMessage(`API verification PASSED: GET /api returned status "ok"`, logDir);
  } else {
    logMessage(`API verification FAILED: GET /api did not respond within ${maxWait}s`, logDir);
    return false;
  }

  // Also check /api/ping
  const pingResult = await httpGet(`${baseUrl}/api/ping`, 5000);
  if (pingResult && pingResult.status === 200 && pingResult.body && pingResult.body.status === 'ok') {
    const info = pingResult.body;
    logMessage(`API verification PASSED: GET /api/ping returned status "${info.status}", service="${info.service || ''}"`, logDir);
    if (info.gtopt_version) {
      logMessage(`  gtopt version: ${info.gtopt_version}`, logDir);
    }
    if (info.gtopt_bin) {
      logMessage(`  gtopt binary: ${info.gtopt_bin}`, logDir);
    }
  } else {
    logMessage(`API verification WARNING: GET /api/ping did not return expected response`, logDir);
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
  
  // Set up environment
  const env = {
    ...process.env,
    PORT: config.port,
  };
  
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
  
  // Start the web service
  const npmScript = config.dev ? 'dev' : 'start';
  const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  
  console.log('');
  console.log('='.repeat(60));
  console.log(`gtopt web service is starting (${config.dev ? 'development' : 'production'} mode)`);
  console.log(`URL: http://localhost:${config.port}`);
  console.log('Press Ctrl+C to stop');
  console.log('='.repeat(60));
  console.log('');
  
  const proc = spawn(npm, ['run', npmScript], {
    cwd: webserviceDir,
    env: env,
    stdio: 'inherit',
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

  // Verify API is working after server starts
  verifyApi(config.port, config.logDir, 30).then((ok) => {
    if (!ok) {
      console.error('Warning: API verification failed. The service may not be working correctly.');
    }
  });
}

if (require.main === module) {
  main();
}
