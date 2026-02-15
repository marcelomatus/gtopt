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
  const { token } = await params;
  const job = await getJob(token);

  if (!job) {
    log.warn(`GET /api/jobs/${token}: job not found`);
    return NextResponse.json({ error: "Job not found" }, { status: 404 });
  }

  log.info(`GET /api/jobs/${token}: status=${job.status}`);
  return NextResponse.json(job);
}
