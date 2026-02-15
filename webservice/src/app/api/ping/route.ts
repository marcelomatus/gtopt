import { NextResponse } from "next/server";
import { resolveGtoptBinary, getGtoptVersion } from "@/lib/jobs";
import { createLogger, getLogFilePath } from "@/lib/logger";

const log = createLogger("api/ping");

export const dynamic = "force-dynamic";

// GET /api/ping - Health check / version endpoint
export async function GET() {
  log.info("GET /api/ping");
  try {
    const gtoptBin = await resolveGtoptBinary();
    const gtoptVersion = await getGtoptVersion();
    return NextResponse.json({
      status: "ok",
      service: "gtopt-webservice",
      gtopt_bin: gtoptBin,
      gtopt_version: gtoptVersion,
      log_file: getLogFilePath(),
      timestamp: new Date().toISOString(),
    });
  } catch (err) {
    log.error(`GET /api/ping error: ${err}`);
    return NextResponse.json({
      status: "error",
      service: "gtopt-webservice",
      gtopt_bin: "",
      gtopt_version: "",
      log_file: getLogFilePath(),
      timestamp: new Date().toISOString(),
    });
  }
}
