import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** @type {import('next').NextConfig} */
const nextConfig = {
  // Pin the NFT (Node File Trace) root to the webservice directory so
  // Turbopack does not walk up into the sibling gtopt/ tree when it
  // sees a `path.join(process.cwd(), '..', …)` in jobs.ts.  Without
  // this, the build reports "Encountered unexpected file in NFT list"
  // because the dynamic-cwd path that resolves to the sibling build/
  // directory triggers a whole-project trace.
  outputFileTracingRoot: __dirname,
};

export default nextConfig;
