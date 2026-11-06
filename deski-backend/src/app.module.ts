import { Module } from '@nestjs/common';
import { AppController } from './app.controller';
import { HederaService } from './hedera/hedera.service';
import { StakeService } from './stake/stake.service';
import { ToolController } from './tool/tool.controller';
import { ToolService } from './tool/tool.service';

@Module({
  controllers: [AppController, ToolController],
  providers: [HederaService, StakeService, ToolService],
})
export class AppModule {}
