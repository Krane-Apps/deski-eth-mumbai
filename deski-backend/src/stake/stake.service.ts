import { Injectable, Logger } from '@nestjs/common';
import { env, HederaService, hederaStatus } from '../hedera/hedera.service';

const USDC_AMOUNT = 1;
const ATTN_AMOUNT = 1;

export interface StakeResult {
  success: boolean;
  usdcStaked: number;
  attnReceived: number;
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
        const status = hederaStatus(err);
        this.logger.error(
          `stake failed at ${step}${status ? ` [${status}]` : ''}: ${(err?.message ?? String(err)).split('\n')[0]}`,
        );
      }
      return result;
    });
  }

  private logTx(step: string, id: string) {
    this.logger.log(`${step}: https://hashscan.io/testnet/transaction/${id}`);
  }
}
