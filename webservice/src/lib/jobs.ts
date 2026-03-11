import { promises as fs } from "fs";
import path from "path";
import { v4 as uuidv4 } from "uuid";
import { spawn, execFile, ChildProcess } from "child_process";
import { createLogger } from "./logger";
import { listDirRecursive } from "./files";

const log = createLogger("jobs");

export interface JobInfo {
  token: string;
  status: "pending" | "running" | "completed" | "failed";
  createdAt: string;
  completedAt?: string;
  systemFile: string;
  error?: string;
}

const DATA_DIR = process.env.GTOPT_DATA_DIR || path.join(process.cwd(), "data");
const GTOPT_BIN =
  process.env.GTOPT_BIN || path.join(process.cwd(), "..", "build", "gtopt");

// In-memory job store (for production, use a database)
const jobs = new Map<string, JobInfo>();

// In-memory map of running child processes keyed by job token
const runningProcesses = new Map<string, ChildProcess>();

export function getDataDir(): string {
  return DATA_DIR;
}

export function getJobDir(token: string): string {
  return path.join(DATA_DIR, "jobs", token);
}

export function getJobInputDir(token: string): string {
  return path.join(getJobDir(token), "input");
}

export function getJobOutputDir(token: string): string {
  return path.join(getJobDir(token), "output");
}

export async function ensureDataDir(): Promise<void> {
  await fs.mkdir(path.join(DATA_DIR, "jobs"), { recursive: true });
}

export async function createJob(systemFile: string): Promise<JobInfo> {
  if (!systemFile || systemFile.trim() === "") {
    throw new Error("systemFile parameter must not be empty");
  }
  const token = uuidv4();
  const job: JobInfo = {
    token,
    status: "pending",
    createdAt: new Date().toISOString(),
    systemFile,
  };
  jobs.set(token, job);

  // Persist job info to disk
  const jobDir = getJobDir(token);
  await fs.mkdir(jobDir, { recursive: true });
  await fs.writeFile(
    path.join(jobDir, "job.json"),
    JSON.stringify(job, null, 2)
  );

  log.info(`Job created: token=${token} systemFile=${systemFile}`);
  return job;
}

export async function getJob(token: string): Promise<JobInfo | null> {
  // Check in-memory first
  if (jobs.has(token)) {
    return jobs.get(token)!;
  }

  // Try loading from disk
  const jobFile = path.join(getJobDir(token), "job.json");
  try {
    const data = await fs.readFile(jobFile, "utf-8");
    const job = JSON.parse(data) as JobInfo;
    jobs.set(token, job);
    return job;
  } catch {
    return null;
  }
}

async function updateJob(job: JobInfo): Promise<void> {
  jobs.set(job.token, job);
  const jobDir = getJobDir(job.token);
  await fs.writeFile(
    path.join(jobDir, "job.json"),
    JSON.stringify(job, null, 2)
  );
}

