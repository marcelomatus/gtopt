import { NextResponse } from "next/server";
import type { NextRequest } from "next/server";

/**
 * Proxy that logs every incoming request for diagnostics.
 * Helps pinpoint routing issues (e.g. API routes returning 404).
 *
 * Renamed from `middleware()` to `proxy()` per Next.js 16+ convention
 * (the `middleware` file/export name was deprecated — see
 * https://nextjs.org/docs/messages/middleware-to-proxy).
 */
export function proxy(request: NextRequest) {
  const response = NextResponse.next();

  // Log request path, method, and user-agent on every request.
  // In production this is written to the server log / GTOPT_LOG_DIR file.
  const method = request.method;
  const path = request.nextUrl.pathname;
  const ua = request.headers.get("user-agent") || "-";
  const shortUa = ua.length > 60 ? ua.slice(0, 60) + "…" : ua;

  console.log(
    `[${new Date().toISOString()}] [INFO] [proxy] ${method} ${path} (ua=${shortUa})`
  );

  return response;
}

// Run the proxy on API routes and the landing page.
// Excludes static assets (_next/static, favicon, etc.) to reduce noise.
export const config = {
  matcher: ["/", "/api/:path*"],
};
