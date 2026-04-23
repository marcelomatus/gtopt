'use client';

import * as React from 'react';
import * as SwitchPrimitive from '@radix-ui/react-switch';
import { Eye, EyeOff } from 'lucide-react';
import { toast } from 'sonner';

import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
  DialogFooter,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useStore } from '@/lib/store';
import {
  type AiProvider,
  DEFAULT_AI_CONFIG,
  PROVIDER_DEFAULTS,
} from '@/lib/ai-config';
import { cn } from '@/lib/utils';

const PROVIDERS: { value: AiProvider; label: string }[] = [
  { value: 'openai', label: 'OpenAI' },
  { value: 'anthropic', label: 'Anthropic' },
  { value: 'ollama', label: 'Ollama (local)' },
  { value: 'custom', label: 'Custom endpoint' },
];

export function AiProviderSettings({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
}) {
  const aiConfig = useStore((s) => s.aiConfig ?? DEFAULT_AI_CONFIG);
  const setAiConfig = useStore((s) => s.setAiConfig);

  // Local form state — initialised from the store when the dialog opens
  const [provider, setProvider] = React.useState<AiProvider>(aiConfig.provider);
  const [apiKey, setApiKey] = React.useState(aiConfig.apiKey);
  const [model, setModel] = React.useState(aiConfig.model);
  const [baseUrl, setBaseUrl] = React.useState(aiConfig.baseUrl);
  const [maxTokens, setMaxTokens] = React.useState(aiConfig.maxTokens);
  const [enabled, setEnabled] = React.useState(aiConfig.enabled);
  const [showKey, setShowKey] = React.useState(false);

  // Sync form state from store when dialog opens
  React.useEffect(() => {
    if (open) {
      setProvider(aiConfig.provider);
      setApiKey(aiConfig.apiKey);
      setModel(aiConfig.model);
      setBaseUrl(aiConfig.baseUrl);
      setMaxTokens(aiConfig.maxTokens);
      setEnabled(aiConfig.enabled);
      setShowKey(false);
    }
  }, [open, aiConfig]);

  // When provider changes, suggest a default model and base URL
  const handleProviderChange = (p: AiProvider) => {
    setProvider(p);
    const defaults = PROVIDER_DEFAULTS[p];
    if (defaults.models.length > 0) setModel(defaults.models[0]);
    setBaseUrl(defaults.baseUrl);
  };

  const showBaseUrl = provider === 'ollama' || provider === 'custom';
  const suggestedModels = PROVIDER_DEFAULTS[provider].models;

  const handleSave = () => {
    setAiConfig({ provider, apiKey, model, baseUrl, maxTokens, enabled });
    toast.success('AI provider settings saved.');
    onOpenChange(false);
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-md">
        <DialogHeader>
          <DialogTitle>AI Provider Settings</DialogTitle>
          <DialogDescription>
            Register your AI provider credentials. The API key is stored
            locally in your browser and never sent to our servers.
          </DialogDescription>
        </DialogHeader>

        <div className="mt-4 flex flex-col gap-5">
          {/* Provider selector */}
          <div className="flex flex-col gap-2">
            <Label>Provider</Label>
            <div className="flex flex-wrap gap-2">
              {PROVIDERS.map((p) => (
                <button
                  key={p.value}
                  type="button"
                  onClick={() => handleProviderChange(p.value)}
                  className={cn(
                    'rounded-md border px-3 py-1.5 text-sm transition-colors',
                    provider === p.value
                      ? 'border-primary bg-primary text-primary-foreground'
                      : 'border-input bg-background hover:bg-accent hover:text-accent-foreground',
                  )}
                >
                  {p.label}
                </button>
              ))}
            </div>
          </div>

          {/* API Key */}
          <div className="flex flex-col gap-2">
            <Label htmlFor="ai-api-key">API Key</Label>
            <div className="relative">
              <Input
                id="ai-api-key"
                type={showKey ? 'text' : 'password'}
                placeholder={
                  provider === 'ollama' ? 'Not required for Ollama' : 'sk-…'
                }
                value={apiKey}
                onChange={(e) => setApiKey(e.target.value)}
                className="pr-10"
                autoComplete="off"
              />
              <button
                type="button"
                aria-label={showKey ? 'Hide API key' : 'Show API key'}
                onClick={() => setShowKey((v) => !v)}
                className="absolute right-2 top-1/2 -translate-y-1/2 text-muted-foreground hover:text-foreground"
              >
                {showKey ? (
                  <EyeOff className="h-4 w-4" />
                ) : (
                  <Eye className="h-4 w-4" />
                )}
              </button>
            </div>
          </div>

          {/* Model */}
          <div className="flex flex-col gap-2">
            <Label htmlFor="ai-model">Model</Label>
            <Input
              id="ai-model"
              list="ai-model-list"
              placeholder="e.g. gpt-4o-mini"
              value={model}
              onChange={(e) => setModel(e.target.value)}
            />
            <datalist id="ai-model-list">
              {suggestedModels.map((m) => (
                <option key={m} value={m} />
              ))}
            </datalist>
          </div>

          {/* Base URL — only for ollama / custom */}
          {showBaseUrl && (
            <div className="flex flex-col gap-2">
              <Label htmlFor="ai-base-url">Base URL</Label>
              <Input
                id="ai-base-url"
                placeholder={PROVIDER_DEFAULTS[provider].baseUrl || 'https://…'}
                value={baseUrl}
                onChange={(e) => setBaseUrl(e.target.value)}
              />
            </div>
          )}

          {/* Max tokens */}
          <div className="flex flex-col gap-2">
            <div className="flex items-center justify-between">
              <Label htmlFor="ai-max-tokens">Max tokens</Label>
              <span className="text-sm tabular-nums text-muted-foreground">
                {maxTokens}
              </span>
            </div>
            <input
              id="ai-max-tokens"
              type="range"
              min={256}
              max={8192}
              step={256}
              value={maxTokens}
              onChange={(e) => setMaxTokens(Number(e.target.value))}
              className="w-full accent-primary"
            />
            <div className="flex justify-between text-xs text-muted-foreground">
              <span>256</span>
              <span>8192</span>
            </div>
          </div>

          {/* Enable toggle */}
          <div className="flex items-center justify-between">
            <Label htmlFor="ai-enabled" className="cursor-pointer">
              Enable AI assistant
            </Label>
            <SwitchPrimitive.Root
              id="ai-enabled"
              checked={enabled}
              onCheckedChange={setEnabled}
              className={cn(
                'relative inline-flex h-6 w-11 shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2',
                enabled ? 'bg-primary' : 'bg-input',
              )}
            >
              <SwitchPrimitive.Thumb
                className={cn(
                  'pointer-events-none block h-5 w-5 rounded-full bg-background shadow-lg ring-0 transition-transform',
                  enabled ? 'translate-x-5' : 'translate-x-0',
                )}
              />
            </SwitchPrimitive.Root>
          </div>
        </div>

        <DialogFooter className="mt-6">
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            Cancel
          </Button>
          <Button onClick={handleSave}>Save settings</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
