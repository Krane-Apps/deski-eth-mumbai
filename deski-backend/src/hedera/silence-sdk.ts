// Must be the first import in main.ts: it has to run before the stablecoin SDK loads.
// Ported from scripts/attn.ts.
import * as path from 'path';
import { config } from 'dotenv';

// The repo-root .env is shared with scripts/attn.ts.
// __dirname is src/hedera or dist/hedera, so the root is three levels up.
config({ path: path.resolve(__dirname, '../../../.env') });

// The SDK has stray console.log calls, one of which (Network.connect) dumps the
// full account object INCLUDING the private key. So mute console.log/info/debug
// (Nest's Logger writes to stdout directly, so it still works) and, as a
// backstop, mask the keys in anything written to stdout/stderr.
console.log = console.info = console.debug = () => {};

const secrets = [process.env.MY_PRIVATE_KEY, process.env.TEST_PRIVATE_KEY, process.env.OG_API_KEY]
  .flatMap((k) => (k ? [k, k.replace(/^0x/, '')] : []))
  .filter((s) => s.length >= 16);

for (const stream of [process.stdout, process.stderr]) {
  const write = stream.write.bind(stream) as (...a: any[]) => boolean;
  stream.write = ((chunk: any, ...rest: any[]) => {
    if (typeof chunk === 'string' || Buffer.isBuffer(chunk)) {
      let s = chunk.toString();
      // The SDK's winston logger dumps huge JSON blobs; hide unless ATTN_DEBUG=1.
      if (!process.env.ATTN_DEBUG && s.startsWith('{"level":')) return true;
      for (const k of secrets) s = s.split(k).join('[REDACTED]');
      chunk = s;
    }
    return write(chunk, ...rest);
  }) as typeof stream.write;
}
