'use client';

import * as React from 'react';
import { useStore } from '@/lib/store';

type Ctx = {
  resolvedTheme: 'light' | 'dark';
};

const ThemeContext = React.createContext<Ctx>({ resolvedTheme: 'light' });

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const theme = useStore((s) => s.theme);
  const [resolved, setResolved] = React.useState<'light' | 'dark'>('light');

  React.useEffect(() => {
    const apply = () => {
      let next: 'light' | 'dark' = 'light';
      if (theme === 'dark') next = 'dark';
      else if (theme === 'light') next = 'light';
      else if (typeof window !== 'undefined') {
        next = window.matchMedia('(prefers-color-scheme: dark)').matches
          ? 'dark'
          : 'light';
      }
      setResolved(next);
      const root = document.documentElement;
      root.classList.toggle('dark', next === 'dark');
    };
    apply();
    if (theme === 'system' && typeof window !== 'undefined') {
      const mq = window.matchMedia('(prefers-color-scheme: dark)');
      mq.addEventListener('change', apply);
      return () => mq.removeEventListener('change', apply);
    }
  }, [theme]);

  return (
    <ThemeContext.Provider value={{ resolvedTheme: resolved }}>
      {children}
    </ThemeContext.Provider>
  );
}

export function useResolvedTheme() {
  return React.useContext(ThemeContext).resolvedTheme;
}
