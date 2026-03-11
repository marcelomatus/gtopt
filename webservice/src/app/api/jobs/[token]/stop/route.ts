import { NextRequest, NextResponse } from "next/server";
import { getJob, stopJob } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/token/stop");

export const dynamic = "force-dynamic";

// POST /api/jobs/[token]/stop - Stop a running job
export async function POST(
  _request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const { token } = await params;

  const job = await getJob(token);
  if (!job) {
    log.warn(`POST /api/jobs/${token}/stop: job not found`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  if (job.status !== "running") {
    log.info(
      `POST /api/jobs/${token}/stop: job is not running (status=${job.status})`
    );
    return NextResponse.json(
      { message: `Job is not running (status: ${job.status})`, stopped: false },
      { status: 200 }
    );
  }

  const signaled = await stopJob(token);
  log.info(`POST /api/jobs/${token}/stop: signaled=${signaled}`);
  return NextResponse.json({
    message: signaled
      ? "Stop signal sent to job process"
      : "Job process not found (may have already exited)",
    stopped: signaled,
  });
}
