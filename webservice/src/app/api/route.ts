import { NextResponse } from "next/server";
import { createLogger } from "@/lib/logger";

const log = createLogger("api");

export const dynamic = "force-dynamic";

// GET /api - API index / health check
export async function GET() {
  log.info("GET /api called");
  return NextResponse.json({
    status: "ok",
    service: "gtopt-webservice",
    endpoints: [
      "GET  /api          - Service info and endpoint list",
      "GET  /api/ping     - Health check with version info",
      "POST /api/jobs     - Submit a new optimization job",
      "GET  /api/jobs     - List all jobs",
      "GET  /api/jobs/:id - Get job status",
      "GET  /api/jobs/:id/logs     - Get job logs",
      "GET  /api/jobs/:id/download - Download job results",
      "GET  /api/logs     - Retrieve service logs",
    ],
  });
}
