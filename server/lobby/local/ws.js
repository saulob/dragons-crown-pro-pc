// A small RFC 6455 server for the development lobby: text and control frames, client masking required, fragmented
// messages reassembled up to `maxMessage` bytes.
import { createHash } from 'node:crypto';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

export function acceptKey(key) {
  return createHash('sha1').update(key + GUID).digest('base64');
}

function frame(opcode, payload) {
  const length = payload.length;
  const header = length < 126 ? Buffer.alloc(2) : length < 0x10000 ? Buffer.alloc(4) : Buffer.alloc(10);
  header[0] = 0x80 | opcode;
  if (length < 126) {
    header[1] = length;
  } else if (length < 0x10000) {
    header[1] = 126;
    header.writeUInt16BE(length, 2);
  } else {
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(length), 2);
  }
  return Buffer.concat([header, payload]);
}

// Completes the handshake on an HTTP upgrade; false when the request is not a WebSocket upgrade.
export function upgrade(req, socket, { onText, onClose, maxMessage = 256 * 1024 }) {
  const key = req.headers['sec-websocket-key'];
  if (String(req.headers.upgrade).toLowerCase() !== 'websocket' || !key || req.headers['sec-websocket-version'] !== '13') {
    return null;
  }
  socket.write(['HTTP/1.1 101 Switching Protocols', 'Upgrade: websocket', 'Connection: Upgrade',
    `Sec-WebSocket-Accept: ${acceptKey(key)}`, '', ''].join('\r\n'));
  socket.setNoDelay(true);
  let buffer = Buffer.alloc(0);
  let pieces = [];
  let pieceBytes = 0;
  let closed = false;
  const transport = {
    send(text) {
      if (!closed) socket.write(frame(1, Buffer.from(text, 'utf8')));
    },
    close(code = 1000, reason = '') {
      if (closed) return;
      closed = true;
      const payload = Buffer.alloc(2 + Buffer.byteLength(reason));
      payload.writeUInt16BE(code, 0);
      payload.write(reason, 2);
      socket.end(frame(8, payload));
    },
  };
  const finish = () => {
    if (closed && !socket.destroyed) socket.destroy();
    if (transport.done) return;
    transport.done = true;
    closed = true;
    onClose();
  };
  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length >= 2) {
      const fin = buffer[0] & 0x80;
      const opcode = buffer[0] & 0x0f;
      const masked = buffer[1] & 0x80;
      let length = buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (buffer.length < 4) return;
        length = buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (buffer.length < 10) return;
        const big = buffer.readBigUInt64BE(2);
        if (big > BigInt(maxMessage)) return transport.close(1009, 'too big');
        length = Number(big);
        offset = 10;
      }
      if (!masked) return transport.close(1002, 'unmasked frame');
      if (length > maxMessage || pieceBytes + length > maxMessage) return transport.close(1009, 'too big');
      if (buffer.length < offset + 4 + length) return;
      const mask = buffer.subarray(offset, offset + 4);
      const payload = Buffer.from(buffer.subarray(offset + 4, offset + 4 + length));
      for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      buffer = buffer.subarray(offset + 4 + length);
      if (opcode === 8) {
        transport.close(1000, '');
        return;
      }
      if (opcode === 9) {
        socket.write(frame(10, payload));
        continue;
      }
      if (opcode === 10) continue;
      if (opcode === 1 || opcode === 0) {
        pieces.push(payload);
        pieceBytes += payload.length;
        if (fin) {
          const text = Buffer.concat(pieces).toString('utf8');
          pieces = [];
          pieceBytes = 0;
          onText(text);
        }
        continue;
      }
      return transport.close(1003, 'unsupported frame');
    }
  });
  socket.on('close', finish);
  socket.on('error', finish);
  return transport;
}
