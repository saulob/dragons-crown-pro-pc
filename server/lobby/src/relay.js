// The relay: PeerSocket datagrams of players who could not reach each other directly, carried as binary WebSocket
// messages and forwarded unchanged to one member of the sender's own room (docs/ONLINE.md, "Relay"). Frame: "DR",
// version, type, room token (u32), member (u16), sequence (u32), payload length (u16), payload; big-endian. The sender
// names the destination member; the lobby replaces it with the sender's own before forwarding.

export const RELAY = {
  version: 1,
  data: 1,
  headerBytes: 16,
  maxPayload: 1200,
  connection: { packets: { burst: 800, perSecond: 400 }, bytes: { burst: 256 * 1024, perSecond: 128 * 1024 } },
  room: { packets: { burst: 1600, perSecond: 800 }, bytes: { burst: 512 * 1024, perSecond: 256 * 1024 } },
  overrunsToClose: 256,
  bufferedBytes: 256 * 1024,
  loggedDrops: 5,
  loggedDropEvery: 1000,
};

export const CAPABILITIES = { relay_supported: true, relay_max_payload: RELAY.maxPayload };

export function parseRelayFrame(bytes) {
  if (!(bytes instanceof Uint8Array)) return { error: 'not_binary' };
  if (bytes.length < 4) return { error: 'truncated' };
  if (bytes[0] !== 0x44 || bytes[1] !== 0x52) return { error: 'bad_magic' };
  if (bytes[2] !== RELAY.version) return { error: 'bad_version' };
  if (bytes[3] !== RELAY.data) return { error: 'bad_type' };
  if (bytes.length < RELAY.headerBytes) return { error: 'truncated' };
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(14);
  if (length > RELAY.maxPayload || bytes.length > RELAY.headerBytes + RELAY.maxPayload) return { error: 'oversized' };
  if (length === 0 || bytes.length !== RELAY.headerBytes + length) return { error: 'bad_length' };
  return { token: view.getUint32(4), member: view.getUint16(8), seq: view.getUint32(10), length };
}

export function encodeRelayFrame(token, member, seq, payload) {
  const bytes = new Uint8Array(RELAY.headerBytes + payload.length);
  const view = new DataView(bytes.buffer);
  bytes.set([0x44, 0x52, RELAY.version, RELAY.data]);
  view.setUint32(4, token >>> 0);
  view.setUint16(8, member);
  view.setUint32(10, seq >>> 0);
  view.setUint16(14, payload.length);
  bytes.set(payload, RELAY.headerBytes);
  return bytes;
}

export const roomToken = (key) => parseInt(String(key).slice(0, 8), 16) >>> 0;

function take(bucket, spec, now, cost) {
  const [tokens, last] = bucket ?? [spec.burst, now];
  const refilled = Math.min(spec.burst, tokens + ((now - last) / 1000) * spec.perSecond);
  return refilled < cost ? null : [refilled - cost, now];
}

// Packets and bytes both have to fit; nothing is taken from either when one does not.
function takeBudget(budget, spec, now, bytes) {
  const packets = take(budget.packets, spec.packets, now, 1);
  const volume = packets && take(budget.bytes, spec.bytes, now, bytes);
  if (!volume) return false;
  budget.packets = packets;
  budget.bytes = volume;
  return true;
}

export class Relay {
  // lobby: the Lobby whose rooms, members and sockets this forwards between.
  constructor(lobby) {
    this.lobby = lobby;
    this.routes = new Map();
    this.connections = new Map();
    this.rooms = new Map();
    this.drops = new Map();
  }

  log(text) {
    this.lobby.host.log(`relay: ${text}`);
  }

  negotiated(att) {
    this.log(`negotiated cid=${att.cid}`);
  }

