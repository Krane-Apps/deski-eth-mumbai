import { Injectable, Logger, OnModuleDestroy, OnModuleInit } from '@nestjs/common';
import {
  Network,
  StableCoin,
  InitializationRequest,
  ConnectRequest,
  SupportedWallets,
  CashInRequest,
  FreezeAccountRequest,
} from '@hashgraph/stablecoin-npm-sdk';
import { Client, PrivateKey, TokenId, AccountId, TransferTransaction, Status } from '@hiero-ledger/sdk';

export const USDC_DECIMALS = 6;

export function env(name: string): string {
  const v = process.env[name];
  if (!v) throw new Error(`Missing env var ${name}`);
  return v;
}

// Pull a Hedera status code (e.g. ACCOUNT_KYC_NOT_GRANTED_FOR_TOKEN) out of nested SDK errors.
// Ported from scripts/attn.ts.
export function hederaStatus(err: any): string | undefined {
  const seen = new Set<any>();
  const stack = [err];
  while (stack.length) {
    const e = stack.pop();
    if (!e || typeof e !== 'object' || seen.has(e)) continue;
    seen.add(e);
    if (typeof e.status === 'string') return e.status;
    if (e.status?._code !== undefined && typeof e.status.toString === 'function') return e.status.toString();
    for (const k of ['message', 'reason', 'description']) {
      if (typeof e[k] !== 'string') continue;
      const m =
        e[k].match(/status"?\s*[:=]?\s*"?([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)\b/) ??
        e[k].match(/\b([A-Z][A-Z0-9]*(?:_[A-Z0-9]+){2,})\b/);
      if (m) return m[1];
    }
    stack.push(e.error, e.cause, e.transactionError, e.errorMessage);
  }
  return undefined;
}

/** Returns the transaction id; throws if the SDK reports failure. */
function txId(label: string, res: { success: boolean; transactionId?: string }): string {
  if (!res.success) throw new Error(`${label} returned success=false (tx ${res.transactionId ?? 'n/a'})`);
  return res.transactionId ?? 'n/a';
}

@Injectable()
export class HederaService implements OnModuleInit, OnModuleDestroy {
  private readonly logger = new Logger(HederaService.name);
  private readonly tokenId = env('ATTN_TOKEN_ID');
  private client!: Client;
  private queue: Promise<unknown> = Promise.resolve();

  readonly adminId = env('MY_ACCOUNT_ID');
  readonly testId = env('TEST_ACCOUNT_ID');

  async onModuleInit() {
    const network = env('HEDERA_NETWORK');
    const mirrorNode = { name: 'mirror', baseUrl: env('HEDERA_MIRROR_NODE') };
    const rpcNode = { name: 'rpc', baseUrl: env('HEDERA_RPC_NODE') };

    await Network.init(
      new InitializationRequest({
        network,
        mirrorNode,
        rpcNode,
        configuration: {
          factoryAddress: env('FACTORY_ADDRESS'),
          resolverAddress: env('RESOLVER_ADDRESS'),
        },
      }),
    );

    const connect = (key: string) =>
      Network.connect(
        new ConnectRequest({
          account: { accountId: this.adminId, privateKey: { key, type: env('MY_PRIVATE_KEY_TYPE') } },
          network,
          mirrorNode,
          rpcNode,
          wallet: SupportedWallets.CLIENT,
        }),
      );
    const key = env('MY_PRIVATE_KEY');
    try {
      await connect(key);
    } catch (e) {
      if (!key.startsWith('0x')) throw e;
      this.logger.warn('Key parse failed, retrying without 0x prefix...');
      await connect(key.slice(2));
    }

    // Plain Hedera client for USDC transfers (the stablecoin SDK only handles its own token).
    // The operator is the admin; each transfer is also signed by the sender's key.
    this.client = Client.forName(network);
    this.client.setOperator(this.adminId, this.parseKey(key));

    this.logger.log(`Connected as admin ${this.adminId} on ${network}`);
  }

  onModuleDestroy() {
    this.client?.close();
  }

  /** Runs jobs one at a time so two requests can't interleave their steps. */
  exclusive<T>(job: () => Promise<T>): Promise<T> {
    const run = this.queue.then(job, job);
    this.queue = run.catch(() => undefined);
    return run;
  }

  parseKey(raw: string): PrivateKey {
    const hex = raw.replace(/^0x/, '');
    try {
      return env('MY_PRIVATE_KEY_TYPE').toUpperCase() === 'ED25519'
        ? PrivateKey.fromStringED25519(hex)
        : PrivateKey.fromStringECDSA(hex);
    } catch {
      return PrivateKey.fromStringDer(hex);
    }
  }

  /** Transfers whole USDC (e.g. 1) from `fromId` to `toId`, signed by `fromKey`. Returns the tx id. */
  async transferUsdc(fromId: string, fromKey: string, toId: string, usdc: number): Promise<string> {
    const units = usdc * 10 ** USDC_DECIMALS;
    const tokenId = TokenId.fromString(env('USDC_TOKEN_ID'));
    const tx = await new TransferTransaction()
      .addTokenTransfer(tokenId, AccountId.fromString(fromId), -units)
      .addTokenTransfer(tokenId, AccountId.fromString(toId), units)
      .freezeWith(this.client)
      .sign(this.parseKey(fromKey));
    const res = await tx.execute(this.client);
    const receipt = await res.getReceipt(this.client); // throws on a non-SUCCESS status
    if (receipt.status !== Status.Success) throw new Error(`USDC transfer status ${receipt.status}`);
    return res.transactionId.toString();
  }

  async mintAttn(targetId: string, amount: number): Promise<string> {
    return txId(
      'cashIn',
      await StableCoin.cashIn(new CashInRequest({ targetId, tokenId: this.tokenId, amount: String(amount) })),
    );
  }

  async freeze(targetId: string): Promise<string> {
    return txId('freeze', await StableCoin.freeze(new FreezeAccountRequest({ targetId, tokenId: this.tokenId })));
  }
}
