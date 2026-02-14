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
 *     --help               Show this help message
 * 
 * Environment Variables:
 *     PORT                 Web service port (default: 3000)
 *     GTOPT_BIN            Path to gtopt binary
 *     GTOPT_DATA_DIR       Directory for job data storage
 */

const fs = require('fs');
const path = require('path');
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
  --dev                Run in development mode
  --help               Show this help message

Environment Variables:
  PORT                 Web service port (default: 3000)
  GTOPT_BIN            Path to gtopt binary
  GTOPT_DATA_DIR       Directory for job data storage

Examples:
  gtopt_websrv                           # Start on default port 3000
  gtopt_websrv --port 8080               # Start on port 8080
  gtopt_websrv --gtopt-bin /usr/bin/gtopt  # Use specific gtopt binary
  gtopt_websrv --dev                     # Run in development mode
`);
}

function getWebserviceDir() {
  // This script is in bin/, webservice is in ../share/gtopt/webservice/
  const scriptDir = path.dirname(__dirname);
  
  // Check common installation locations
  const possibleLocations = [
    path.join(scriptDir, 'share', 'gtopt', 'webservice'),
    path.join(scriptDir, '..', 'share', 'gtopt', 'webservice'),
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
    dev: false,
    help: false,
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
      case '--dev':
        config.dev = true;
        break;
      default:
        console.error(`Unknown option: ${args[i]}`);
        process.exit(1);
    }
  }
  
  return config;
}

function main() {
  const config = parseArgs();
  
  if (config.help) {
    showHelp();
    process.exit(0);
  }
  
  // Find webservice directory
  const webserviceDir = getWebserviceDir();
  console.log(`Using webservice from: ${webserviceDir}`);
  
  // Find or verify gtopt binary
  let gtoptBin = config.gtoptBin;
  if (!gtoptBin) {
    gtoptBin = findGtoptBinary();
    if (!gtoptBin) {
      console.error('Error: gtopt binary not found');
      console.error('');
      console.error('Please specify the gtopt binary location:');
      console.error('  gtopt_websrv --gtopt-bin /path/to/gtopt');
      console.error('  or set GTOPT_BIN environment variable');
      process.exit(1);
    }
  }
  
  // Verify gtopt binary exists
  if (!fs.existsSync(gtoptBin)) {
    console.error(`Error: gtopt binary not found at: ${gtoptBin}`);
    process.exit(1);
  }
  
  console.log(`Using gtopt binary: ${gtoptBin}`);
  console.log(`Starting web service on port ${config.port}...`);
  
  // Set up environment
  const env = {
    ...process.env,
    PORT: config.port,
    GTOPT_BIN: gtoptBin,
  };
  
  if (config.dataDir) {
    env.GTOPT_DATA_DIR = config.dataDir;
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
}

if (require.main === module) {
  main();
}
