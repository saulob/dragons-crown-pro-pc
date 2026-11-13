// The parts of the Durable Object WebSocket Hibernation runtime the Worker uses, enough to load worker.js under Node: one
// object's storage and accepted sockets, which outlive any LobbyObject built on them, as they outlive a hibernation.
import * as nodeModule from 'node:module';

import { sqliteStore } from '../../local/host.js';

// registerHooks arrived in Node.js 22.15; older versions skip the tests that load worker.js.
export const available = typeof nodeModule.registerHooks === 'function';

nodeModule.registerHooks?.({
  resolve(specifier, context, next) {
    if (specifier === 'cloudflare:workers') return { url: new URL('./cloudflare_workers.js', import.meta.url).href, shortCircuit: true };
    return next(specifier, context);
  },
});

globalThis.WebSocketRequestResponsePair = class {
  constructor(request, response) {
    this.request = request;
    this.response = response;
  }
};

export class HibernatingSocket {
  constructor() {
    this.stored = null;
    this.sent = [];
    this.closedWith = null;
    this.bufferedAmount = 0;
    this.autoResponseAt = null;
  }

  serializeAttachment(value) {
    this.stored = structuredClone(value);
  }

  deserializeAttachment() {
    return this.stored === null ? null : structuredClone(this.stored);
  }

  send(data) {
    this.sent.push(data);
  }

  close(code, reason) {
    this.closedWith ??= { code, reason };
  }
}

globalThis.WebSocketPair = class {
  constructor() {
    this[0] = new HibernatingSocket();
    this[1] = new HibernatingSocket();
  }
};

const NodeResponse = globalThis.Response;
globalThis.Response = class extends NodeResponse {
  constructor(body, init = {}) {
    if (init.status !== 101) {
      super(body, init);
      return;
    }
    super(null, { status: 200 });
    Object.defineProperty(this, 'status', { value: 101 });
    this.webSocket = init.webSocket;
  }
};

export const LobbyObject = available ? (await import('../../src/worker.js')).LobbyObject : null;

export class DurableObjectState {
  constructor() {
    this.store = sqliteStore();
    this.sockets = [];
    this.autoResponse = null;
    this.alarmAt = null;
    this.storage = {
      sql: {
        exec: (query, ...args) => {
          const rows = this.store.all(query, ...args);
          return { toArray: () => rows };
        },
      },
      transactionSync: (work) => this.store.transaction(work),
      getAlarm: async () => this.alarmAt,
      setAlarm: async (ms) => {
        this.alarmAt = ms;
      },
    };
  }

  blockConcurrencyWhile(work) {
    return work();
  }

  acceptWebSocket(ws) {
    this.sockets.push(ws);
  }

  getWebSockets() {
    return this.sockets.filter((ws) => !ws.closedWith);
  }

  setWebSocketAutoResponse(pair) {
    this.autoResponse = pair;
  }

  getWebSocketAutoResponseTimestamp(ws) {
    return ws.autoResponseAt;
  }

  changes() {
    return this.store.get('SELECT total_changes() AS n').n;
  }

  // A LobbyObject on this state, as the runtime builds one for a request or after a hibernation.
  async wake() {
    const object = new LobbyObject(this, {});
    await object.ctx.blockConcurrencyWhile(async () => {});
    await new Promise((resolve) => setImmediate(resolve));
    return object;
  }

  async connect(object, ip = '203.0.113.7') {
    const request = new Request('https://lobby.example/', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': ip } });
    const response = await object.fetch(request);
    return response.status === 101 ? this.sockets.at(-1) : null;
  }

  // What the runtime does with an incoming message: the auto-response pair is answered without waking the object.
  deliver(object, ws, message) {
    if (ws.closedWith) return;
    if (typeof message === 'string' && this.autoResponse && message === this.autoResponse.request) {
      ws.send(this.autoResponse.response);
      ws.autoResponseAt = new Date(Date.now());
      return;
    }
    object.webSocketMessage(ws, message);
  }
}
