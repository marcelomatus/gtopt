'use client';

import * as React from 'react';
import { Bot, Settings, X, Send, Loader2, Wrench } from 'lucide-react';
import { toast } from 'sonner';

import { Button } from '@/components/ui/button';
import { ScrollArea } from '@/components/ui/scroll-area';
import { useStore } from '@/lib/store';
import { DEFAULT_AI_CONFIG, type AiMessage } from '@/lib/ai-config';
import {
  executeToolCall,
  buildSystemPrompt,
  AI_TOOL_DEFINITIONS,
} from '@/lib/ai-tools';
import { AiProviderSettings } from './ai-provider-settings';
import { cn } from '@/lib/utils';

// ---------------------------------------------------------------------------
// Types for the OpenAI-compatible chat API
// ---------------------------------------------------------------------------

type ApiToolCall = {
  id: string;
  type: 'function';
  function: { name: string; arguments: string };
};

type ApiMessage =
  | { role: 'system'; content: string }
  | { role: 'user'; content: string }
  | { role: 'assistant'; content: string | null; tool_calls?: ApiToolCall[] }
  | { role: 'tool'; content: string; tool_call_id: string };

type ApiChatResponse = {
  choices?: Array<{
    message: {
      role: string;
      content: string | null;
      tool_calls?: ApiToolCall[];
    };
  }>;
  error?: { message: string };
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function nanoid() {
  return Math.random().toString(36).slice(2, 10);
}

/** Convert our AiMessage store entries to the API wire format. */
function toApiMessages(msgs: AiMessage[]): ApiMessage[] {
  return msgs.flatMap((m): ApiMessage[] => {
    if (m.role === 'user') return [{ role: 'user', content: m.content }];
    if (m.role === 'assistant') {
      const msg: ApiMessage = {
        role: 'assistant',
        content: m.content || null,
        ...(m.toolCalls && m.toolCalls.length > 0
          ? {
              tool_calls: m.toolCalls.map((tc) => ({
                id: tc.id,
                type: 'function' as const,
                function: { name: tc.name, arguments: tc.arguments },
              })),
            }
          : {}),
      };
      return [msg];
    }
    if (m.role === 'tool' && m.toolCallId) {
      return [
        {
          role: 'tool',
          content: m.content,
          tool_call_id: m.toolCallId,
        },
      ];
    }
    return [];
  });
}

// ---------------------------------------------------------------------------
// Message bubble components
// ---------------------------------------------------------------------------

function UserBubble({ content }: { content: string }) {
  return (
    <div className="flex justify-end">
      <div className="max-w-[80%] rounded-2xl rounded-tr-sm bg-primary px-4 py-2.5 text-sm text-primary-foreground">
        {content}
      </div>
    </div>
  );
}

function AssistantBubble({ content }: { content: string }) {
  return (
    <div className="flex justify-start">
      <div className="max-w-[80%] rounded-2xl rounded-tl-sm bg-card px-4 py-2.5 text-sm text-card-foreground shadow-sm ring-1 ring-border/50">
        <pre className="whitespace-pre-wrap font-sans">{content}</pre>
      </div>
    </div>
  );
}

function ToolBubble({ msg }: { msg: AiMessage }) {
  const isPending = msg.status === 'pending';
  const isError = msg.status === 'error';

  return (
    <div
      className={cn(
        'w-full rounded-lg border px-3 py-2 text-xs font-mono',
        isPending && 'border-border bg-muted/40 text-muted-foreground',
        isError && 'border-destructive/40 bg-destructive/5 text-destructive',
        !isPending &&
          !isError &&
          'border-emerald-500/20 bg-emerald-500/5 text-foreground',
      )}
    >
      <div className="mb-1 flex items-center gap-1.5 font-sans font-medium">
        <Wrench className="h-3 w-3 shrink-0" />
        <span className="text-[11px] uppercase tracking-wide text-muted-foreground">
          {msg.toolName ?? 'tool'}
        </span>
        {isPending && (
          <Loader2 className="ml-auto h-3 w-3 animate-spin text-muted-foreground" />
        )}
      </div>
      <pre className="max-h-40 overflow-y-auto whitespace-pre-wrap">
        {msg.content || '…'}
      </pre>
    </div>
  );
}

function MessageList({ messages }: { messages: AiMessage[] }) {
  const bottomRef = React.useRef<HTMLDivElement>(null);

  React.useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages.length]);

  if (messages.length === 0) {
    return (
      <div className="flex h-full flex-col items-center justify-center gap-3 p-6 text-center text-muted-foreground">
        <Bot className="h-10 w-10 opacity-30" />
        <p className="text-sm">
          Ask me anything about your case — I can read element data, scale
          fields, add/remove elements, and query results.
        </p>
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-3 p-4">
      {messages.map((msg) => {
        if (msg.role === 'user') return <UserBubble key={msg.id} content={msg.content} />;
        if (msg.role === 'tool') return <ToolBubble key={msg.id} msg={msg} />;
        // assistant — skip pure tool-call messages with no text
        if (!msg.content && msg.toolCalls && msg.toolCalls.length > 0) return null;
        if (msg.role === 'assistant')
          return <AssistantBubble key={msg.id} content={msg.content} />;
        return null;
      })}
      <div ref={bottomRef} />
    </div>
  );
}

