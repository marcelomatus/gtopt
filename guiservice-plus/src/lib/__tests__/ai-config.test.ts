import { describe, it, expect } from 'vitest';
import {
  DEFAULT_AI_CONFIG,
  PROVIDER_DEFAULTS,
  type AiProvider,
} from '@/lib/ai-config';

describe('DEFAULT_AI_CONFIG', () => {
  it('has all required fields', () => {
    expect(DEFAULT_AI_CONFIG.provider).toBe('openai');
    expect(DEFAULT_AI_CONFIG.enabled).toBe(false);
    expect(DEFAULT_AI_CONFIG.apiKey).toBe('');
    expect(DEFAULT_AI_CONFIG.maxTokens).toBe(2048);
    expect(DEFAULT_AI_CONFIG.panelOpen).toBe(false);
  });
});

describe('PROVIDER_DEFAULTS', () => {
  const providers: AiProvider[] = ['openai', 'anthropic', 'ollama', 'custom'];

  it('has entries for all 4 providers', () => {
    for (const p of providers) {
      expect(PROVIDER_DEFAULTS[p]).toBeDefined();
    }
  });

  it('openai supports tools', () => {
    expect(PROVIDER_DEFAULTS.openai.supportsTools).toBe(true);
  });

  it('anthropic supports tools', () => {
    expect(PROVIDER_DEFAULTS.anthropic.supportsTools).toBe(true);
  });

  it('ollama does not require tools (may not support)', () => {
    expect(typeof PROVIDER_DEFAULTS.ollama.supportsTools).toBe('boolean');
  });

  it('openai has popular models listed', () => {
    expect(PROVIDER_DEFAULTS.openai.models).toContain('gpt-4o-mini');
    expect(PROVIDER_DEFAULTS.openai.models).toContain('gpt-4o');
  });

  it('anthropic has claude models', () => {
    const models = PROVIDER_DEFAULTS.anthropic.models.join(' ');
    expect(models).toContain('claude');
  });

  it('ollama has a localhost baseUrl', () => {
    expect(PROVIDER_DEFAULTS.ollama.baseUrl).toContain('localhost');
  });
});
