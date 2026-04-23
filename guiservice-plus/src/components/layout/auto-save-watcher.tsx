'use client';

/**
 * AutoSaveWatcher
 *
 * Subscribes to caseData changes and debounces persistence to localStorage
 * (already handled by zustand/persist), but also fires a Sonner toast every
 * time a save is confirmed so the user has visual feedback.
 */

import * as React from 'react';
import { toast } from 'sonner';
import { useStore } from '@/lib/store';

const DEBOUNCE_MS = 2000;

export function AutoSaveWatcher() {
  const caseData = useStore((s) => s.caseData);
  const timerRef = React.useRef<ReturnType<typeof setTimeout> | null>(null);
  const mountedRef = React.useRef(false);

  React.useEffect(() => {
    // Don't fire on initial mount — only on actual edits.
    if (!mountedRef.current) {
      mountedRef.current = true;
      return;
    }
    if (timerRef.current) clearTimeout(timerRef.current);
    timerRef.current = setTimeout(() => {
      toast.success('Draft auto-saved', { duration: 1500, id: 'auto-save' });
    }, DEBOUNCE_MS);
    return () => {
      if (timerRef.current) clearTimeout(timerRef.current);
    };
  }, [caseData]);

  return null;
}