// ---------------------------------------------------------------------------
// Main panel
// ---------------------------------------------------------------------------

export function AiChatPanel({
  open,
  onClose,
}: {
  open: boolean;
  onClose: () => void;
}) {
  const aiConfig = useStore((s) => s.aiConfig ?? DEFAULT_AI_CONFIG);
  const aiMessages = useStore((s) => s.aiMessages ?? []);
  const caseData = useStore((s) => s.caseData);
  const results = useStore((s) => s.results);
  const updateCaseData = useStore((s) => s.updateCaseData);
  const addAiMessage = useStore((s) => s.addAiMessage);
  const updateAiMessage = useStore((s) => s.updateAiMessage);
  const clearAiMessages = useStore((s) => s.clearAiMessages);

  const [input, setInput] = React.useState('');
  const [loading, setLoading] = React.useState(false);
  const [settingsOpen, setSettingsOpen] = React.useState(false);
  const textareaRef = React.useRef<HTMLTextAreaElement>(null);

  // Auto-resize textarea
  React.useEffect(() => {
    const el = textareaRef.current;
    if (!el) return;
    el.style.height = 'auto';
    el.style.height = `${Math.min(el.scrollHeight, 120)}px`;
  }, [input]);

  const sendMessage = React.useCallback(async () => {
    const text = input.trim();
    if (!text || loading) return;

    // Guard: AI must be configured
    if (!aiConfig.enabled || !aiConfig.apiKey) {
      toast.error('No AI configured. Click ⚙ to add your key.');
      return;
    }

    setInput('');
    setLoading(true);

    // 1. Add user message to store
    const userMsg: AiMessage = {
      id: nanoid(),
      role: 'user',
      content: text,
      timestamp: Date.now(),
    };
    addAiMessage(userMsg);

    // 2. Build messages array for API
    const systemMsg: ApiMessage = {
      role: 'system',
      content: buildSystemPrompt(caseData, results),
    };
    const historyMsgs = toApiMessages([...aiMessages, userMsg]);

    let apiMessages: ApiMessage[] = [systemMsg, ...historyMsgs];

    try {
      // 3. Initial POST
      let res = await fetch('/api/ai/chat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          config: aiConfig,
          messages: apiMessages,
          tools: AI_TOOL_DEFINITIONS,
        }),
      });

      if (!res.ok) {
        const txt = await res.text();
        throw new Error(`AI request failed (${res.status}): ${txt}`);
      }

      let data: ApiChatResponse = (await res.json()) as ApiChatResponse;

      if (data.error) throw new Error(data.error.message);

      // 4. Tool-call agentic loop (up to 5 rounds)
      let round = 0;
      while (
        round < 5 &&
        data.choices?.[0]?.message.tool_calls &&
        data.choices[0].message.tool_calls.length > 0
      ) {
        round++;
        const assistantMsg = data.choices[0].message;
        const toolCalls = assistantMsg.tool_calls!;

        // a. Record assistant message with tool_calls
        const assistantStoreMsg: AiMessage = {
          id: nanoid(),
          role: 'assistant',
          content: assistantMsg.content ?? '',
          toolCalls: toolCalls.map((tc) => ({
            id: tc.id,
            name: tc.function.name,
            arguments: tc.function.arguments,
          })),
          timestamp: Date.now(),
        };
        addAiMessage(assistantStoreMsg);

        // b. Execute each tool call
        const toolResultMsgs: ApiMessage[] = [];

        for (const tc of toolCalls) {
          const toolMsgId = nanoid();

          // Add pending tool message
          const pendingToolMsg: AiMessage = {
            id: toolMsgId,
            role: 'tool',
            content: '',
            toolCallId: tc.id,
            toolName: tc.function.name,
            status: 'pending',
            timestamp: Date.now(),
          };
          addAiMessage(pendingToolMsg);

          // Parse arguments
          let args: Record<string, unknown> = {};
          try {
            args = JSON.parse(tc.function.arguments) as Record<string, unknown>;
          } catch {
            // leave as empty object
          }

          // Execute
          let result: string;
          try {
            result = executeToolCall(tc.function.name, args, {
              caseData,
              results,
              updateCaseData,
            });
          } catch (err) {
            result = `Error: ${err instanceof Error ? err.message : String(err)}`;
            updateAiMessage(toolMsgId, { content: result, status: 'error' });
            toolResultMsgs.push({
              role: 'tool',
              content: result,
              tool_call_id: tc.id,
            });
            continue;
          }

          // Update tool message as done
          updateAiMessage(toolMsgId, { content: result, status: 'done' });

          toolResultMsgs.push({
            role: 'tool',
            content: result,
            tool_call_id: tc.id,
          });
        }

        // c. Build follow-up messages and POST again
        apiMessages = [
          ...apiMessages,
          {
            role: 'assistant',
            content: assistantMsg.content ?? null,
            tool_calls: toolCalls,
          },
          ...toolResultMsgs,
        ];

        res = await fetch('/api/ai/chat', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            config: aiConfig,
            messages: apiMessages,
            tools: AI_TOOL_DEFINITIONS,
          }),
        });

        if (!res.ok) {
          const txt = await res.text();
          throw new Error(`AI follow-up request failed (${res.status}): ${txt}`);
        }

        data = (await res.json()) as ApiChatResponse;
        if (data.error) throw new Error(data.error.message);
      }

      // 5. Add final assistant text response
      const finalContent = data.choices?.[0]?.message.content ?? '(no response)';
      addAiMessage({
        id: nanoid(),
        role: 'assistant',
        content: finalContent,
        timestamp: Date.now(),
      });
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      toast.error(msg);
      addAiMessage({
        id: nanoid(),
        role: 'assistant',
        content: `⚠️ ${msg}`,
        timestamp: Date.now(),
      });
    } finally {
      setLoading(false);
    }
  }, [
    input,
    loading,
    aiConfig,
    caseData,
    results,
    aiMessages,
    addAiMessage,
    updateAiMessage,
    updateCaseData,
  ]);

  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      void sendMessage();
    }
  };

  return (
    <>
      {/* Backdrop (mobile) */}
      {open && (
        <div
          className="fixed inset-0 z-30 bg-black/20 backdrop-blur-[1px] lg:hidden"
          onClick={onClose}
          aria-hidden="true"
        />
      )}

      {/* Panel */}
      <aside
        aria-label="AI chat panel"
        className={cn(
          'fixed bottom-0 right-0 top-0 z-40 flex w-[420px] flex-col border-l bg-background shadow-2xl transition-transform duration-300 ease-in-out',
          open ? 'translate-x-0' : 'translate-x-full',
        )}
      >
        {/* Header */}
        <header className="flex h-12 shrink-0 items-center gap-2 border-b px-4">
          <Bot className="h-4 w-4 text-primary" />
          <span className="flex-1 text-sm font-semibold">gtopt AI</span>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8"
            aria-label="AI settings"
            onClick={() => setSettingsOpen(true)}
          >
            <Settings className="h-4 w-4" />
          </Button>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8"
            aria-label="Clear conversation"
            onClick={clearAiMessages}
            title="Clear conversation"
          >
            <span className="text-xs font-medium text-muted-foreground">CLR</span>
          </Button>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8"
            aria-label="Close AI panel"
            onClick={onClose}
          >
            <X className="h-4 w-4" />
          </Button>
        </header>

        {/* Message list */}
        <ScrollArea className="flex-1">
          <div
            role="log"
            aria-live="polite"
            aria-label="Conversation history"
          >
            <MessageList messages={aiMessages} />
            {loading && (
              <div className="flex items-center gap-2 px-4 pb-4 text-sm text-muted-foreground">
                <Loader2 className="h-4 w-4 animate-spin" />
                <span>Thinking…</span>
              </div>
            )}
          </div>
        </ScrollArea>

        {/* Input footer */}
        <footer className="shrink-0 border-t p-3">
          <div className="flex items-end gap-2">
            <textarea
              ref={textareaRef}
              rows={1}
              value={input}
              onChange={(e) => setInput(e.target.value)}
              onKeyDown={handleKeyDown}
              placeholder="Ask about your case… (Enter to send)"
              disabled={loading}
              className="flex-1 resize-none rounded-lg border border-input bg-background px-3 py-2 text-sm placeholder:text-muted-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-0 disabled:cursor-not-allowed disabled:opacity-50"
              style={{ minHeight: '40px', maxHeight: '120px' }}
            />
            <Button
              size="icon"
              disabled={loading || !input.trim()}
              onClick={() => void sendMessage()}
              aria-label="Send message"
              className="shrink-0"
            >
              {loading ? (
                <Loader2 className="h-4 w-4 animate-spin" />
              ) : (
                <Send className="h-4 w-4" />
              )}
            </Button>
          </div>
          <p className="mt-1.5 text-center text-[10px] text-muted-foreground/60">
            Shift+Enter for new line · Enter to send
          </p>
        </footer>
      </aside>

      {/* Provider settings dialog */}
      <AiProviderSettings open={settingsOpen} onOpenChange={setSettingsOpen} />
    </>
  );
}
