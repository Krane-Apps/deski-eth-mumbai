import { Body, Controller, HttpCode, HttpException, Post } from '@nestjs/common';
import { failed, ToolService } from './tool.service';

const nonEmpty = (v: unknown): v is string => typeof v === 'string' && v.trim().length > 0;

@Controller('tool')
export class ToolController {
  constructor(private readonly toolService: ToolService) {}

  @Post()
  @HttpCode(200)
  handle(@Body() body: { user_text?: unknown; ai_res?: unknown }) {
    if (!nonEmpty(body?.user_text) || !nonEmpty(body?.ai_res)) {
      throw new HttpException(failed(), 400);
    }
    return this.toolService.decide(body.user_text, body.ai_res);
  }
}
