'use client';

import { Command } from 'cmdk';
import * as React from 'react';
import { useRouter } from 'next/navigation';
import { toast } from 'sonner';
import {
  LayoutDashboard,
  FileEdit,
  Network,
  Briefcase,
  BarChart3,
  Sun,
  Moon,
  Laptop,
  Undo2,
  Redo2,
  RotateCcw,
  Download,
  Wand2,
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
  const undo = useStore((s) => s.undo);
  const redo = useStore((s) => s.redo);
  const resetCaseData = useStore((s) => s.resetCaseData);
  const caseData = useStore((s) => s.caseData);
  const historyLen = useStore((s) => s.history.length);
  const futureLen = useStore((s) => s.future.length);

  React.useEffect(() => {
    function onKey(e: KeyboardEvent) {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        onOpenChange(!open);
      }
      if (e.key === 'Escape') onOpenChange(false);
      // Global undo / redo shortcuts even when palette is closed
      if (!open) {
        if ((e.metaKey || e.ctrlKey) && !e.shiftKey && e.key.toLowerCase() === 'z') {
          e.preventDefault();
          undo();
          toast.info('Undone', { duration: 800, id: 'undo' });
        }
        if (
          ((e.metaKey || e.ctrlKey) && e.shiftKey && e.key.toLowerCase() === 'z') ||
          ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'y')
        ) {
          e.preventDefault();
          redo();
          toast.info('Redone', { duration: 800, id: 'redo' });
        }
      }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [open, onOpenChange, undo, redo]);

  if (!open) return null;

  const go = (href: string) => {
    onOpenChange(false);
    router.push(href);
  };

  const downloadJson = () => {
    const blob = new Blob([JSON.stringify(caseData, null, 2)], {
      type: 'application/json',
    });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${caseData.case_name ?? 'case'}.json`;
    a.click();
    URL.revokeObjectURL(url);
    onOpenChange(false);
    toast.success('JSON downloaded');
  };

  const newCase = () => {
    resetCaseData();
    onOpenChange(false);
    toast.info('New blank case created');
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
            {([
              ['/', 'Dashboard', LayoutDashboard],
              ['/case', 'Case editor', FileEdit],
              ['/topology', 'Topology', Network],
              ['/jobs', 'Jobs', Briefcase],
              ['/results', 'Results', BarChart3],
            ] as [string, string, React.ElementType][]).map(([href, label, Icon]) => (
              <Command.Item
                key={href}
                onSelect={() => go(href)}
                className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent"
              >
                <Icon className="h-4 w-4" /> {label}
              </Command.Item>
            ))}
          </Command.Group>

          <Command.Group heading="Case" className="px-2 py-1 text-xs text-muted-foreground">
            <Command.Item
              onSelect={newCase}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent"
            >
              <Wand2 className="h-4 w-4" /> New blank case
            </Command.Item>
            <Command.Item
              onSelect={() => go('/case?wizard=1')}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent"
            >
              <Wand2 className="h-4 w-4" /> New case wizard…
            </Command.Item>
            <Command.Item
              onSelect={downloadJson}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent"
            >
              <Download className="h-4 w-4" /> Download case JSON
            </Command.Item>
            <Command.Item
              onSelect={() => { undo(); onOpenChange(false); toast.info('Undone', { duration: 800, id: 'undo' }); }}
              disabled={historyLen === 0}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent aria-disabled:opacity-40"
            >
              <Undo2 className="h-4 w-4" /> Undo{historyLen > 0 ? ` (${historyLen})` : ''}
            </Command.Item>
            <Command.Item
              onSelect={() => { redo(); onOpenChange(false); toast.info('Redone', { duration: 800, id: 'redo' }); }}
              disabled={futureLen === 0}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent aria-disabled:opacity-40"
            >
              <Redo2 className="h-4 w-4" /> Redo{futureLen > 0 ? ` (${futureLen})` : ''}
            </Command.Item>
            <Command.Item
              onSelect={newCase}
              className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm text-destructive aria-selected:bg-accent"
            >
              <RotateCcw className="h-4 w-4" /> Reset to blank case
            </Command.Item>
          </Command.Group>

          <Command.Group heading="Theme" className="px-2 py-1 text-xs text-muted-foreground">
            {([
              ['light', 'Light theme', Sun],
              ['dark', 'Dark theme', Moon],
              ['system', 'System theme', Laptop],
            ] as [Parameters<typeof setTheme>[0], string, React.ElementType][]).map(([t, label, Icon]) => (
              <Command.Item
                key={t}
                onSelect={() => { setTheme(t); onOpenChange(false); }}
                className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-2 text-sm aria-selected:bg-accent"
              >
                <Icon className="h-4 w-4" /> {label}
              </Command.Item>
            ))}
          </Command.Group>
        </Command.List>
      </Command>
    </div>
  );
}
