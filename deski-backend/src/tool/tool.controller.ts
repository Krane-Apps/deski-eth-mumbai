import { Body, Controller, HttpCode, HttpException, Logger, Post } from '@nestjs/common';
import { StorageService } from '../storage/storage.service';
import { failed, OgTrace, ToolService } from './tool.service';

const nonEmpty = (v: unknown): v is string => typeof v === 'string' && v.trim().length > 0;

@Controller('tool')
export class ToolController {
  private readonly logger = new Logger(ToolController.name);

  constructor(
    private readonly toolService: ToolService,
    private readonly storage: StorageService,
  ) {}

  @Post()
  @HttpCode(200)
  async handle(@Body() body: { user_text?: unknown; ai_res?: unknown; request_id?: unknown }) {
    const time = new Date().toISOString();
    this.logger.log(`/tool params: ${JSON.stringify(body)}`);
    let status = 200;
    let response: unknown = failed();
    // decision = the backend's own 0G call; stays null if 0G was never reached.
    const trace: OgTrace = { requestId: null };
    try {
      if (!nonEmpty(body?.user_text) || !nonEmpty(body?.ai_res)) {
        throw new HttpException(failed(), 400);
      }
      response = await this.toolService.decide(body.user_text, body.ai_res, trace);
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
      // client = the optional 0G request id the device sends (e.g. its vision call); not validated, null if unusable.
      const request_ids = {
        client: nonEmpty(body?.request_id) ? body.request_id.trim() : null,
        decision: trace.requestId,
      };
      this.storage.log({ time, request_ids, user_text: body?.user_text, ai_res: body?.ai_res, status, response });
    }
  }
}
