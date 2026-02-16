import { NextRequest, NextResponse } from "next/server";
import { getJob, getJobOutputDir, getJobDir } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";
import archiver from "archiver";
import { promises as fs } from "fs";
import { PassThrough } from "stream";

const log = createLogger("api/jobs/download");

export const dynamic = "force-dynamic";

// GET /api/jobs/[token]/download - Download job results as zip
export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const { token } = await params;
  const job = await getJob(token);

  if (!job) {
    log.warn(`GET /api/jobs/${token}/download: job not found`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  if (job.status !== "completed" && job.status !== "failed") {
    log.info(`GET /api/jobs/${token}/download: job not finished (status=${job.status})`);
    return NextResponse.json(
      {
        error: "Job is not finished yet",
        status: job.status,
      },
      { status: 409 }
    );
  }

  const outputDir = getJobOutputDir(token);
  const jobDir = getJobDir(token);
  log.info(`GET /api/jobs/${token}/download: preparing results, jobDir=${jobDir} outputDir=${outputDir}`);

  // Check if output directory exists
  try {
    await fs.access(outputDir);
  } catch {
    log.info(`GET /api/jobs/${token}/download: no output directory found, checking for job logs`);
    // If no output dir, include logs at minimum
    try {
      await fs.access(jobDir);
    } catch {
      log.warn(`GET /api/jobs/${token}/download: no output or job directory found`);
      return NextResponse.json(
        { error: "No output available" },
        { status: 404 }
      );
    }
  }

  // Create zip archive
  const archive = archiver("zip", { zlib: { level: 9 } });
  const passthrough = new PassThrough();

  archive.pipe(passthrough);

  // Add output directory if it exists
  try {
    await fs.access(outputDir);
    archive.directory(outputDir, "output");
    log.info(`GET /api/jobs/${token}/download: adding output/ directory to zip`);
  } catch {
    log.info(`GET /api/jobs/${token}/download: no output directory to include`);
  }

  // Add log files
  for (const logFile of ["stdout.log", "stderr.log", "job.json"]) {
    const logPath = `${jobDir}/${logFile}`;
    try {
      await fs.access(logPath);
      archive.file(logPath, { name: logFile });
      log.info(`GET /api/jobs/${token}/download: adding ${logFile} to zip`);
    } catch {
      // Log file doesn't exist
    }
  }

  await archive.finalize();

  // Collect the archive into a buffer
  const chunks: Buffer[] = [];
  for await (const chunk of passthrough) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  const zipBuffer = Buffer.concat(chunks);

  log.info(`GET /api/jobs/${token}/download: sending zip (${zipBuffer.length} bytes)`);
  return new NextResponse(zipBuffer, {
    status: 200,
    headers: {
      "Content-Type": "application/zip",
      "Content-Disposition": `attachment; filename="gtopt-results-${token}.zip"`,
      "Content-Length": String(zipBuffer.length),
    },
  });
}
