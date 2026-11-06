import { Injectable, Logger } from '@nestjs/common';
import { env } from '../hedera/hedera.service';

export type EnsLookup =
  | { status: 'found'; name: string }
  | { status: 'not_found' }
  | { status: 'error' };

// Exact match is cheap (~0.4s); the starts_with search is slower, so it only runs when needed.
const EXACT_QUERY = `query ($name: String!) { domains(where: { name: $name }, first: 1) { name } }`;
const SIMILAR_QUERY = `query ($label: String!) {
  domains(where: { labelName_starts_with: $label, parent_: { name: "eth" } }, first: 10) { name }
}`;

class SubgraphError extends Error {}

@Injectable()
export class EnsService {
  private readonly logger = new Logger(EnsService.name);

  /** Finds `name` (or the closest similar .eth name) on the ENS subgraph. */
  async lookup(raw: string): Promise<EnsLookup> {
    const label = raw.trim().toLowerCase().replace(/\.eth$/, '');
    // A single ENS label: no whitespace, no dots, not empty.
    if (!label || /[\s.]/.test(label)) return { status: 'not_found' };

    try {
      const exact = await this.query(EXACT_QUERY, { name: `${label}.eth` });
      if (exact[0]?.name) return { status: 'found', name: exact[0].name };

      // Closest match = shortest name that starts with the label.
      const similar: string[] = (await this.query(SIMILAR_QUERY, { label }))
        .map((d: { name?: string }) => d.name)
        .filter((n: unknown): n is string => typeof n === 'string' && n.endsWith('.eth'))
        .sort((a: string, b: string) => a.length - b.length);
      return similar[0] ? { status: 'found', name: similar[0] } : { status: 'not_found' };
    } catch (err: any) {
      // Subgraph error, timeout, DNS failure, or a missing ENS_SUBGRAPH_URL.
      this.logger.error(`ENS lookup failed: ${err?.message ?? err}`);
      return { status: 'error' };
    }
  }

  /** POSTs one GraphQL query (8s timeout) and returns `data.domains`. */
  private async query(query: string, variables: Record<string, string>): Promise<{ name?: string }[]> {
    const res = await fetch(env('ENS_SUBGRAPH_URL'), {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ query, variables }),
      signal: AbortSignal.timeout(8_000),
    });
    const json: any = await res.json().catch(() => null);
    if (!res.ok || !json?.data || json.errors) {
      throw new SubgraphError(`subgraph ${res.status}: ${JSON.stringify(json?.errors ?? json)?.slice(0, 300)}`);
    }
    return json.data.domains ?? [];
  }
}
