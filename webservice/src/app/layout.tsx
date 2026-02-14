import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "gtopt Web Service",
  description: "Upload and run gtopt optimization cases",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
