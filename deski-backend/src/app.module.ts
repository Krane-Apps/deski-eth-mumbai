import { Module } from '@nestjs/common';
import { AppController } from './app.controller';
import { EnsService } from './ens/ens.service';
import { HederaService } from './hedera/hedera.service';
import { StakeService } from './stake/stake.service';
import { StateService } from './state/state.service';
import { StorageService } from './storage/storage.service';
import { ToolController } from './tool/tool.controller';
import { ToolService } from './tool/tool.service';

@Module({
  controllers: [AppController, ToolController],
  providers: [EnsService, HederaService, StakeService, StateService, StorageService, ToolService],
})
export class AppModule {}
