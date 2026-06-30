// ipc.ts — named-pipe client for the injected core.dll.
//
// Framing matches the DLL: a 4-byte little-endian length prefix followed by a
// UTF-8 JSON body. Requests carry an incrementing id; responses are correlated
// back by id. Connection is lazy and auto-reconnects.

import net from "node:net";

const PIPE_PATH = "\\\\.\\pipe\\gmod_mcp";

export interface Envelope {
  id?: number;
  ok: boolean;
  result?: any;
  error?: string;
}

interface Pending {
  resolve: (env: Envelope) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

export class GmodIpc {
  private socket: net.Socket | null = null;
  private connecting: Promise<net.Socket> | null = null;
  private buffer = Buffer.alloc(0);
  private nextId = 1;
  private pending = new Map<number, Pending>();

  private connect(): Promise<net.Socket> {
    if (this.socket && !this.socket.destroyed) return Promise.resolve(this.socket);
    if (this.connecting) return this.connecting;

    this.connecting = new Promise<net.Socket>((resolve, reject) => {
      const sock = net.connect({ path: PIPE_PATH });

      sock.on("connect", () => {
        this.socket = sock;
        this.connecting = null;
        resolve(sock);
      });
      sock.on("data", (chunk) => this.onData(chunk));
      sock.on("error", (err) => {
        this.connecting = null;
        if (this.socket !== sock) reject(this.friendlyError(err));
        this.teardown(this.friendlyError(err));
      });
      sock.on("close", () => this.teardown(new Error("pipe closed")));
    });
    return this.connecting;
  }

  private friendlyError(err: NodeJS.ErrnoException): Error {
    if (err.code === "ENOENT" || err.code === "ECONNREFUSED") {
      return new Error(
        "Cannot reach the in-game DLL (pipe not found). Is Garry's Mod running and is core.dll injected? Try the gmod_inject tool."
      );
    }
    return err;
  }

  private teardown(err: Error) {
    if (this.socket) { this.socket.destroy(); this.socket = null; }
    this.buffer = Buffer.alloc(0);
    for (const [, p] of this.pending) { clearTimeout(p.timer); p.reject(err); }
    this.pending.clear();
  }

  private onData(chunk: Buffer) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      if (this.buffer.length < 4) return;
      const len = this.buffer.readUInt32LE(0);
      if (this.buffer.length < 4 + len) return;
      const body = this.buffer.subarray(4, 4 + len);
      this.buffer = this.buffer.subarray(4 + len);
      try {
        const env: Envelope = JSON.parse(body.toString("utf8"));
        const id = env.id ?? 0;
        const p = this.pending.get(id);
        if (p) { clearTimeout(p.timer); this.pending.delete(id); p.resolve(env); }
      } catch {
        /* ignore malformed frame */
      }
    }
  }

  async request(method: string, params: Record<string, unknown> = {}, timeoutMs = 15000): Promise<Envelope> {
    const sock = await this.connect();
    const id = this.nextId++;
    const payload = Buffer.from(JSON.stringify({ id, method, params }), "utf8");
    const frame = Buffer.alloc(4 + payload.length);
    frame.writeUInt32LE(payload.length, 0);
    payload.copy(frame, 4);

    return new Promise<Envelope>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`request '${method}' timed out after ${timeoutMs}ms`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      sock.write(frame, (err) => {
        if (err) {
          clearTimeout(timer);
          this.pending.delete(id);
          reject(err);
        }
      });
    });
  }

  /** Connectivity probe used by gmod_status; never throws. */
  async ping(): Promise<Envelope | null> {
    try {
      return await this.request("ping", {}, 3000);
    } catch {
      return null;
    }
  }
}
