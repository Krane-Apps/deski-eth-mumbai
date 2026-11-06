import { Body, Controller, HttpCode, HttpException, Post } from '@nestjs/common';
import { StorageService } from '../storage/storage.service';
import { failed, ToolService } from './tool.service';

const nonEmpty = (v: unknown): v is string => typeof v === 'string' && v.trim().length > 0;

@Controller('tool')
export class ToolController {
  constructor(
    private readonly toolService: ToolService,
    private readonly storage: StorageService,
  ) {}

  @Post()
  @HttpCode(200)
  async handle(@Body() body: { user_text?: unknown; ai_res?: unknown }) {
    const time = new Date().toISOString();
    let status = 200;
    let response: unknown = failed();
    try {
      if (!nonEmpty(body?.user_text) || !nonEmpty(body?.ai_res)) {
        throw new HttpException(failed(), 400);
      }
      response = await this.toolService.decide(body.user_text, body.ai_res);
      return response;
    } catch (err) {
      if (err instanceof HttpException) {
        status = err.getStatus();
        response = err.getResponse();
      } else {
        status = 500;
      }
      throw err;
    } finally {
      // Every /tool call is logged to 0G Storage in the background; never delays the reply.
      this.storage.log({ time, user_text: body?.user_text, ai_res: body?.ai_res, status, response });
    }
  }
}
