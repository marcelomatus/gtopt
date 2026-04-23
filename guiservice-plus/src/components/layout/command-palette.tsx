'use client';

import { Command } from 'cmdk';
import * as React from 'react';
import { useRouter } from 'next/navigation';
import {
  LayoutDashboard,
  FileEdit,
  Network,
  Briefcase,
  BarChart3,
  Sun,
  Moon,
  Laptop,
} from 'lucide-react';
import { useStore } from '@/lib/store';

export function CommandPalette({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
}) {
  const router = useRouter();
  const setTheme = useStore((s) => s.setTheme);

  React.useEffect(() => {
    function onKey(e: KeyboardEvent) {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        onOpenChange(!open);
      }
      if (e.key === 'Escape') onOpenChange(false);
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onOpenChange]);

  if (!open) return null;

  const go = (href: string) => {
    onOpenChange(false);
    router.push(href);
  };

  return (
    <div
      className="fixed inset-0 z-50 flex items-start justify-center bg-black/40 p-6 pt-[12vh]"
      onClick={() => onOpenChange(false)}
    >
      <Command
        label="Command palette"
        className="w-full max-w-lg overflow-hidden rounded-xl border bg-popover text-popover-foreground shadow-lg"
        onClick={(e) => e.stopPropagation()}
      >
        <Command.Input
          placeholder="Type a command or search…"
          className="w-full border-b bg-transparent px-4 py-3 text-sm outline-none"
        />
        <Command.List className="max-h-96 overflow-auto p-2">
          <Command.Empty className="px-4 py-6 text-center text-sm text-muted-foreground">
            No matches found.
          </Command.Empty>
          <Command.Group heading="Navigate" className="px-2 py-1 text-xs text-muted-foreground">
            <Command.Item onSelect={() => go('/')} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <LayoutDashboard className="h-4 w-4" /> Dashboard
            </Command.Item>
            <Command.Item onSelect={() => go('/case')} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <FileEdit className="h-4 w-4" /> Case editor
            </Command.Item>
            <Command.Item onSelect={() => go('/topology')} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <Network className="h-4 w-4" /> Topology
            </Command.Item>
            <Command.Item onSelect={() => go('/jobs')} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <Briefcase className="h-4 w-4" /> Jobs
            </Command.Item>
            <Command.Item onSelect={() => go('/results')} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <BarChart3 className="h-4 w-4" /> Results
            </Command.Item>
          </Command.Group>
          <Command.Group heading="Theme" className="px-2 py-1 text-xs text-muted-foreground">
            <Command.Item onSelect={() => { setTheme('light'); onOpenChange(false); }} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <Sun className="h-4 w-4" /> Light theme
            </Command.Item>
            <Command.Item onSelect={() => { setTheme('dark'); onOpenChange(false); }} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <Moon className="h-4 w-4" /> Dark theme
            </Command.Item>
            <Command.Item onSelect={() => { setTheme('system'); onOpenChange(false); }} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent">
              <Laptop className="h-4 w-4" /> System theme
            </Command.Item>
          </Command.Group>
        </Command.List>
      </Command>
    </div>
  );
}
