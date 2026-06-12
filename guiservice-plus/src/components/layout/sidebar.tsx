'use client';

import Link from 'next/link';
import { usePathname } from 'next/navigation';
import {
  LayoutDashboard,
  FileEdit,
  Network,
  BarChart3,
  Briefcase,
  Workflow,
} from 'lucide-react';
import { cn } from '@/lib/utils';

const nav = [
  { href: '/', label: 'Dashboard', icon: LayoutDashboard },
  { href: '/case', label: 'Case Editor', icon: FileEdit },
  { href: '/topology', label: 'Topology', icon: Network },
  { href: '/jobs', label: 'Jobs', icon: Briefcase },
  { href: '/results', label: 'Results', icon: BarChart3 },
];

export function Sidebar() {
  const pathname = usePathname() ?? '/';
  return (
    <aside className="group/sidebar flex h-screen w-60 shrink-0 flex-col border-r bg-sidebar text-sidebar-foreground">
      <div className="flex items-center gap-2 px-4 py-5">
        <div className="flex h-8 w-8 items-center justify-center rounded-md bg-primary">
          <Workflow className="h-4 w-4 text-primary-foreground" />
        </div>
        <div>
          <div className="text-sm font-semibold text-white">gtopt</div>
          <div className="text-xs text-sidebar-foreground/60">GUI Plus</div>
        </div>
      </div>
      <nav className="flex-1 space-y-0.5 px-2">
        {nav.map((item) => {
          const active =
            pathname === item.href ||
            (item.href !== '/' && pathname.startsWith(item.href));
          const Icon = item.icon;
          return (
            <Link
              key={item.href}
              href={item.href}
              className={cn(
                'flex items-center gap-3 rounded-md px-3 py-2 text-sm font-medium transition',
                active
                  ? 'bg-sidebar-accent/15 border-l-2 border-sidebar-accent text-white'
                  : 'text-sidebar-foreground/80 hover:bg-white/5 hover:text-white',
              )}
            >
              <Icon className="h-4 w-4 shrink-0" />
              <span>{item.label}</span>
            </Link>
          );
        })}
      </nav>
      <div className="border-t border-white/10 px-4 py-3 text-xs text-sidebar-foreground/50">
        Connected to Flask:{' '}
        <code className="text-sidebar-foreground/80">/flask/*</code>
      </div>
    </aside>
  );
}
