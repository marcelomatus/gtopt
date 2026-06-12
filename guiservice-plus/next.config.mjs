/** @type {import('next').NextConfig} */
const nextConfig = {
  // Reverse proxy to the Flask guiservice running on port 5001 by default.
  // The target can be overridden at runtime via GTOPT_GUISERVICE_URL.
  async rewrites() {
    const target = process.env.GTOPT_GUISERVICE_URL || 'http://localhost:5001';
    return [
      {
        source: '/flask/:path*',
        destination: `${target}/:path*`,
      },
    ];
  },
  reactStrictMode: true,
};

export default nextConfig;
