'use client';

import { Moon, Sun, Laptop, Command as CmdIcon, Bot } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { useStore, type Theme } from '@/lib/store';
import { DEFAULT_AI_CONFIG } from '@/lib/ai-config';
import { cn } from '@/lib/utils';

function ThemeCycle() {
  const theme = useStore((s) => s.theme);
  const setTheme = useStore((s) => s.setTheme);

  const next: Record<Theme, Theme> = {
    light: 'dark',
    dark: 'system',
    system: 'light',
  };
  const Icon = theme === 'light' ? Sun : theme === 'dark' ? Moon : Laptop;

  return (
    <Button
      variant="ghost"
      size="icon"
      aria-label={`Theme: ${theme}`}
      onClick={() => setTheme(next[theme])}
    >
      <Icon className="h-4 w-4" />
    </Button>
  );
}

export function TopBar({
  onOpenCommandPalette,
  onToggleAi,
}: {
  onOpenCommandPalette?: () => void;
  onToggleAi?: () => void;
}) {
  const aiEnabled = useStore((s) => (s.aiConfig ?? DEFAULT_AI_CONFIG).enabled);

  return (
    <header className="flex h-14 items-center justify-between border-b bg-background px-6">
      <div className="text-sm text-muted-foreground">
        GTEP planning workbench — connected to the Flask guiservice
      </div>
      <div className="flex items-center gap-2">
        <Button
          variant="outline"
          size="sm"
          onClick={onOpenCommandPalette}
          className="gap-2"
        >
          <CmdIcon className="h-3.5 w-3.5" />
          <span className="text-xs">⌘K</span>
        </Button>

        {/* AI toggle button */}
        <Button
          variant="ghost"
          size="icon"
          aria-label="Toggle AI chat panel"
          onClick={onToggleAi}
          className="relative"
        >
          <Bot className="h-4 w-4" />
          {aiEnabled && (
            <span
              className={cn(
                'absolute right-1.5 top-1.5 h-2 w-2 rounded-full bg-emerald-500 ring-2 ring-background',
              )}
              aria-label="AI enabled"
            />
          )}
        </Button>

        <ThemeCycle />
      </div>
    </header>
  );
}
