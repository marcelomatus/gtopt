import { NextRequest, NextResponse } from "next/server";
import { getJob } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/token");

export const dynamic = "force-dynamic";

// GET /api/jobs/[token] - Get job status
export async function GET(
  _request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const startTime = Date.now();
  const { token } = await params;
  log.info(`GET /api/jobs/${token}: looking up job`);
  const job = await getJob(token);

  if (!job) {
    log.warn(`GET /api/jobs/${token}: job not found (${Date.now() - startTime}ms)`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  log.info(`GET /api/jobs/${token}: status=${job.status} (${Date.now() - startTime}ms)`);
  return NextResponse.json(job);
}
