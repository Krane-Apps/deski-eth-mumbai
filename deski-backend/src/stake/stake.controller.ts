import { Body, Controller, HttpCode, HttpException, Post } from '@nestjs/common';
import { StakeService } from './stake.service';

@Controller('stake')
export class StakeController {
  constructor(private readonly stakeService: StakeService) {}

  @Post()
  @HttpCode(200)
  async handle(@Body() body: { action?: string }) {
    switch (body?.action) {
      case 'stake': {
        const result = await this.stakeService.stake();
        if (!result.success) throw new HttpException(result, 500);
        return result;
      }
      case 'unstake': {
        const result = await this.stakeService.unstake();
        if (!result.success) throw new HttpException(result, 500);
        return result;
      }
      default:
        throw new HttpException({ success: false, message: "action must be 'stake' or 'unstake'" }, 400);
    }
  }
}
