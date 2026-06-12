import type { Metadata } from 'next';
import '@/styles/globals.css';
import { Toaster } from 'sonner';
import { ThemeProvider } from '@/components/layout/theme-provider';
import { QueryProvider } from '@/components/layout/query-provider';
import { AppShell } from '@/components/layout/app-shell';
import { AutoSaveWatcher } from '@/components/layout/auto-save-watcher';

export const metadata: Metadata = {
  title: 'gtopt – GUI Plus',
  description: 'Modern planning workbench for the gtopt LP/MIP solver',
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en" suppressHydrationWarning>
      <head>
        <link rel="preconnect" href="https://fonts.googleapis.com" />
        <link rel="preconnect" href="https://fonts.gstatic.com" crossOrigin="" />
        <link
          href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&family=Plus+Jakarta+Sans:wght@500;600;700&family=JetBrains+Mono:wght@400;500&display=swap"
          rel="stylesheet"
        />
      </head>
      <body>
        <ThemeProvider>
          <QueryProvider>
            <AppShell>{children}</AppShell>
            <AutoSaveWatcher />
          </QueryProvider>
          <Toaster richColors closeButton position="bottom-right" />
        </ThemeProvider>
      </body>
    </html>
  );
}
