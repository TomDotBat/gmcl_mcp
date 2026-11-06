// waitFor.ts — gmod_wait_for: block until a game condition holds or a timeout.
//
// Polling lives here in the server, not in the game: each poll is an ordinary
// IPC request, so a wait survives map loads (the client Lua state is recreated
// and requests fail for a while) — failed polls are recorded and retried.

import { GmodIpc, Envelope } from "./ipc.js";

export interface WaitArgs {
  lua?: string;
  console?: string;
  hook?: string;
  luaError?: boolean;
  inGame?: boolean;
  timeoutMs?: number;
  pollMs?: number;
}

export interface WaitResult {
  matched: boolean;
  condition: string;
  elapsedMs: number;
  polls: number;
  timedOut?: boolean;
  detail?: unknown;
  lastError?: string;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// One poll's outcome: matched (with detail), not yet, or a transient error.
type Poll = { done: true; detail: unknown } | { done: false; error?: string };

// A condition: an optional setup (baseline) and a poll step.
interface Condition {
  name: string;
  setup?: () => Promise<void>;
  poll: () => Promise<Poll>;
}

export function validateWaitArgs(a: WaitArgs): string | null {
  const given = [a.lua, a.console, a.hook, a.luaError || undefined, a.inGame || undefined].filter(
    (v) => v !== undefined && v !== ""
  );
  if (given.length !== 1) return "give exactly one condition: lua, console, hook, luaError or inGame";
  if (a.console !== undefined) {
    try { new RegExp(a.console, "i"); } catch (e: any) { return `invalid console regex: ${e?.message ?? e}`; }
  }
  return null;
}

export async function waitFor(ipc: GmodIpc, a: WaitArgs): Promise<WaitResult> {
  const timeoutMs = Math.min(Math.max(a.timeoutMs ?? 10000, 0), 120000);
  const pollMs = Math.min(Math.max(a.pollMs ?? 100, 20), 5000);
  const start = Date.now();
  const deadline = start + timeoutMs;
  const remaining = () => Math.max(1, deadline - Date.now());

  // Per-request timeout: short, so a stalled main thread (map load) doesn't
  // swallow the whole budget in one request.
  const req = (method: string, params: Record<string, unknown> = {}): Promise<Envelope> =>
    ipc.request(method, params, Math.min(3000, remaining()));

  // Unwraps an envelope whose Lua result may itself carry an `error`.
  const unwrap = (env: Envelope): { result?: any; error?: string } => {
    if (!env.ok) return { error: env.error ?? "request failed" };
    const r = env.result ?? {};
    return r.error && Object.keys(r).length === 1 ? { error: String(r.error) } : { result: r };
  };

  const cond = buildCondition(a, req, unwrap);

  let polls = 0;
  let lastError: string | undefined;
  let setupDone = !cond.setup;

  for (;;) {
    try {
      if (!setupDone) { await cond.setup!(); setupDone = true; }
      polls++;
      const p = await cond.poll();
      if (p.done) {
        return { matched: true, condition: cond.name, elapsedMs: Date.now() - start, polls, detail: p.detail };
      }
      if (p.error) lastError = p.error;
    } catch (e: any) {
      const msg = String(e?.message ?? e);
      // A request cut short by the wait's own deadline isn't a real failure;
      // keep the previous, meaningful lastError instead.
      const deadlineCut = /timed out/.test(msg) && deadline - Date.now() <= 10;
      if (!deadlineCut) lastError = msg;
    }
    // Stop rather than start a poll with almost no budget (one is served per
    // rendered frame, so tiny budgets just time out).
    if (deadline - Date.now() < 50) {
      return { matched: false, timedOut: true, condition: cond.name, elapsedMs: Date.now() - start, polls, lastError };
    }
    await sleep(Math.min(pollMs, Math.max(0, remaining() - 50)));
  }
}

function buildCondition(
  a: WaitArgs,
  req: (method: string, params?: Record<string, unknown>) => Promise<Envelope>,
  unwrap: (env: Envelope) => { result?: any; error?: string }
): Condition {
  if (a.lua !== undefined) {
    return {
      name: `lua: ${a.lua}`,
      poll: async () => {
        const { result, error } = unwrap(await req("eval_condition", { expr: a.lua }));
        if (error) return { done: false, error };
        if (result.truthy) return { done: true, detail: { value: result.value } };
        return { done: false, error: result.error };
      },
    };
  }

  if (a.console !== undefined) {
    const re = new RegExp(a.console, "i");
    let since = 0;
    return {
      name: `console: /${a.console}/i`,
      // Baseline: only lines printed after the wait started count.
      setup: async () => {
        const env = await req("console_log", { lines: 1 });
        if (!env.ok) throw new Error(env.error ?? "console_log failed");
        since = Math.max(0, (env.result?.nextId ?? 1) - 1);
      },
      poll: async () => {
        const env = await req("console_log", { sinceId: since, lines: 4000 });
        if (!env.ok) return { done: false, error: env.error };
        const lines: { id: number; severity: number; text: string }[] = env.result?.lines ?? [];
        for (const l of lines) {
          if (re.test(l.text)) return { done: true, detail: { line: l } };
          since = Math.max(since, l.id);
        }
        return { done: false };
      },
    };
  }

  if (a.hook !== undefined) {
    let epoch: string | undefined;
    let since = 0;
    return {
      name: `hook: ${a.hook}`,
      // The first events call registers the watch and returns the baseline seq.
      setup: async () => {
        const { result, error } = unwrap(await req("events", { hook: a.hook }));
        if (error) throw new Error(error);
        epoch = result.epoch;
        since = result.seq ?? 0;
      },
      poll: async () => {
        const { result, error } = unwrap(await req("events", { hook: a.hook, since, epoch }));
        if (error) return { done: false, error };
        if (result.epoch !== epoch) {
          // New Lua state (map load): the watch was just re-registered there and
          // seqs restarted, so count everything from its start.
          epoch = result.epoch;
          since = 0;
          return { done: false };
        }
        const evs: any[] = result.events ?? [];
        if (evs.length > 0) return { done: true, detail: { event: evs[0], more: evs.length - 1 } };
        since = result.seq ?? since;
        return { done: false };
      },
    };
  }

  if (a.luaError) {
    let epoch: string | undefined;
    let since = 0;
    return {
      name: "luaError",
      setup: async () => {
        const { result, error } = unwrap(await req("lua_errors", { limit: 1 }));
        if (error) throw new Error(error);
        epoch = result.epoch;
        since = result.nextSeq ?? 0;
      },
      poll: async () => {
        const { result, error } = unwrap(await req("lua_errors", { since, epoch, limit: 5 }));
        if (error) return { done: false, error };
        if (result.epoch !== epoch) { epoch = result.epoch; since = 0; }
        const errs: any[] = result.errors ?? [];
        if (errs.length > 0) return { done: true, detail: { errors: errs, epoch: result.epoch } };
        since = result.nextSeq ?? since;
        return { done: false };
      },
    };
  }

  // inGame: client realm, agent loaded, and a valid local player.
  return {
    name: "inGame",
    poll: async () => {
      const ping = await req("ping");
      if (!ping.ok) return { done: false, error: ping.error };
      const p = ping.result ?? {};
      if (p.realm !== "client" || !p.bootstrapped) return { done: false, error: `realm=${p.realm} bootstrapped=${p.bootstrapped}` };
      const { result, error } = unwrap(await req("get_state", { maxEntities: 0 }));
      if (error) return { done: false, error };
      if (result.loading || !result.player) return { done: false, error: "loading" };
      return { done: true, detail: { map: result.map, player: result.player?.name } };
    },
  };
}