  frame(conn, bytes) {
    const att = conn.attachment();
    if (!att?.hello) return this.drop('unknown_connection');
    if (!att.relay) return this.drop('not_negotiated');
    const parsed = parseRelayFrame(bytes);
    if (parsed.error) return this.drop(parsed.error);
    const state = this.connection(att.cid);
    if (state.closed) return this.drop('relay_closed');
    const now = this.lobby.host.now();
    if (!takeBudget(state.budget, RELAY.connection, now, bytes.length)) return this.overrun(conn, state, 'rate_limit');
    const route = this.routeFor(att, parsed.member);
    if (typeof route === 'string') return this.drop(route);
    if (parsed.token !== route.token) return this.drop('bad_token');
    const room = this.room(route.room);
    if (!takeBudget(room.budget, RELAY.room, now, bytes.length)) return this.overrun(conn, state, 'room_rate_limit');
    const buffered = route.peer.bufferedAmount;
    if (typeof buffered === 'number' && buffered > RELAY.bufferedBytes) return this.overrun(conn, state, 'backpressure');
    const out = new Uint8Array(bytes);
    new DataView(out.buffer).setUint16(8, att.member);
    try {
      route.peer.sendBinary(out);
    } catch {
      this.forgetConnection(route.peerCid);
      this.drop('destination_unavailable');
    }
  }

  connection(cid) {
    let state = this.connections.get(cid);
    if (!state) {
      state = { budget: {}, overruns: 0, closed: false };
      this.connections.set(cid, state);
    }
    return state;
  }

  room(id) {
    let state = this.rooms.get(id);
    if (!state) {
      state = { budget: {} };
      this.rooms.set(id, state);
    }
    return state;
  }

  // The cached route, else one built from the tables; the reason (a string) when the sender may not reach `member`.
  routeFor(att, member) {
    if (!att.room) return 'no_room';
    const key = `${att.cid}|${member}`;
    const cached = this.routes.get(key);
    if (cached && cached.room === att.room && cached.src === att.member) return cached;
    this.routes.delete(key);
    const lobby = this.lobby;
    const room = lobby.roomRow(att.room);
    if (!room) return 'no_room';
    const source = lobby.memberRow(att.room, att.member);
    if (!source || source.conn !== att.cid) return 'stale_membership';
    if (member === att.member) return 'self';
    const target = lobby.memberRow(att.room, member);
    if (!target) return 'no_destination';
    if (!target.conn) return 'destination_offline';
    lobby.index = null;
    const peer = lobby.socketByCid(target.conn);
    lobby.index = null;
    if (!peer) return 'destination_offline';
    if (!peer.att.relay) return 'destination_not_negotiated';
    if (peer.att.room !== att.room || peer.att.member !== member) return 'destination_stale';
    if ((peer.att.online_protocol ?? 0) !== (att.online_protocol ?? 0)) return 'incompatible';
    const route = { room: att.room, token: roomToken(room.key), src: att.member, dest: member, peerCid: peer.att.cid,
      peer: peer.conn };
    this.routes.set(key, route);
    this.log(`route room=${att.room} ${att.member}->${member}`);
    return route;
  }

  overrun(conn, state, reason) {
    this.drop(reason);
    if (++state.overruns < RELAY.overrunsToClose) return;
    state.closed = true;
    this.log(`closed for cid=${conn.attachment()?.cid} reason=${reason}`);
    this.lobby.send(conn, { ev: 'relay.closed', reason });
  }

  // The first few drops of each reason are logged, then one in a thousand.
  drop(reason) {
    const count = (this.drops.get(reason) ?? 0) + 1;
    this.drops.set(reason, count);
    if (count <= RELAY.loggedDrops || count % RELAY.loggedDropEvery === 0) this.log(`drop reason=${reason} count=${count}`);
  }

  forgetMember(room, member) {
    for (const [key, route] of this.routes) {
      if (route.room === room && (route.src === member || route.dest === member)) this.routes.delete(key);
    }
  }

  forgetConnection(cid) {
    for (const [key, route] of this.routes) {
      if (key.startsWith(`${cid}|`) || route.peerCid === cid) this.routes.delete(key);
    }
    this.connections.delete(cid);
  }

  forgetRoom(room) {
    for (const [key, route] of this.routes) if (route.room === room) this.routes.delete(key);
    this.rooms.delete(room);
  }
}
