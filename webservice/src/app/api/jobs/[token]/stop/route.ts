import { NextRequest, NextResponse } from "next/server";
import { getJob, softStopJob, stopJob } from "@/lib/jobs";
import { createLogger } from "@/lib/logger";

const log = createLogger("api/jobs/token/stop");

export const dynamic = "force-dynamic";

// POST /api/jobs/[token]/stop - Stop a running job
//
// Query parameters:
//   mode=soft  (default) — Create the SDDP sentinel file so the solver
//              finishes the current iteration and saves cuts before exiting.
//              For the monolithic solver this has no effect on its own;
//              use mode=force in that case.
//   mode=force — Send SIGTERM to the process immediately (hard stop).
//
// Both modes can be combined: POST /api/jobs/[token]/stop?mode=force will
// skip the sentinel and send SIGTERM right away.
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

  // Soft stop (default): create SDDP sentinel file
  const sentinelCreated = await softStopJob(token);
  log.info(
    `POST /api/jobs/${token}/stop(soft): sentinelCreated=${sentinelCreated}`
  );
  return NextResponse.json({
    mode: "soft",
    message: sentinelCreated
      ? "Sentinel file created — solver will stop gracefully after the current iteration"
      : "Could not create sentinel file (output directory not ready yet)",
    stopped: sentinelCreated,
  });
}
