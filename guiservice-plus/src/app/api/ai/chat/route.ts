/**
 * Next.js API route: POST /api/ai/chat
 *
 * Proxies a chat-completion request to the user-configured AI provider.
 * Supports OpenAI (and OpenAI-compatible endpoints like Ollama / LM Studio /
 * vLLM / LiteLLM) and the Anthropic Messages API.
 *
 * The client sends:
 *   { config: AiConfig, messages: OpenAI-format message[], context: string }
 *
 * The route always returns a normalised OpenAI-format assistant message so
 * the frontend only needs to understand one response shape:
 *   {
 *     message: { role, content, tool_calls? },
 *     error?: string
 *   }
 */

import { NextRequest, NextResponse } from 'next/server';
import type { AiConfig } from '@/lib/ai-config';
import { AI_TOOL_DEFINITIONS } from '@/lib/ai-tools';

// ---------------------------------------------------------------------------
// Types mirrored from the frontend
// ---------------------------------------------------------------------------

type OAIMessage = {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content: string | null;
  tool_calls?: OAIToolCall[];
  tool_call_id?: string;
  name?: string;
};

type OAIToolCall = {
  id: string;
  type: 'function';
  function: { name: string; arguments: string };
};

type RequestBody = {
  config: AiConfig;
  messages: OAIMessage[];
};

// ---------------------------------------------------------------------------
// OpenAI / compatible (ollama, custom)
// ---------------------------------------------------------------------------

function isPrivateOrLocalHost(hostname: string): boolean {
  const h = hostname.toLowerCase();
  if (h === 'localhost' || h === '::1') return true;

  // Basic IPv4 checks
  const ipv4 = h.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if (ipv4) {
    const [a, b] = [Number(ipv4[1]), Number(ipv4[2])];
    if (a === 10) return true;
    if (a === 127) return true;
    if (a === 169 && b === 254) return true;
    if (a === 172 && b >= 16 && b <= 31) return true;
    if (a === 192 && b === 168) return true;
  }

  // Common local/private IPv6 patterns
  if (h.startsWith('fc') || h.startsWith('fd') || h.startsWith('fe80:')) return true;

  return false;
}

function getValidatedOpenAIBaseUrl(config: AiConfig): string {
  const rawBase = config.baseUrl || 'https://api.openai.com/v1';

  let parsed: URL;
  try {
    parsed = new URL(rawBase);
  } catch {
    throw new Error('Invalid AI base URL.');
  }

  if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
    throw new Error('AI base URL must use http or https.');
  }

  if (parsed.username || parsed.password) {
    throw new Error('AI base URL must not include credentials.');
  }

  if (isPrivateOrLocalHost(parsed.hostname)) {
    throw new Error('AI base URL points to a disallowed host.');
  }

  return parsed.toString().replace(/\/$/, '');
}

