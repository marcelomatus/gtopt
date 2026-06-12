/**
 * AI provider configuration types and defaults.
 *
 * The user registers their own AI API key in the provider settings dialog.
 * The key is persisted in localStorage via the Zustand `persist` middleware
 * and sent as a bearer token from the browser to the Next.js `/api/ai/chat`
 * route, which proxies it to the chosen AI provider.
 *
 * Supported providers:
 *   openai  — OpenAI chat completions  (gpt-4o, gpt-4o-mini, etc.)
 *   anthropic — Anthropic messages API (claude-3-5-sonnet, claude-3-haiku, etc.)
 *   ollama  — OpenAI-compatible local inference (llama3.2, mistral, etc.)
 *   custom  — any OpenAI-compatible endpoint (LM Studio, vLLM, LiteLLM, etc.)
 */

export type AiProvider = 'openai' | 'anthropic' | 'ollama' | 'custom';

export type AiConfig = {
  provider: AiProvider;
  apiKey: string;
  model: string;
  /** Base URL used for ollama and custom providers.  Empty = provider default. */
  baseUrl: string;
  enabled: boolean;
  /** Max tokens to request from the provider per turn (default 2048). */
  maxTokens: number;
  /** Whether to show the chat panel on first load. */
  panelOpen: boolean;
};

export const DEFAULT_AI_CONFIG: AiConfig = {
  provider: 'openai',
  apiKey: '',
  model: 'gpt-4o-mini',
  baseUrl: '',
  enabled: false,
  maxTokens: 2048,
  panelOpen: false,
};

export const PROVIDER_DEFAULTS: Record<
  AiProvider,
  { models: string[]; baseUrl: string; label: string; supportsTools: boolean }
> = {
  openai: {
    models: ['gpt-4o', 'gpt-4o-mini', 'gpt-4-turbo', 'gpt-3.5-turbo'],
    baseUrl: 'https://api.openai.com/v1',
    label: 'OpenAI',
    supportsTools: true,
  },
  anthropic: {
    models: [
      'claude-3-5-sonnet-20241022',
      'claude-3-5-haiku-20241022',
      'claude-3-opus-20240229',
      'claude-3-haiku-20240307',
    ],
    baseUrl: 'https://api.anthropic.com',
    label: 'Anthropic',
    supportsTools: true,
  },
  ollama: {
    models: ['llama3.2', 'llama3.1', 'mistral', 'codellama', 'deepseek-r1'],
    baseUrl: 'http://localhost:11434/v1',
    label: 'Ollama (local)',
    supportsTools: false,
  },
  custom: {
    models: [],
    baseUrl: '',
    label: 'Custom endpoint',
    supportsTools: true,
  },
};

/** A single message in the conversation history. */
export type AiMessage = {
  id: string;
  role: 'user' | 'assistant' | 'tool';
  content: string;
  /** Populated when the assistant decides to call one or more tools. */
  toolCalls?: AiToolCall[];
  /** Only present on role='tool' messages — links back to the call. */
  toolCallId?: string;
  /** Human-readable tool name for display purposes. */
  toolName?: string;
  /** 'pending' while a tool call is executing client-side. */
  status?: 'pending' | 'done' | 'error';
  timestamp: number;
};

export type AiToolCall = {
  id: string;
  name: string;
  /** Raw JSON string of the arguments, as returned by the provider. */
  arguments: string;
};
