import { Injectable, Logger } from '@nestjs/common';
import { env, HederaService, hederaStatus } from '../hedera/hedera.service';

const USDC_AMOUNT = 1;
const ATTN_AMOUNT = 1;

export interface StakeResult {
  success: boolean;
  usdcStaked: number;
  attnReceived: number;
}

export interface UnstakeResult {
  success: boolean;
  usdcReturned: number;
  attnRemoved: number;
}

@Injectable()
export class StakeService {
  private readonly logger = new Logger(StakeService.name);

  constructor(private readonly hedera: HederaService) {}

  /** USDC test → admin, mint ATTN to test, freeze test. Amounts report what actually moved. */
  stake(): Promise<StakeResult> {
    return this.hedera.exclusive(async () => {
      const { adminId, testId } = this.hedera;
      const result: StakeResult = { success: false, usdcStaked: 0, attnReceived: 0 };
      let step = 'transferUsdc';
      try {
        const usdcTx = await this.hedera.transferUsdc(testId, env('TEST_PRIVATE_KEY'), adminId, USDC_AMOUNT);
        result.usdcStaked = USDC_AMOUNT;
        this.logTx(step, usdcTx);

        step = 'mintAttn';
        const mintTx = await this.hedera.mintAttn(testId, ATTN_AMOUNT);
        result.attnReceived = ATTN_AMOUNT;
        this.logTx(step, mintTx);

        step = 'freeze';
        this.logTx(step, await this.hedera.freeze(testId));

        result.success = true;
      } catch (err: any) {
        this.logFailure('stake', step, err);
      }
      return result;
    });
  }

  /** Unfreeze test, wipe ATTN from test, USDC admin → test. Amounts report what actually moved. */
  unstake(): Promise<UnstakeResult> {
    return this.hedera.exclusive(async () => {
      const { adminId, testId } = this.hedera;
      const result: UnstakeResult = { success: false, usdcReturned: 0, attnRemoved: 0 };
      let step = 'unfreeze';
      try {
        this.logTx(step, await this.hedera.unfreeze(testId));

        step = 'wipeAttn';
        const wipeTx = await this.hedera.wipeAttn(testId, ATTN_AMOUNT);
        result.attnRemoved = ATTN_AMOUNT;
        this.logTx(step, wipeTx);

        step = 'returnUsdc';
        const usdcTx = await this.hedera.transferUsdc(adminId, env('MY_PRIVATE_KEY'), testId, USDC_AMOUNT);
        result.usdcReturned = USDC_AMOUNT;
        this.logTx(step, usdcTx);

        result.success = true;
      } catch (err: any) {
        this.logFailure('unstake', step, err);
      }
      return result;
    });
  }

  private logFailure(action: string, step: string, err: any) {
    const status = hederaStatus(err);
    this.logger.error(
      `${action} failed at ${step}${status ? ` [${status}]` : ''}: ${(err?.message ?? String(err)).split('\n')[0]}`,
    );
  }

  private logTx(step: string, id: string) {
    this.logger.log(`${step}: https://hashscan.io/testnet/transaction/${id}`);
  }
}
