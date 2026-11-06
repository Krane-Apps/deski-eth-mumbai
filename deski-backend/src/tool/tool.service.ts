import { HttpException, Injectable, Logger } from '@nestjs/common';
import OpenAI, { APIConnectionError, APIError, BadRequestError } from 'openai';
import { EnsService } from '../ens/ens.service';
import { env } from '../hedera/hedera.service';
import { StakeService } from '../stake/stake.service';
import { StateService } from '../state/state.service';

const SYSTEM_PROMPT =
  'You decide if Deski should stake or unstake ATTN based on the conversation. ' +
  'Call `stake` only when the user clearly asks to start a focus attention session or stake. ' +
  'Call `unstake` only when they clearly ask to stop or end it. ' +
  'Call `ens` when the user asks to look up or find an ENS name; pass the name they said. ' +
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
  {
    type: 'function',
    function: {
      name: 'ens',
      description: 'Look up an ENS name (e.g. vitalik or vitalik.eth).',
      parameters: {
        type: 'object',
        properties: { name: { type: 'string', description: 'The ENS name the user asked about' } },
        required: ['name'],
        additionalProperties: false,
      },
    },
  },
];

// Image turns: user_text is "img" and ai_res is the vision AI's description of the camera frame.
const ATTENTION_PROMPT =
  'You receive a description of a camera frame of the user at their desk. ' +
  'Decide whether the user is paying attention to their work. Always call `attention`.';

const ATTENTION_TOOL: OpenAI.Chat.Completions.ChatCompletionTool = {
  type: 'function',
  function: {
    name: 'attention',
    description: 'Report whether the user is paying attention to their work.',
    parameters: {
      type: 'object',
      properties: {
        attentive: { type: 'boolean', description: 'true if the user is focused on their work' },
      },
      required: ['attentive'],
      additionalProperties: false,
    },
  },
};

type ToolName = 'stake' | 'unstake' | 'attention' | 'ens';

interface OgCall {
  system: string;
  tools: OpenAI.Chat.Completions.ChatCompletionTool[];
  toolChoice: unknown;
}

const TEXT_CALL: OgCall = { system: SYSTEM_PROMPT, tools: TOOLS, toolChoice: 'auto' };
const IMAGE_CALL: OgCall = {
  system: ATTENTION_PROMPT,
  tools: [ATTENTION_TOOL],
  toolChoice: { type: 'function', function: { name: 'attention' } },
};

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
  attention: { ok: () => 'Attention check passed.', fail: 'Attention check failed.' },
  // ens success message is the .limo link, set in lookupEns().
  ens: { ok: () => '', fail: 'no ens found' },
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

  constructor(
    private readonly stakeService: StakeService,
    private readonly state: StateService,
    private readonly ens: EnsService,
  ) {}

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
    if (userText.trim().toLowerCase() === 'img') return this.checkAttention(userText, aiRes);

    const res = await this.callOg(TEXT_CALL, userText, aiRes);
    const reqId = (res as any).x_0g_trace?.request_id ?? res.id;

    // Only the first tool call is used; any others are ignored.
    const call = res.choices[0]?.message?.tool_calls?.[0];
    if (!call || call.type !== 'function') {
      this.logger.log(`[0g ${reqId}] no tool`);
      return failed();
    }

    const name = call.function.name;
    if (name !== 'stake' && name !== 'unstake' && name !== 'ens') {
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

    const valid =
      name === 'stake'
        ? !!args && isPositive(args.amount) && isPositive(args.minutes)
        : name === 'ens'
          ? !!args && typeof args.name === 'string' && args.name.trim().length > 0
          : !!args;
    if (!valid) {
      this.logger.warn(`[0g ${reqId}] ${name} invalid args: ${call.function.arguments}`);
      return respond(name);
    }

    this.logger.log(`[0g ${reqId}] ${name} ${JSON.stringify(args)}`);
    if (name === 'ens') return this.lookupEns(args!.name as string);

    // StakeService still uses a fixed 1 ATTN; amount/minutes are not applied on-chain yet.
    if (name === 'stake') {
      const r = await this.stakeService.stake();
      if (r.success) this.state.setStake(true);
      return respond(name, r.success, r.usdcStaked, r.attnReceived);
    }
    const r = await this.stakeService.unstake();
    if (r.success) {
      this.state.setStake(false);
      this.state.setExercise(false);
    }
    return respond(name, r.success, r.usdcReturned, r.attnRemoved);
  }

  /** ENS lookup: found → https://<name>.limo link; subgraph failure is reported separately from "not found". */
  private async lookupEns(name: string): Promise<ToolResponse> {
    const r = await this.ens.lookup(name);
    const out = respond('ens', r.status === 'found');
    if (r.status === 'found') out.message = `https://${r.name}.limo`;
    else if (r.status === 'error') out.message = 'ENS lookup failed, try again.';
    this.logger.log(`ens "${name}" → ${r.status}${r.status === 'found' ? ` ${r.name}` : ''}`);
    return out;
  }

  /**
   * Image turn: the model only gets the `attention` tool, so an image can never stake/unstake.
   * success = attentive. If the model gives no usable answer we return tool: null rather than
   * guessing "not attentive".
   */
  private async checkAttention(userText: string, aiRes: string): Promise<ToolResponse> {
    const res = await this.callOg(IMAGE_CALL, userText, aiRes);
    const reqId = (res as any).x_0g_trace?.request_id ?? res.id;

    const call = res.choices[0]?.message?.tool_calls?.[0];
    let attentive: unknown;
    if (call?.type === 'function' && call.function.name === 'attention') {
      try {
        attentive = JSON.parse(call.function.arguments || '{}')?.attentive;
      } catch {
        // attentive stays undefined
      }
    }
    if (typeof attentive !== 'boolean') {
      this.logger.warn(`[0g ${reqId}] attention: no usable answer (${call?.type === 'function' ? call.function.arguments : 'no tool call'})`);
      return failed();
    }

    this.logger.log(`[0g ${reqId}] attention attentive=${attentive}`);
    if (attentive) return respond('attention', true);

    // Lost attention: first warn with an exercise, then slash if it happens again.
    const out = respond('attention', false);
    const { stake, exercise } = this.state.get();
    if (!exercise || !stake) {
      // Nothing staked means nothing to slash, so keep warning.
      this.state.setExercise(true);
      out.message = 'Need more focus, do neck exercise.';
    } else {
      // Slash = unfreeze + wipe ATTN. That ends the session, so reset the state.
      const r = await this.stakeService.slash();
      out.attnReceived = r.attnRemoved;
      if (r.success) {
        this.state.setStake(false);
        this.state.setExercise(false);
        out.message = `We have slashed your stake. ${r.attnRemoved} ATTN removed.`;
      } else {
        out.message = 'Could not slash your stake.';
      }
    }
    this.logger.log(`attention lost: ${out.message} state=${JSON.stringify(this.state.get())}`);
    return out;
  }

  private async callOg(call: OgCall, userText: string, aiRes: string): Promise<OpenAI.Chat.Completions.ChatCompletion> {
    try {
      return await this.og().chat.completions.create({
        model: env('OG_CHAT_MODEL'),
        tool_choice: call.toolChoice,
        tools: call.tools,
        verify_tee: true, // 0G extension
        messages: [
          { role: 'system', content: call.system },
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
