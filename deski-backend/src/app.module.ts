import { Module } from '@nestjs/common';
import { AppController } from './app.controller';
import { HederaService } from './hedera/hedera.service';
import { StakeController } from './stake/stake.controller';
import { StakeService } from './stake/stake.service';

@Module({
  controllers: [AppController, StakeController],
  providers: [HederaService, StakeService],
})
export class AppModule {}
