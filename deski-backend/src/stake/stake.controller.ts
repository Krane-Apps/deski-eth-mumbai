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
      case 'unstake':
        throw new HttpException({ success: false, message: 'unstake is not implemented yet' }, 400);
      default:
        throw new HttpException({ success: false, message: "action must be 'stake' or 'unstake'" }, 400);
    }
  }
}
