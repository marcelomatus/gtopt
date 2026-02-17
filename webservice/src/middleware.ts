import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";

/**
 * Middleware that logs every incoming request for diagnostics.
 * Helps pinpoint routing issues (e.g. API routes returning 404).
 */
export function middleware(request: NextRequest) {
  const start = Date.now();
  const response = NextResponse.next();

  // Log request path, method, and user-agent on every request.
  // In production this is written to the server log / GTOPT_LOG_DIR file.
  const method = request.method;
  const path = request.nextUrl.pathname;
  const ua = request.headers.get("user-agent") || "-";
  const shortUa = ua.length > 60 ? ua.slice(0, 60) + "…" : ua;

  console.log(
    `[${new Date().toISOString()}] [INFO] [middleware] ${method} ${path} (ua=${shortUa})`
  );

  return response;
}

// Run the middleware on API routes and the landing page.
// Excludes static assets (_next/static, favicon, etc.) to reduce noise.
export const config = {
  matcher: ["/", "/api/:path*"],
};
