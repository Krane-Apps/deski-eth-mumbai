import { Injectable, Logger } from '@nestjs/common';
import { Indexer, MemData } from '@0gfoundation/0g-storage-ts-sdk';
import { JsonRpcProvider, Wallet } from 'ethers';

/**
 * Logs every /tool call to 0G Storage in the background.
 * Uploads run one at a time (one wallet → avoids nonce clashes) and never block or fail a request.
 * Disabled (with one warning) if OG_STORAGE_PRIVATE_KEY is not set.
 */
@Injectable()
export class StorageService {
  private readonly logger = new Logger(StorageService.name);
  private readonly rpc = process.env.OG_STORAGE_RPC || 'https://evmrpc-testnet.0g.ai';
  private readonly indexer = new Indexer(
    process.env.OG_STORAGE_INDEXER || 'https://indexer-storage-testnet-turbo.0g.ai',
  );
  private signer?: Wallet;
  private queue: Promise<void> = Promise.resolve();
  private warned = false;

  /** Fire-and-forget: queues the upload and returns immediately. */
  log(entry: Record<string, unknown>): void {
    const key = process.env.OG_STORAGE_PRIVATE_KEY;
    if (!key) {
      if (!this.warned) this.logger.warn('OG_STORAGE_PRIVATE_KEY not set, 0G Storage logging is off');
      this.warned = true;
      return;
    }
    this.signer ??= new Wallet(key, new JsonRpcProvider(this.rpc));
    this.queue = this.queue.then(() => this.upload(entry));
  }

  private async upload(entry: Record<string, unknown>): Promise<void> {
    try {
      const data = new MemData(new TextEncoder().encode(JSON.stringify(entry)));
      const [tx, err] = await this.indexer.upload(data, this.rpc, this.signer!);
      if (err) throw err;
      const rootHash = 'rootHash' in tx ? tx.rootHash : tx.rootHashes[0];
      const txHash = 'txHash' in tx ? tx.txHash : tx.txHashes[0];
      this.logger.log(`0G Storage log saved: root=${rootHash} tx=${txHash}`);
    } catch (err: any) {
      this.logger.error(`0G Storage upload failed: ${err?.message ?? err}`);
    }
  }
}