export async function runGtopt(token: string): Promise<void> {
  const job = await getJob(token);
  if (!job) return;

  job.status = "running";
  await updateJob(job);

  const inputDir = getJobInputDir(token);
  const outputDir = getJobOutputDir(token);

  await fs.mkdir(outputDir, { recursive: true });

  const gtoptBin = await resolveGtoptBinary();
  log.info(`Job ${token}: starting gtopt binary=${gtoptBin} systemFile=${job.systemFile}`);
  log.info(`Job ${token}: working directory (cwd)=${inputDir}`);
  log.info(`Job ${token}: output directory=${outputDir}`);

  // Log input directory contents before execution
  try {
    const inputFiles = await listDirRecursive(inputDir);
    log.info(`Job ${token}: input directory contains ${inputFiles.length} file(s):`);
    for (const f of inputFiles) {
      log.info(`Job ${token}:   input: ${f}`);
    }
  } catch (err) {
    log.warn(`Job ${token}: could not list input directory contents: ${err}`);
  }

  return new Promise<void>((resolve) => {
    const proc = spawn(
      gtoptBin,
      [job.systemFile, "--output-directory", outputDir],
      {
        cwd: inputDir,
        stdio: ["ignore", "pipe", "pipe"],
      }
    );

    // Register process so it can be stopped via stopJob()
    runningProcesses.set(token, proc);
    log.info(`Job ${token}: process started pid=${proc.pid ?? "unknown"}`);

    let stdout = "";
    let stderr = "";

    proc.stdout.on("data", (data: Buffer) => {
      const text = data.toString();
      stdout += text;
      // Log each line of terminal output for real-time visibility
      for (const line of text.split("\n").filter((l: string) => l.length > 0)) {
        log.info(`Job ${token} [stdout]: ${line}`);
      }
    });

    proc.stderr.on("data", (data: Buffer) => {
      const text = data.toString();
      stderr += text;
      for (const line of text.split("\n").filter((l: string) => l.length > 0)) {
        log.warn(`Job ${token} [stderr]: ${line}`);
      }
    });

    proc.on("close", async (code) => {
      // Unregister the process when it exits
      runningProcesses.delete(token);

      if (code === 0) {
        job.status = "completed";
        job.completedAt = new Date().toISOString();
        log.info(`Job ${token}: gtopt completed successfully (exit code 0)`);
      } else {
        job.status = "failed";
        job.error = stderr || stdout || `Process exited with code ${code}`;
        log.error(`Job ${token}: gtopt failed with exit code ${code}: ${job.error}`);
      }

      // Log output directory contents after execution
      try {
        const outputFiles = await listDirRecursive(outputDir);
        log.info(`Job ${token}: output directory contains ${outputFiles.length} file(s):`);
        for (const f of outputFiles) {
          log.info(`Job ${token}:   output: ${f}`);
        }
      } catch (err) {
        log.warn(`Job ${token}: could not list output directory contents: ${err}`);
      }

      // Save logs to job directory
      try {
        const jobDir = getJobDir(token);
        await fs.writeFile(path.join(jobDir, "stdout.log"), stdout);
        await fs.writeFile(path.join(jobDir, "stderr.log"), stderr);
        log.info(`Job ${token}: saved stdout.log (${stdout.length} bytes) and stderr.log (${stderr.length} bytes)`);
      } catch {
        log.warn(`Job ${token}: failed to save log files`);
      }
      // Also save terminal output inside the output directory so it is
      // included in the results ZIP downloaded by the GUI.
      try {
        await fs.writeFile(path.join(outputDir, "gtopt_terminal.log"), stdout + stderr);
      } catch {
        // Ignore write errors
      }
      await updateJob(job);
      resolve();
    });

    proc.on("error", async (err) => {
      runningProcesses.delete(token);
      job.status = "failed";
      job.error = `Failed to start gtopt: ${err.message}`;
      log.error(`Job ${token}: failed to start gtopt: ${err.message}`);
      await updateJob(job);
      resolve();
    });
  });
}

/**
 * The sentinel file name that the SDDP solver polls for graceful stop.
 * Must match `sddp_file::stop_sentinel` in include/gtopt/sddp_solver.hpp.
 */
export const SDDP_STOP_SENTINEL = "sddp_stop";

/**
 * Soft-stop a running SDDP job by creating the sentinel file in the job's
 * output directory.  The SDDP solver checks for this file at the start of
 * each iteration; when found it finishes the iteration, saves all accumulated
 * Benders cuts, and returns normally.
 *
 * Returns true if the sentinel file was written, false if the output directory
 * does not exist yet (job may not have started).
 */
export async function softStopJob(token: string): Promise<boolean> {
  const outputDir = getJobOutputDir(token);
  const sentinelPath = path.join(outputDir, SDDP_STOP_SENTINEL);
  try {
    // Ensure the output directory exists before writing
    await fs.mkdir(outputDir, { recursive: true });
    await fs.writeFile(sentinelPath, `stop requested at ${new Date().toISOString()}\n`);
    // Note: the SDDP solver only checks file existence, not content.
    // The timestamp string is written purely as a human-readable marker for debugging.
    log.info(`Job ${token}: created SDDP sentinel file at ${sentinelPath}`);
    return true;
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    log.warn(`Job ${token}: could not create sentinel file ${sentinelPath}: ${msg}`);
    return false;
  }
}

/**
 * Stop a running job by sending SIGTERM to its child process (hard stop).
 * Returns true if a signal was sent, false if the job was not running.
 */
