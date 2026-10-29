// Cloudflare Worker + one SQLite-backed Durable Object holding the lobby (README.md). WebSockets use the Hibernation
// API: "ping" is answered by the runtime without waking the object, and every socket's state is its attachment.
import { DurableObject } from 'cloudflare:workers';

import { Lobby, LIMITS, PROTOCOL, listReports } from './lobby.js';

class WorkerConn {
  constructor(ws) {
    this.ws = ws;
  }

  attachment() {
    return this.ws.deserializeAttachment() ?? null;
  }

  setAttachment(value) {
    try {
      this.ws.serializeAttachment(value);
    } catch {
      // A socket that already closed keeps no state.
    }
  }

  send(text) {
    this.ws.send(text);
  }

  close(code, reason) {
    this.ws.close(code, reason);
  }
}

export class LobbyObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
    const sql = ctx.storage.sql;
    this.lobby = new Lobby({
      sql: {
        all: (query, ...args) => sql.exec(query, ...args).toArray(),
        get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
        run: (query, ...args) => {
          sql.exec(query, ...args);
        },
      },
      sockets: () => ctx.getWebSockets().map((ws) => new WorkerConn(ws)),
      now: () => Date.now(),
      setAlarm: (ms) => ctx.storage.setAlarm(ms),
      log: (text) => console.log(text),
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/reports') {
      return Response.json(listReports(this.lobby.host.sql, 100), { headers: { 'X-Content-Type-Options': 'nosniff' } });
    }
    const ip = request.headers.get('CF-Connecting-IP') ?? '';
    const first = this.lobby.admit(ip);
    if (!first) return new Response('Too many connections from this address', { status: 429 });
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(first);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (typeof message !== 'string' || message.length > LIMITS.reportFrameBytes) {
      ws.close(1009, 'text frames up to the size limit only');
      return;
    }
    this.lobby.message(new WorkerConn(ws), message);
  }

  webSocketClose(ws, code) {
    this.lobby.closed(new WorkerConn(ws));
    try {
      ws.close(code === 1005 ? 1000 : code, 'closed');
    } catch {
      // Already closed.
    }
  }

  webSocketError(ws) {
    this.lobby.closed(new WorkerConn(ws));
  }

  alarm() {
    this.lobby.alarm();
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const stub = env.LOBBY.get(env.LOBBY.idFromName('global'));
    if (url.pathname === '/reports') {
      const token = env.REPORTS_TOKEN;
      if (!token || request.headers.get('Authorization') !== `Bearer ${token}`) {
        return new Response('Not found', { status: 404 });
      }
      return stub.fetch(request);
    }
    if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') return stub.fetch(request);
    return Response.json({ service: 'dcp-lobby', proto: PROTOCOL }, { headers: { 'X-Content-Type-Options': 'nosniff' } });
  },
};
