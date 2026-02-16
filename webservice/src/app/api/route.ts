import { NextResponse } from "next/server";

export const dynamic = "force-dynamic";

// GET /api - API index / health check
export async function GET() {
  return NextResponse.json({
    status: "ok",
    service: "gtopt-webservice",
    endpoints: [
      "GET  /api          - Service info and endpoint list",
      "GET  /api/ping     - Health check with version info",
      "POST /api/jobs     - Submit a new optimization job",
      "GET  /api/jobs     - List all jobs",
      "GET  /api/jobs/:id - Get job status",
      "GET  /api/logs     - Retrieve service logs",
    ],
  });
}
