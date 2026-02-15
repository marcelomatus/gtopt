import { promises as fs } from "fs";
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
