// episode log for one focus session: every 0G decision with its request id, saved to 0G Storage when the session ends
import { Injectable, Logger } from '@nestjs/common';
import { Indexer, MemData } from '@0gfoundation/0g-storage-ts-sdk';
import { ethers } from 'ethers';

export interface EpisodeEvent {
  at: string;
  decision: string; // e.g. "stake", "attention attentive=false", "none"
  requestId: string; // 0G x_0g_trace.request_id
  provider?: string; // 0G x_0g_trace.provider
  userText: string;
  aiRes: string; // for image turns: what the 0G vision model saw
  result?: string; // what Deski did with the decision
}

const RPC = process.env.OG_STORAGE_RPC ?? 'https://evmrpc-testnet.0g.ai';
const INDEXER = process.env.OG_STORAGE_INDEXER ?? 'https://indexer-storage-testnet-turbo.0g.ai';

@Injectable()
export class EpisodeService {
  private readonly logger = new Logger(EpisodeService.name);
  private startedAt = '';
  private events: EpisodeEvent[] = [];

  start() {
    this.startedAt = new Date().toISOString();
    this.events = [];
  }

  /** Only decisions inside a session are kept. */
  record(e: Omit<EpisodeEvent, 'at'>) {
    if (this.startedAt) this.events.push({ at: new Date().toISOString(), ...e });
  }

  /**
   * Ends the session and returns the log's 0G Storage root hash right away.
   * The upload waits for chain finality, so it runs in the background and logs its tx hash or error.
   * Never throws: a storage problem must not block the unstake or slash response.
   */
  async finish(outcome: string): Promise<string | undefined> {
    if (!this.startedAt) return undefined;
    const episode = { app: 'deski', startedAt: this.startedAt, endedAt: new Date().toISOString(), outcome, events: this.events };
    this.startedAt = '';
    this.events = [];
    try {
      const data = new MemData(new TextEncoder().encode(JSON.stringify(episode, null, 2)));
      const [tree, treeErr] = await data.merkleTree();
      const root = tree?.rootHash();
      if (treeErr || !root) throw treeErr ?? new Error('no root hash');
      const key = process.env.OG_STORAGE_KEY;
      if (!key) {
        this.logger.warn(`episode ${root} not uploaded: set OG_STORAGE_KEY. ${JSON.stringify(episode)}`);
        return undefined;
      }
      this.logger.log(`episode (${episode.events.length} decisions, ${outcome}) root ${root}, uploading to 0G Storage`);
      const signer = new ethers.Wallet(key, new ethers.JsonRpcProvider(RPC));
      new Indexer(INDEXER)
        .upload(data, RPC, signer)
        .then(([tx, err]) => {
          if (err) this.logger.error(`episode ${root} upload FAILED: ${err.message.slice(0, 160)}`);
          else this.logger.log(`episode ${root} saved on 0G Storage, tx ${'txHash' in tx ? tx.txHash : tx.txHashes.join(',')}`);
        })
        .catch((err) => this.logger.error(`episode ${root} upload FAILED: ${String(err?.message ?? err).slice(0, 160)}`));
      return root;
    } catch (err) {
      this.logger.error(`episode log failed: ${(err as Error)?.message ?? err}`);
      return undefined;
    }
  }
}
