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

type ToolName = 'stake' | 'unstake';

/**
 * Same shape for every response (see docs/tool-api.html). For unstake, the amounts are
 * what was returned/removed. `message` is only present when a tool was called.
 */
export interface ToolResponse {
  tool: ToolName | null;
  success: boolean;
  usdcStaked: number;
  attnReceived: number;
  message?: string;
}

const MESSAGES: Record<ToolName, { ok: (attn: number) => string; fail: string }> = {
  stake: { ok: (attn) => `Staked ${attn} ATTN. Focus session started.`, fail: 'Could not start the focus session.' },
  unstake: { ok: (attn) => `Unstaked ${attn} ATTN. Focus session ended.`, fail: 'Could not end the focus session.' },
};

/** Builds a response; adds `message` whenever a tool was called. */
function respond(tool: ToolName | null, success = false, usdcStaked = 0, attnReceived = 0): ToolResponse {
  const res: ToolResponse = { tool, success, usdcStaked, attnReceived };
  if (tool) res.message = success ? MESSAGES[tool].ok(attnReceived) : MESSAGES[tool].fail;
  return res;
}

/** No tool ran (or the request failed before a decision): no message. */
export const failed = (): ToolResponse => respond(null);

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
      return failed();
    }

    const name = call.function.name;
    if (name !== 'stake' && name !== 'unstake') {
      this.logger.warn(`[0g ${reqId}] model called unknown tool "${name}", ignoring`);
      return failed();
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
      return respond(name);
    }

    this.logger.log(`[0g ${reqId}] ${name} ${JSON.stringify(args)}`);
    // StakeService still uses a fixed 1 ATTN; amount/minutes are not applied on-chain yet.
    if (name === 'stake') {
      const r = await this.stakeService.stake();
      return respond(name, r.success, r.usdcStaked, r.attnReceived);
    }
    const r = await this.stakeService.unstake();
    return respond(name, r.success, r.usdcReturned, r.attnRemoved);
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
        throw new HttpException(failed(), 503);
      }
      if (err instanceof BadRequestError) {
        this.logger.error(`0G returned 400, model doesn't support tool calling: ${err.message}`);
        throw new HttpException(failed(), 502);
      }
      if (err instanceof APIError && (err.status ?? 0) >= 500) {
        this.logger.error(`0G server error ${err.status}: ${err.message}`);
        throw new HttpException(failed(), 503);
      }
      if (err instanceof APIError) {
        this.logger.error(`0G error ${err.status}: ${err.message}`);
        throw new HttpException(failed(), 502);
      }
      // e.g. a missing OG_* env var
      this.logger.error(`0G call failed: ${(err as Error)?.message ?? err}`);
      throw new HttpException(failed(), 500);
    }
  }
}
