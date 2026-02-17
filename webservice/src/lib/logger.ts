import { promises as fs, existsSync, readFileSync } from "fs";
import path from "path";

export type LogLevel = "info" | "warn" | "error" | "debug";

const LOG_DIR = process.env.GTOPT_LOG_DIR || "";

function timestamp(): string {
  return new Date().toISOString();
}

function formatMessage(level: LogLevel, component: string, msg: string): string {
  return `[${timestamp()}] [${level.toUpperCase()}] [${component}] ${msg}`;
}

async function appendToFile(filePath: string, line: string): Promise<void> {
  try {
    await fs.mkdir(path.dirname(filePath), { recursive: true });
    await fs.appendFile(filePath, line + "\n");
  } catch (err) {
    console.error(`Failed to write to log file ${filePath}: ${err}`);
  }
}

function writeLog(level: LogLevel, component: string, msg: string): void {
  const formatted = formatMessage(level, component, msg);

  // Always write to console
  switch (level) {
    case "error":
      console.error(formatted);
      break;
    case "warn":
      console.warn(formatted);
      break;
    default:
      console.log(formatted);
      break;
  }

  // If a log directory is configured, also write to a file
  if (LOG_DIR) {
    const logFile = path.join(LOG_DIR, "gtopt-webservice.log");
    appendToFile(logFile, formatted);
  }
}

export function createLogger(component: string) {
  return {
    info: (msg: string) => writeLog("info", component, msg),
    warn: (msg: string) => writeLog("warn", component, msg),
    error: (msg: string) => writeLog("error", component, msg),
    debug: (msg: string) => writeLog("debug", component, msg),
  };
}

// Log startup environment once when module is first loaded
const startupLog = createLogger("startup");
startupLog.info(`gtopt-webservice initializing`);
startupLog.info(`  NODE_ENV=${process.env.NODE_ENV || "(not set)"}`);
startupLog.info(`  GTOPT_BIN=${process.env.GTOPT_BIN || "(not set)"}`);
startupLog.info(`  GTOPT_DATA_DIR=${process.env.GTOPT_DATA_DIR || "(not set)"}`);
startupLog.info(`  GTOPT_LOG_DIR=${LOG_DIR || "(not set — logging to console only)"}`);
startupLog.info(`  PORT=${process.env.PORT || "(not set)"}`);
startupLog.info(`  GTOPT_HOSTNAME=${process.env.GTOPT_HOSTNAME || "(not set)"}`);
startupLog.info(`  HOSTNAME=${process.env.HOSTNAME || "(not set)"}`);
startupLog.info(`  cwd=${process.cwd()}`);
startupLog.info(`  platform=${process.platform}, arch=${process.arch}, node=${process.version}`);
// Detect WSL environment for diagnostic purposes
const isWSL = !!(process.env.WSL_DISTRO_NAME || process.env.WSLENV);
if (isWSL) {
  startupLog.info(`  WSL detected: distro=${process.env.WSL_DISTRO_NAME || "unknown"}`);
}
startupLog.info(`  log file: ${LOG_DIR ? path.join(LOG_DIR, "gtopt-webservice.log") : "(console only)"}`);

// Log Next.js build state for diagnosing route loading issues (e.g. API 404)
try {
  const nextDir = path.join(process.cwd(), ".next");
  if (existsSync(nextDir)) {
    const buildIdPath = path.join(nextDir, "BUILD_ID");
    const buildId = existsSync(buildIdPath) ? readFileSync(buildIdPath, "utf-8").trim() : "(missing)";
    startupLog.info(`  .next/BUILD_ID=${buildId}`);

    // Log registered app routes from the manifest
    const manifestPath = path.join(nextDir, "server", "app-paths-manifest.json");
    if (existsSync(manifestPath)) {
      const manifest = JSON.parse(readFileSync(manifestPath, "utf-8"));
      const routes = Object.keys(manifest);
      startupLog.info(`  app-paths-manifest: ${routes.length} route(s): ${routes.join(", ")}`);
    } else {
      startupLog.warn(`  app-paths-manifest.json NOT FOUND — API routes will return 404`);
    }

    // Log config file type to help diagnose config loading issues
    const configFiles = ["next.config.mjs", "next.config.js", "next.config.ts", "next.config.cjs"];
    const found = configFiles.filter((f) => existsSync(path.join(process.cwd(), f)));
    startupLog.info(`  next config file(s): ${found.length > 0 ? found.join(", ") : "(none found)"}`);
  } else {
    startupLog.warn(`  .next directory NOT FOUND in ${process.cwd()} — run 'npm run build' first`);
  }
} catch (err) {
  startupLog.warn(`  build diagnostics failed: ${err}`);
}

/**
 * Return the path to the log file, or empty string if file logging is disabled.
 */
export function getLogFilePath(): string {
  if (!LOG_DIR) return "";
  return path.join(LOG_DIR, "gtopt-webservice.log");
}

/**
 * Read the last N lines from the log file.
 */
export async function readLogTail(lines: number = 200): Promise<string[]> {
  const logFile = getLogFilePath();
  if (!logFile) return [];
  try {
    const content = await fs.readFile(logFile, "utf-8");
    const allLines = content.split("\n").filter((l) => l.length > 0);
    return allLines.slice(-lines);
  } catch {
    return [];
  }
}
