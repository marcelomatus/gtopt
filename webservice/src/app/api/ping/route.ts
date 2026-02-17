import { NextResponse } from "next/server";
import { resolveGtoptBinary, getGtoptVersion } from "@/lib/jobs";
import { createLogger, getLogFilePath } from "@/lib/logger";
import { existsSync, readFileSync } from "fs";
import path from "path";

const log = createLogger("api/ping");

export const dynamic = "force-dynamic";

// GET /api/ping - Health check / version endpoint
export async function GET() {
  log.info("GET /api/ping called");
  const startTime = Date.now();
  let gtoptBin = "";
  let gtoptVersion = "";

  try {
    gtoptBin = await resolveGtoptBinary();
    log.info(`GET /api/ping: resolved gtopt binary: ${gtoptBin}`);
  } catch (err) {
    log.warn(`GET /api/ping: could not resolve gtopt binary: ${err}`);
  }

  try {
    gtoptVersion = await getGtoptVersion();
    log.info(`GET /api/ping: gtopt version: ${gtoptVersion || "(empty)"}`);
  } catch (err) {
    log.warn(`GET /api/ping: could not get gtopt version: ${err}`);
  }

  // Read build ID for diagnostics
  let buildId = "";
  try {
    const buildIdPath = path.join(process.cwd(), ".next", "BUILD_ID");
    if (existsSync(buildIdPath)) {
      buildId = readFileSync(buildIdPath, "utf-8").trim();
    }
  } catch {
    // ignore
  }

  const elapsed = Date.now() - startTime;
  log.info(`GET /api/ping completed in ${elapsed}ms (gtopt_bin=${gtoptBin || "none"}, gtopt_version=${gtoptVersion || "none"}, build_id=${buildId || "none"})`);

  // Always return status "ok" — the webservice itself is healthy.
  // The gtopt binary being absent is reported via empty gtopt_bin/gtopt_version
  // fields, not by marking the service as unhealthy.
  return NextResponse.json({
    status: "ok",
    service: "gtopt-webservice",
    gtopt_bin: gtoptBin,
    gtopt_version: gtoptVersion,
    log_file: getLogFilePath(),
    node_version: process.version,
    build_id: buildId,
    cwd: process.cwd(),
    timestamp: new Date().toISOString(),
  });
}
