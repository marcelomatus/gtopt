import { NextRequest, NextResponse } from "next/server";
import { promises as fs } from "fs";
import path from "path";
import {
  createJob,
  ensureDataDir,
  getJobInputDir,
  runGtopt,
  listJobs,
} from "@/lib/jobs";
import { createLogger } from "@/lib/logger";
import extractZip from "extract-zip";

const log = createLogger("api/jobs");

export const dynamic = "force-dynamic";

/**
 * Recursively list all files under a directory, returning paths relative to
 * the base directory.
 */
async function listDirRecursive(dir: string, base?: string): Promise<string[]> {
  const root = base ?? dir;
  const entries = await fs.readdir(dir, { withFileTypes: true });
  const files: string[] = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      files.push(...(await listDirRecursive(full, root)));
    } else {
      files.push(path.relative(root, full));
    }
  }
  return files;
}

// POST /api/jobs - Upload a case zip and start a job
export async function POST(request: NextRequest) {
  try {
    await ensureDataDir();

    const formData = await request.formData();
    const file = formData.get("file") as File | null;
    const systemFile = formData.get("systemFile") as string | null;

    if (!file) {
      log.warn("POST /api/jobs: no file uploaded");
      return NextResponse.json(
        { error: "No file uploaded. Please upload a zip file." },
        { status: 400 }
      );
    }

    if (!systemFile) {
      log.warn("POST /api/jobs: no systemFile specified");
      return NextResponse.json(
        {
          error:
            "No systemFile specified. Please provide the system JSON filename.",
        },
        { status: 400 }
      );
    }

    // Validate file type
    if (
      !file.name.endsWith(".zip")
    ) {
      log.warn(`POST /api/jobs: invalid file type: ${file.name}`);
      return NextResponse.json(
        {
          error:
            "Invalid file type. Please upload a .zip file.",
        },
        { status: 400 }
      );
    }

    log.info(`POST /api/jobs: received file=${file.name} (${file.size} bytes) systemFile=${systemFile}`);

    // Create job
    const job = await createJob(systemFile);
    const inputDir = getJobInputDir(job.token);
    await fs.mkdir(inputDir, { recursive: true });
    log.info(`Job ${job.token}: created dedicated job directory inputDir=${inputDir}`);

    // Save uploaded file
    const buffer = Buffer.from(await file.arrayBuffer());
    const zipPath = path.join(inputDir, "upload.zip");
    await fs.writeFile(zipPath, buffer);
    log.info(`Job ${job.token}: saved uploaded zip (${buffer.length} bytes) to ${zipPath}`);

    // Extract zip into the dedicated job input directory
    try {
      await extractZip(zipPath, { dir: inputDir });
      log.info(`Job ${job.token}: zip extracted successfully to ${inputDir}`);
    } catch (err) {
      log.error(`Job ${job.token}: zip extraction failed in ${inputDir}: ${err instanceof Error ? err.message : String(err)}`);
      return NextResponse.json(
        {
          error: `Failed to extract zip file: ${err instanceof Error ? err.message : String(err)}`,
        },
        { status: 400 }
      );
    }

    // Clean up zip file after extraction
    await fs.unlink(zipPath);

    // Log the contents of the extracted directory
    try {
      const extractedFiles = await listDirRecursive(inputDir);
      log.info(`Job ${job.token}: extracted ${extractedFiles.length} file(s) in ${inputDir}:`);
      for (const f of extractedFiles) {
        log.info(`Job ${job.token}:   ${f}`);
      }
    } catch {
      // Non-critical: proceed even if listing fails
    }

    // Verify system file exists
    const systemFilePath = path.join(inputDir, systemFile);
    try {
      await fs.access(systemFilePath);
      log.info(`Job ${job.token}: system file '${systemFile}' found at ${systemFilePath}`);
    } catch {
      log.error(`Job ${job.token}: system file '${systemFile}' not found in archive at ${systemFilePath}`);
      return NextResponse.json(
        {
          error: `System file '${systemFile}' not found in uploaded archive.`,
        },
        { status: 400 }
      );
    }

    // Start the job asynchronously
    log.info(`Job ${job.token}: starting gtopt asynchronously, cwd=${inputDir}`);
    runGtopt(job.token).catch((err) => {
      log.error(`Job ${job.token} failed: ${err}`);
    });

    log.info(`Job ${job.token}: job submitted successfully`);
    return NextResponse.json(
      {
        token: job.token,
        status: job.status,
        message:
          "Job submitted successfully. Use the token to check status and download results.",
      },
      { status: 201 }
    );
  } catch (err) {
    log.error(`POST /api/jobs: internal error: ${err}`);
    return NextResponse.json(
      { error: "Internal server error" },
      { status: 500 }
    );
  }
}

// GET /api/jobs - List all jobs
export async function GET() {
  try {
    log.info("GET /api/jobs: listing jobs");
    const allJobs = await listJobs();
    return NextResponse.json({ jobs: allJobs });
  } catch (err) {
    log.error(`GET /api/jobs: internal error: ${err}`);
    return NextResponse.json(
      { error: "Internal server error" },
      { status: 500 }
    );
  }
}
