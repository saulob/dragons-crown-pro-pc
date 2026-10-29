#!/usr/bin/env node
// Development lobby: the Worker's lobby (src/lobby.js) over node:http and node:sqlite, without Cloudflare.
//   node server/lobby/local/server.js [--host 127.0.0.1] [--port 8790] [--db <file>|:memory:] [--verbose]
//                                     [--seed-friends <identity.ini> --seed-friends <identity.ini>]
// Clients use ws://<host>:<port>/. Loopback clients are exempt from the per-address limits. With a --db file, accounts,
// friend codes and friends survive restarts; rooms never do.
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

import { Lobby, PROTOCOL, LIMITS } from '../src/lobby.js';
import { LocalConn, LocalHost, sqliteStore } from './host.js';
import { upgrade } from './ws.js';

// What a log line may show of a client message: never the secret.
function describe(text) {
  try {
    const msg = JSON.parse(text);
    if (msg && typeof msg === 'object') {
      if ('secret' in msg) msg.secret = '<hidden>';
      if (typeof msg.log === 'string') msg.log = `<${msg.log.length} chars>`;
      return JSON.stringify(msg).slice(0, 300);
    }
  } catch {
    // Not JSON: shown as is below.
  }
  return text.slice(0, 120);
}

// A client's online_identity.ini (src/net/identity.cpp): account and secret.
export function readIdentity(file, name) {
  const lines = readFileSync(file, 'utf8').split(/\r?\n/);
  const values = Object.fromEntries(lines.map((line) => line.split('=')).filter((kv) => kv.length === 2));
  if (!/^[1-9][0-9]{0,19}$/.test(values.account ?? '') || !/^[0-9a-f]{64}$/.test(values.secret ?? '')) {
    throw new Error(`${file} is not an online identity`);
  }
  return { account: values.account, secret: values.secret, name };
}

export function startLobbyServer({ host: address = '127.0.0.1', port = 8790, db = ':memory:', verbose = false,
  seedFriends = [], log = () => {} } = {}) {
  const host = new LocalHost(sqliteStore(db), { log });
  const lobby = new Lobby(host, { local: true });
  host.onAlarm = () => lobby.alarm();
  const ghosts = lobby.clearRooms();
  if (seedFriends.length === 2) {
    lobby.seedFriends(seedFriends[0], seedFriends[1]);
    log('Lobby: test friends seeded');
  }
  if (db !== ':memory:') {
    const stats = lobby.stats();
    log(`Lobby: persistent test state ${db}`);
    log(`Lobby: loaded ${stats.accounts} accounts, ${stats.friendships} friendships, ${stats.requests} pending requests` +
      (ghosts ? `; ${ghosts} rooms of the previous run removed` : ''));
  }

  const server = createServer((req, res) => {
    res.setHeader('Content-Type', 'application/json');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.end(JSON.stringify({ service: 'dcp-lobby (local)', proto: PROTOCOL, sockets: host.conns.size }));
  });

  server.on('upgrade', (req, socket) => {
    const ip = req.socket.remoteAddress?.replace(/^::ffff:/, '') ?? '';
    const first = lobby.admit(ip);
    if (!first) {
      socket.end('HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\n\r\n');
      return;
    }
    let conn = null;
    const transport = upgrade(req, socket, {
      maxMessage: LIMITS.reportFrameBytes,
      onText(text) {
        if (text === 'ping') return transport.send('pong');
        if (verbose) log(`<- ${first.cid} ${describe(text)}`);
        lobby.message(conn, text);
      },
      onClose() {
        if (!conn) return;
        lobby.closed(conn);
        host.conns.delete(conn);
        log(`socket ${first.cid} closed (${host.conns.size} open)`);
      },
    });
    if (!transport) {
      socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
      return;
    }
    const send = transport.send;
    transport.send = (text) => {
      if (verbose) log(`-> ${first.cid} ${text.slice(0, 300)}`);
      send(text);
    };
    conn = new LocalConn(host, transport);
    conn.setAttachment(first);
    host.conns.add(conn);
    log(`socket ${first.cid} from ${ip} (${host.conns.size} open)`);
  });

  return new Promise((resolve) => {
    server.listen(port, address, () => {
      resolve({
        port: server.address().port,
        lobby,
        host,
        close: () => new Promise((done) => {
          for (const conn of host.conns) conn.close(1001, 'server stopping');
          server.close(() => {
            host.sql.close();
            done();
          });
          server.closeAllConnections?.();
        }),
      });
    });
  });
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      host: { type: 'string', default: '127.0.0.1' },
      port: { type: 'string', default: '8790' },
      db: { type: 'string', default: ':memory:' },
      verbose: { type: 'boolean', default: false },
      'seed-friends': { type: 'string', multiple: true, default: [] },
    },
  });
  const seeds = values['seed-friends'];
  if (seeds.length !== 0 && seeds.length !== 2) throw new Error('--seed-friends takes two identity files');
  const stamp = () => new Date().toISOString().slice(11, 23);
  const log = (text) => console.log(`${stamp()} ${text}`);
  const started = await startLobbyServer({ host: values.host, port: Number(values.port), db: values.db,
    verbose: values.verbose, seedFriends: seeds.map((file, i) => readIdentity(file, `Client${'AB'[i]}`)), log });
  log(`dcp lobby on ws://${values.host}:${started.port}/ (database ${values.db}); Ctrl+C stops it`);
}