async function callOpenAI(
  config: AiConfig,
  messages: OAIMessage[],
  useTools: boolean,
): Promise<OAIMessage> {
  const base = getValidatedOpenAIBaseUrl(config);
  const url = `${base}/chat/completions`;

  const body: Record<string, unknown> = {
    model: config.model,
    messages,
    max_tokens: config.maxTokens ?? 2048,
    temperature: 0.2,
  };

  if (useTools) {
    body.tools = AI_TOOL_DEFINITIONS;
    body.tool_choice = 'auto';
  }

  const res = await fetch(url, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${config.apiKey}`,
    },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`OpenAI API error ${res.status}: ${text.slice(0, 400)}`);
  }

  const data = (await res.json()) as {
    choices: Array<{ message: OAIMessage }>;
  };

  return data.choices[0]?.message ?? { role: 'assistant', content: '(empty response)' };
}

// ---------------------------------------------------------------------------
// Anthropic Messages API adapter
// ---------------------------------------------------------------------------

type AnthropicContent =
  | { type: 'text'; text: string }
  | { type: 'tool_use'; id: string; name: string; input: Record<string, unknown> };

type AnthropicMessage = {
  role: 'user' | 'assistant';
  content: string | AnthropicContent[];
};

/** Convert an OpenAI-format tool definition to Anthropic tool format. */
function toAnthropicTool(def: (typeof AI_TOOL_DEFINITIONS)[0]) {
  return {
    name: def.function.name,
    description: def.function.description,
    input_schema: def.function.parameters,
  };
}

/** Convert OpenAI messages to Anthropic messages format. */
function toAnthropicMessages(
  messages: OAIMessage[],
): { system: string; messages: AnthropicMessage[] } {
  let system = '';
  const out: AnthropicMessage[] = [];

  for (const m of messages) {
    if (m.role === 'system') {
      system = m.content ?? '';
      continue;
    }
    if (m.role === 'tool') {
      // Append as a user turn wrapping a tool_result block
      out.push({
        role: 'user',
        content: [
          {
            // Anthropic's tool_result content block
            // eslint-disable-next-line @typescript-eslint/no-explicit-any
            ...(({ type: 'tool_result', tool_use_id: m.tool_call_id, content: m.content ?? '' } as any)),
          },
        ],
      });
      continue;
    }
    if (m.role === 'assistant' && m.tool_calls?.length) {
      out.push({
        role: 'assistant',
        content: m.tool_calls.map((tc) => ({
          type: 'tool_use' as const,
          id: tc.id,
          name: tc.function.name,
          input: JSON.parse(tc.function.arguments || '{}') as Record<string, unknown>,
        })),
      });
      continue;
    }
    out.push({
      role: m.role as 'user' | 'assistant',
      content: m.content ?? '',
    });
  }

  return { system, messages: out };
}

async function callAnthropic(config: AiConfig, messages: OAIMessage[]): Promise<OAIMessage> {
  const { system, messages: anthropicMessages } = toAnthropicMessages(messages);
  const url = 'https://api.anthropic.com/v1/messages';

  const body: Record<string, unknown> = {
    model: config.model,
    max_tokens: config.maxTokens ?? 2048,
    tools: AI_TOOL_DEFINITIONS.map(toAnthropicTool),
    messages: anthropicMessages,
  };
  if (system) body.system = system;

  const res = await fetch(url, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'x-api-key': config.apiKey,
      'anthropic-version': '2023-06-01',
    },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Anthropic API error ${res.status}: ${text.slice(0, 400)}`);
  }

  const data = (await res.json()) as { content: AnthropicContent[] };

  // Convert Anthropic response back to OpenAI format
  const textParts = data.content.filter((c) => c.type === 'text') as {
    type: 'text';
    text: string;
  }[];
  const toolParts = data.content.filter((c) => c.type === 'tool_use') as {
    type: 'tool_use';
    id: string;
    name: string;
    input: Record<string, unknown>;
  }[];

  const msg: OAIMessage = {
    role: 'assistant',
    content: textParts.map((t) => t.text).join('\n') || null,
  };

  if (toolParts.length > 0) {
    msg.tool_calls = toolParts.map((t) => ({
      id: t.id,
      type: 'function' as const,
      function: {
        name: t.name,
        arguments: JSON.stringify(t.input),
      },
    }));
  }

  return msg;
}

// ---------------------------------------------------------------------------
// Route handler
// ---------------------------------------------------------------------------

export async function POST(req: NextRequest): Promise<NextResponse> {
  let body: RequestBody;
  try {
    body = (await req.json()) as RequestBody;
  } catch {
    return NextResponse.json({ error: 'Invalid JSON body.' }, { status: 400 });
  }

  const { config, messages } = body;

  if (!config?.apiKey) {
    return NextResponse.json(
      { error: 'No API key configured. Open Settings → AI to add your key.' },
      { status: 400 },
    );
  }

  if (!Array.isArray(messages) || messages.length === 0) {
    return NextResponse.json({ error: 'messages array is required.' }, { status: 400 });
  }

  try {
    let message: OAIMessage;

    if (config.provider === 'anthropic') {
      message = await callAnthropic(config, messages);
    } else {
      // openai / ollama / custom — all use the OpenAI chat completions format
      const useTools = config.provider !== 'ollama';
      message = await callOpenAI(config, messages, useTools);
    }

    return NextResponse.json({ message });
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err);
    return NextResponse.json({ error: msg }, { status: 502 });
  }
}
