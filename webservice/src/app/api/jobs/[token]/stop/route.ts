import { NextRequest, NextResponse } from "next/server";
import { getJob, softStopJob, stopJob } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/token/stop");

export const dynamic = "force-dynamic";

// POST /api/jobs/[token]/stop - Stop a running job
//
// Query parameters:
//   mode=soft  (default) — Write the monitoring API stop-request file
//              (sddp_stop_request.json) in the job output directory.  The SDDP
//              solver checks both the legacy sentinel file and this file at the
//              start of each iteration; when found it finishes the iteration,
//              saves all accumulated Benders cuts, and exits normally.
//              For the monolithic solver use mode=force instead.
//   mode=force — Send SIGTERM to the process immediately (hard stop).
export async function POST(
  request: NextRequest,
  { params }: { params: Promise<{ token: string }> }
) {
  const { token } = await params;
  const { searchParams } = request.nextUrl;
  const mode = searchParams.get("mode") ?? "soft";

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

  if (mode === "force") {
    // Hard stop: send SIGTERM immediately
    const signaled = await stopJob(token);
    log.info(`POST /api/jobs/${token}/stop(force): signaled=${signaled}`);
    return NextResponse.json({
      mode: "force",
      message: signaled
        ? "SIGTERM sent to job process"
        : "Job process not found (may have already exited)",
      stopped: signaled,
    });
  }

  if (mode !== "soft") {
    return NextResponse.json(
      { error: `Invalid mode "${mode}". Accepted values: "soft", "force"` },
      { status: 400 }
    );
  }

  // Soft stop: write the monitoring API stop-request file.
  // The SDDP solver checks this file (and the legacy sentinel) at the start of
  // each iteration.  When found it completes the iteration, saves cuts, and exits.
  const requestWritten = await softStopJob(token);
  log.info(
    `POST /api/jobs/${token}/stop(soft): requestWritten=${requestWritten}`
  );
  return NextResponse.json({
    mode: "soft",
    message: requestWritten
      ? "Stop request written — solver will stop gracefully after the current iteration"
      : "Could not write stop-request file (output directory not ready yet)",
    stopped: requestWritten,
  });
}
