import { NextRequest, NextResponse } from "next/server";
import { createLogger, getLogFilePath, readLogTail } from "@/lib/logger";

const log = createLogger("api/logs");

export const dynamic = "force-dynamic";

// GET /api/logs - Retrieve webservice log contents
export async function GET(request: NextRequest) {
  const startTime = Date.now();
  const { searchParams } = new URL(request.url);
  const linesParam = searchParams.get("lines");
  const lines = Math.max(1, Math.min(parseInt(linesParam || "200", 10) || 200, 5000));

  log.info(`GET /api/logs called (lines=${lines})`);
  try {
    const logFile = getLogFilePath();
    const logLines = await readLogTail(lines);
    log.info(`GET /api/logs: returning ${logLines.length} log lines from ${logFile || "(console only)"} in ${Date.now() - startTime}ms`);
    return NextResponse.json({
      log_file: logFile,
      lines: logLines,
    });
  } catch (err) {
    log.error(`GET /api/logs error: ${err}`);
    return NextResponse.json(
      { error: "Failed to read logs" },
      { status: 500 }
    );
  }
}
