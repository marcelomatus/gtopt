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

    log.info(`POST /api/jobs: received file=${file.name} systemFile=${systemFile}`);

    // Create job
    const job = await createJob(systemFile);
    const inputDir = getJobInputDir(job.token);
    await fs.mkdir(inputDir, { recursive: true });

    // Save uploaded file
    const buffer = Buffer.from(await file.arrayBuffer());
    const zipPath = path.join(inputDir, "upload.zip");
    await fs.writeFile(zipPath, buffer);

    // Extract zip
    try {
      await extractZip(zipPath, { dir: inputDir });
    } catch (err) {
      log.error(`POST /api/jobs: zip extraction failed: ${err instanceof Error ? err.message : String(err)}`);
      return NextResponse.json(
        {
          error: `Failed to extract zip file: ${err instanceof Error ? err.message : String(err)}`,
        },
        { status: 400 }
      );
    }

    // Clean up zip file after extraction
    await fs.unlink(zipPath);

    // Verify system file exists
    const systemFilePath = path.join(inputDir, systemFile);
    try {
      await fs.access(systemFilePath);
    } catch {
      log.error(`POST /api/jobs: system file '${systemFile}' not found in archive`);
      return NextResponse.json(
        {
          error: `System file '${systemFile}' not found in uploaded archive.`,
        },
        { status: 400 }
      );
    }

    // Start the job asynchronously
    runGtopt(job.token).catch((err) => {
      log.error(`Job ${job.token} failed: ${err}`);
    });

    log.info(`POST /api/jobs: job submitted token=${job.token}`);
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
