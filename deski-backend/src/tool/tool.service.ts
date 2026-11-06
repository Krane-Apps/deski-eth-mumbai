import { HttpException, Injectable, Logger } from '@nestjs/common';
import OpenAI, { APIConnectionError, APIError, BadRequestError } from 'openai';
import { env } from '../hedera/hedera.service';
import { StakeService } from '../stake/stake.service';

const SYSTEM_PROMPT =
  'You decide if Deski should stake or unstake ATTN based on the conversation. ' +
  'Call `stake` only when the user clearly asks to start a focus attention session or stake. ' +
  'Call `unstake` only when they clearly ask to stop or end it. ' +
  'Otherwise call no tool.';

const TOOLS: OpenAI.Chat.Completions.ChatCompletionTool[] = [
  {
    type: 'function',
    function: {
      name: 'stake',
      description: 'Stake ATTN to start a focus session.',
      parameters: {
        type: 'object',
        properties: {
          amount: { type: 'number', exclusiveMinimum: 0, description: 'ATTN to stake' },
          minutes: { type: 'number', exclusiveMinimum: 0, description: 'Session length in minutes' },
        },
        required: ['amount', 'minutes'],
        additionalProperties: false,
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'unstake',
      description: 'Unstake ATTN to end the current focus session.',
      parameters: { type: 'object', properties: {}, additionalProperties: false },
    },
  },
];

export interface ToolResponse {
  tool: 'stake' | 'unstake' | null;
  args: Record<string, unknown> | null;
  result: object | null;
}

const isPositive = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0;

@Injectable()
export class ToolService {
  private readonly logger = new Logger(ToolService.name);
  private client?: OpenAI;

  constructor(private readonly stakeService: StakeService) {}

  // Created lazily so a missing OG_* var fails the request, not the server boot.
  // 10s timeout and no retries, so a 0G outage fails fast with a 503.
  private og(): OpenAI {
    return (this.client ??= new OpenAI({
      baseURL: env('OG_BASE_URL'),
      apiKey: env('OG_API_KEY'),
      timeout: 10_000,
      maxRetries: 0,
    }));
  }

  async decide(userText: string, aiRes: string): Promise<ToolResponse> {
    const res = await this.callOg(userText, aiRes);
    const reqId = (res as any).x_0g_trace?.request_id ?? res.id;

    // Only the first tool call is used; any others are ignored.
    const call = res.choices[0]?.message?.tool_calls?.[0];
    if (!call || call.type !== 'function') {
      this.logger.log(`[0g ${reqId}] no tool`);
      return { tool: null, args: null, result: null };
    }

    const name = call.function.name;
    if (name !== 'stake' && name !== 'unstake') {
      this.logger.warn(`[0g ${reqId}] model called unknown tool "${name}", ignoring`);
      return { tool: null, args: null, result: null };
    }

    let args: Record<string, unknown> | null = null;
    try {
      const parsed = JSON.parse(call.function.arguments || '{}');
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) args = parsed;
    } catch {
      // args stays null → invalid_args below
    }

    const valid = name === 'stake' ? !!args && isPositive(args.amount) && isPositive(args.minutes) : !!args;
    if (!valid) {
      this.logger.warn(`[0g ${reqId}] ${name} invalid args: ${call.function.arguments}`);
      return { tool: name, args, result: { ok: false, error: 'invalid_args' } };
    }

    this.logger.log(`[0g ${reqId}] ${name} ${JSON.stringify(args)}`);
    // StakeService still uses a fixed 1 ATTN; amount/minutes are not applied on-chain yet.
    const result = name === 'stake' ? await this.stakeService.stake() : await this.stakeService.unstake();
    return { tool: name, args, result };
  }

  private async callOg(userText: string, aiRes: string): Promise<OpenAI.Chat.Completions.ChatCompletion> {
    try {
      return await this.og().chat.completions.create({
        model: env('OG_CHAT_MODEL'),
        tool_choice: 'auto',
        tools: TOOLS,
        verify_tee: true, // 0G extension
        messages: [
          { role: 'system', content: SYSTEM_PROMPT },
          { role: 'user', content: userText },
          { role: 'assistant', content: aiRes },
        ],
      } as any);
    } catch (err) {
      // Never guess a tool decision when 0G fails: surface the error instead.
      if (err instanceof APIConnectionError) {
        // Includes APIConnectionTimeoutError (the 10s timeout).
        this.logger.error(`0G unreachable: ${err.message}`);
        throw new HttpException({ error: 'og_unavailable' }, 503);
      }
      if (err instanceof BadRequestError) {
        this.logger.error(`0G returned 400, model doesn't support tool calling: ${err.message}`);
        throw new HttpException({ error: 'og_bad_request' }, 502);
      }
      if (err instanceof APIError && (err.status ?? 0) >= 500) {
        this.logger.error(`0G server error ${err.status}: ${err.message}`);
        throw new HttpException({ error: 'og_unavailable' }, 503);
      }
      if (err instanceof APIError) {
        this.logger.error(`0G error ${err.status}: ${err.message}`);
        throw new HttpException({ error: 'og_error' }, 502);
      }
      throw err;
    }
  }
}