export async function stopJob(token: string): Promise<boolean> {
  const proc = runningProcesses.get(token);
  if (!proc) {
    log.info(`Job ${token}: stop requested but no running process found`);
    return false;
  }
  log.info(`Job ${token}: sending SIGTERM to process pid=${proc.pid}`);
  proc.kill("SIGTERM");
  return true;
}

/**
 * Read the solver monitor status JSON file for a job.
 * Checks for sddp_status.json first, then monolithic_status.json.
 * Returns the parsed JSON object or null if not found.
 */
export async function getJobMonitorData(
  token: string
): Promise<Record<string, unknown> | null> {
  const outputDir = getJobOutputDir(token);
  const candidates = [
    path.join(outputDir, "sddp_status.json"),
    path.join(outputDir, "monolithic_status.json"),
  ];
  for (const candidate of candidates) {
    try {
      const raw = await fs.readFile(candidate, "utf-8");
      const data = JSON.parse(raw) as Record<string, unknown>;
      // Annotate with the solver type derived from the filename
      data._solver_type = path.basename(candidate).startsWith("sddp")
        ? "sddp"
        : "monolithic";
      return data;
    } catch (e) {
      // File not present or not valid JSON yet — try next candidate
      const msg = e instanceof Error ? e.message : String(e);
      log.warn(`getJobMonitorData: could not parse ${candidate}: ${msg}`);
    }
  }
  return null;
}

export async function resolveGtoptBinary(): Promise<string> {
  // Check configured path first
  try {
    await fs.access(GTOPT_BIN, fs.constants.X_OK);
    log.info(`Using configured gtopt binary: ${GTOPT_BIN}`);
    return GTOPT_BIN;
  } catch {
    // Fall through
  }

  // Check common install locations — user-local installation first so that a
  // non-root installation in ~/.local/bin takes precedence over a system-wide
  // one in /usr/local/bin.
  const candidates = [
    path.join(process.env.HOME || "", ".local", "bin", "gtopt"),
    "/usr/local/bin/gtopt",
    "/usr/bin/gtopt",
    path.join(process.cwd(), "..", "build", "standalone", "gtopt"),
  ];

  for (const candidate of candidates) {
    try {
      await fs.access(candidate, fs.constants.X_OK);
      log.info(`Found gtopt binary at: ${candidate}`);
      return candidate;
    } catch {
      continue;
    }
  }

  // Default: hope it's on PATH
  log.warn("gtopt binary not found in configured paths, falling back to PATH lookup");
  return "gtopt";
}

export async function listJobs(): Promise<JobInfo[]> {
  const jobsDir = path.join(DATA_DIR, "jobs");
  try {
    const entries = await fs.readdir(jobsDir, { withFileTypes: true });
    const result: JobInfo[] = [];
    for (const entry of entries) {
      if (entry.isDirectory()) {
        const job = await getJob(entry.name);
        if (job) result.push(job);
      }
    }
    return result.sort(
      (a, b) =>
        new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime()
    );
  } catch {
    return [];
  }
}

/**
 * Read saved stdout/stderr logs for a job.
 */
export async function getJobLogs(
  token: string
): Promise<{ stdout: string; stderr: string } | null> {
  const jobDir = getJobDir(token);
  let stdout = "";
  let stderr = "";
  try {
    stdout = await fs.readFile(path.join(jobDir, "stdout.log"), "utf-8");
  } catch {
    // file may not exist yet
  }
  try {
    stderr = await fs.readFile(path.join(jobDir, "stderr.log"), "utf-8");
  } catch {
    // file may not exist yet
  }
  return { stdout, stderr };
}

/**
 * Query the gtopt binary for its version string.
 */
export async function getGtoptVersion(): Promise<string> {
  const gtoptBin = await resolveGtoptBinary();
  return new Promise<string>((resolve) => {
    execFile(gtoptBin, ["--version"], { timeout: 5000 }, (err, stdout, stderr) => {
      if (err) {
        log.warn(`Failed to get gtopt version: ${err.message}`);
        resolve("");
        return;
      }
      const output = (stdout || stderr || "").trim();
      resolve(output);
    });
  });
}
