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
  const startTime = Date.now();
  const { token } = await params;
  log.info(`GET /api/jobs/${token}/logs: request received`);
  const job = await getJob(token);

  if (!job) {
    log.warn(`GET /api/jobs/${token}/logs: job not found (${Date.now() - startTime}ms)`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  const logs = await getJobLogs(token);

  const stdoutLen = logs?.stdout?.length ?? 0;
  const stderrLen = logs?.stderr?.length ?? 0;
  log.info(`GET /api/jobs/${token}/logs: returning logs (stdout=${stdoutLen} bytes, stderr=${stderrLen} bytes) in ${Date.now() - startTime}ms`);
  return NextResponse.json({
    token,
    status: job.status,
    stdout: logs?.stdout ?? "",
    stderr: logs?.stderr ?? "",
  });
}
