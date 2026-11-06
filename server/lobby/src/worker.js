// Cloudflare Worker + one SQLite-backed Durable Object holding the lobby (README.md). WebSockets use the Hibernation
// API: "ping" is answered by the runtime without waking the object, and every socket's state is its attachment.
import { DurableObject } from 'cloudflare:workers';

import { COUNTRY_HEADER, handleAdmin, internalIncidentRoute, internalMetricsRoute, isAdminPath, limitedCount } from './admin.js';
import { handleUpgrade } from './edge.js';
import { INCIDENT } from './incidents.js';
import { Lobby, PROTOCOL } from './lobby.js';

class WorkerConn {
  constructor(ws, ctx) {
    this.ws = ws;
    this.ctx = ctx;
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

  sendBinary(bytes) {
    this.ws.send(bytes);
  }

  get bufferedAmount() {
    return this.ws.bufferedAmount;
  }

  // The runtime answers "ping" by itself and only remembers when.
  keepaliveAt() {
    return this.ctx.getWebSocketAutoResponseTimestamp(this.ws)?.getTime() ?? 0;
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
    // Every wake builds the lobby again; knowing the stored alarm keeps it from being written again each time.
    ctx.blockConcurrencyWhile(async () => {
      this.lobby = new Lobby({
        sql: {
          all: (query, ...args) => sql.exec(query, ...args).toArray(),
          get: (query, ...args) => sql.exec(query, ...args).toArray()[0],
          run: (query, ...args) => {
            sql.exec(query, ...args);
          },
          transaction: (work) => ctx.storage.transactionSync(work),
        },
        sockets: () => ctx.getWebSockets().map((ws) => new WorkerConn(ws, ctx)),
        now: () => Date.now(),
        setAlarm: (ms) => ctx.storage.setAlarm(ms),
        log: (text) => console.log(text),
      }, { alarmAt: await ctx.storage.getAlarm() });
    });
  }

  async fetch(request) {
    const incident = internalIncidentRoute(request);
    if (incident?.kind === 'list') return Response.json(this.lobby.incidentList());
    if (incident?.kind === 'one') {
      const found = this.lobby.incidentForAdmin(incident.ticket);
      return found ? Response.json(found) : new Response('Not found', { status: 404 });
    }
    if (incident?.kind === 'status') {
      const saved = this.lobby.setIncidentStatus(incident.ticket, await request.json());
      return saved ? Response.json(saved) : new Response('Not found', { status: 404 });
    }
    const metrics = internalMetricsRoute(request);
    if (metrics?.kind === 'summary') return Response.json(this.lobby.metricsSummary());
    if (metrics?.kind === 'history') return Response.json(this.lobby.metricsHistory(metrics.days));
    if (metrics) return new Response('Bad request', { status: 400 });
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return new Response('Not found', { status: 404 });
    const ip = request.headers.get('CF-Connecting-IP') ?? '';
    this.lobby.edgeLimited(limitedCount(request.headers));
    const first = this.lobby.admit(ip, request.headers.get(COUNTRY_HEADER));
    if (!first) return new Response('Too many connections from this address', { status: 429 });
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(first);
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, message) {
    if (message instanceof ArrayBuffer) {
      this.lobby.relay(new WorkerConn(ws, this.ctx), new Uint8Array(message));
      return;
    }
    if (typeof message !== 'string' || message.length > INCIDENT.frameChars) {
      ws.close(1009, 'text frames up to the size limit only');
      return;
    }
    this.lobby.message(new WorkerConn(ws, this.ctx), message);
  }

  webSocketClose(ws, code) {
    this.lobby.closed(new WorkerConn(ws, this.ctx));
    try {
      ws.close(code === 1005 ? 1000 : code, 'closed');
    } catch {
      // Already closed.
    }
  }

  webSocketError(ws) {
    this.lobby.closed(new WorkerConn(ws, this.ctx));
  }

  alarm() {
    this.lobby.alarm();
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const stub = env.LOBBY.get(env.LOBBY.idFromName('global'));
    if (isAdminPath(url.pathname)) {
      const internal = (path) => stub.fetch(`https://lobby.internal${path}`).then((r) => {
        if (!r.ok) throw new Error(`lobby answered ${r.status}`);
        return r.json();
      });
      const report = (path, init) => stub.fetch(`https://lobby.internal${path}`, init).then((r) => {
        if (r.status === 404) return null;
        if (!r.ok) throw new Error(`lobby answered ${r.status}`);
        return r.json();
      });
      return handleAdmin(request, env, {
        summary: () => internal('/metrics/summary'),
        history: (days) => internal(`/metrics/history?days=${days}`),
        incidents: () => internal('/incidents'),
        incident: (ticket) => report(`/incidents/${ticket}`),
        setIncidentStatus: (ticket, change) => report(`/incidents/${ticket}/status`, { method: 'POST', body: JSON.stringify(change) }),
      }, (text) => console.log(text));
    }
    if (request.headers.get('Upgrade')?.toLowerCase() === 'websocket') return handleUpgrade(request, env, stub);
    return Response.json({ service: 'dcp-lobby', proto: PROTOCOL }, { headers: { 'X-Content-Type-Options': 'nosniff' } });
  },
};
