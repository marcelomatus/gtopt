import { promises as fs } from "fs";
import path from "path";
import { v4 as uuidv4 } from "uuid";
import { spawn, execFile } from "child_process";
import { createLogger } from "./logger";

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
  log.info(`Job ${token}: starting gtopt binary=${gtoptBin} systemFile=${job.systemFile} inputDir=${inputDir} outputDir=${outputDir}`);

  return new Promise<void>((resolve) => {
    const proc = spawn(
      gtoptBin,
      [job.systemFile, "--output-directory", outputDir],
      {
        cwd: inputDir,
        stdio: ["ignore", "pipe", "pipe"],
      }
    );

    let stdout = "";
    let stderr = "";

    proc.stdout.on("data", (data: Buffer) => {
      stdout += data.toString();
    });

    proc.stderr.on("data", (data: Buffer) => {
      stderr += data.toString();
    });

    proc.on("close", async (code) => {
      if (code === 0) {
        job.status = "completed";
        job.completedAt = new Date().toISOString();
        log.info(`Job ${token}: gtopt completed successfully`);
      } else {
        job.status = "failed";
        job.error = stderr || stdout || `Process exited with code ${code}`;
        log.error(`Job ${token}: gtopt failed with exit code ${code}: ${job.error}`);
      }
      // Save logs
      try {
        await fs.writeFile(path.join(getJobDir(token), "stdout.log"), stdout);
        await fs.writeFile(path.join(getJobDir(token), "stderr.log"), stderr);
      } catch {
        // Ignore log write errors
      }
      await updateJob(job);
      resolve();
    });

    proc.on("error", async (err) => {
      job.status = "failed";
      job.error = `Failed to start gtopt: ${err.message}`;
      log.error(`Job ${token}: failed to start gtopt: ${err.message}`);
      await updateJob(job);
      resolve();
    });
  });
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

  // Check common install locations
  const candidates = [
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
