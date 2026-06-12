'use client';

import * as React from 'react';
import { Sidebar } from './sidebar';
import { TopBar } from './top-bar';
import { CommandPalette } from './command-palette';
import { AiChatPanel } from '@/components/ai/ai-chat-panel';

export function AppShell({ children }: { children: React.ReactNode }) {
  const [cmdkOpen, setCmdkOpen] = React.useState(false);
  const [aiOpen, setAiOpen] = React.useState(false);

  return (
    <div className="flex h-screen w-full overflow-hidden bg-background text-foreground">
      <Sidebar />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar
          onOpenCommandPalette={() => setCmdkOpen(true)}
          onToggleAi={() => setAiOpen((v) => !v)}
        />
        <main className="min-h-0 flex-1 overflow-auto p-6">{children}</main>
      </div>
      <CommandPalette open={cmdkOpen} onOpenChange={setCmdkOpen} />
      <AiChatPanel open={aiOpen} onClose={() => setAiOpen(false)} />
    </div>
  );
}
