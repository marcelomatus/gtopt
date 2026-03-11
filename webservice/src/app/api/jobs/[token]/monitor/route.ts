import { NextRequest, NextResponse } from "next/server";
import { getJob, getJobMonitorData } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/token/monitor");

export const dynamic = "force-dynamic";

// GET /api/jobs/[token]/monitor - Read solver monitor status JSON
export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const { token } = await params;

  const job = await getJob(token);
  if (!job) {
    log.warn(`GET /api/jobs/${token}/monitor: job not found`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  const data = await getJobMonitorData(token);
  if (!data) {
    // Return a minimal "not-yet-available" response while the solver is warming up
    return NextResponse.json(
      { status: job.status, available: false },
      { status: 200 }
    );
  }

  log.info(
    `GET /api/jobs/${token}/monitor: status=${data["status"]} solver=${data["_solver_type"]}`
  );
  return NextResponse.json({ ...data, available: true, job_status: job.status });
}
