'use client';

import { Moon, Sun, Laptop, Command as CmdIcon } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { useStore, type Theme } from '@/lib/store';

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
}: {
  onOpenCommandPalette?: () => void;
}) {
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
        <ThemeCycle />
      </div>
    </header>
  );
}
