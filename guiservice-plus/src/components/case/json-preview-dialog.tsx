'use client';

import * as React from 'react';
import * as Dialog from '@radix-ui/react-dialog';
import { useStore } from '@/lib/store';
import { Button } from '@/components/ui/button';
import { X, Copy } from 'lucide-react';

export function JsonPreviewDialog({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
}) {
  const caseData = useStore((s) => s.caseData);
  const json = React.useMemo(() => JSON.stringify(caseData, null, 2), [caseData]);

  const copy = () => {
    if (typeof navigator !== 'undefined' && navigator.clipboard) {
      navigator.clipboard.writeText(json).catch(() => {
        /* silent */
      });
    }
  };

  return (
    <Dialog.Root open={open} onOpenChange={onOpenChange}>
      <Dialog.Portal>
        <Dialog.Overlay className="fixed inset-0 z-50 bg-black/50" />
        <Dialog.Content className="fixed left-1/2 top-1/2 z-50 w-[min(95vw,800px)] -translate-x-1/2 -translate-y-1/2 rounded-xl border bg-popover p-0 text-popover-foreground shadow-lg">
          <div className="flex items-center justify-between border-b px-4 py-3">
            <Dialog.Title className="text-lg font-semibold">
              Case JSON
            </Dialog.Title>
            <div className="flex gap-2">
              <Button variant="outline" size="sm" onClick={copy}>
                <Copy className="mr-1 h-3.5 w-3.5" /> Copy
              </Button>
              <Dialog.Close asChild>
                <Button variant="ghost" size="icon">
                  <X className="h-4 w-4" />
                </Button>
              </Dialog.Close>
            </div>
          </div>
          <pre className="max-h-[70vh] overflow-auto bg-background p-4 text-xs font-mono leading-relaxed">
            {json}
          </pre>
        </Dialog.Content>
      </Dialog.Portal>
    </Dialog.Root>
  );
}
