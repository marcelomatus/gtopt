import { NextRequest, NextResponse } from "next/server";
import { getJob, getJobLogs } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/logs");

export const dynamic = "force-dynamic";

// GET /api/jobs/[token]/logs - Retrieve stdout/stderr for a job
export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const { token } = await params;
  const job = await getJob(token);

  if (!job) {
    log.warn(`GET /api/jobs/${token}/logs: job not found`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  const logs = await getJobLogs(token);

  log.info(`GET /api/jobs/${token}/logs: returning logs`);
  return NextResponse.json({
    token,
    status: job.status,
    stdout: logs?.stdout ?? "",
    stderr: logs?.stderr ?? "",
  });
}
