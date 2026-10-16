#!/usr/bin/env python3
"""A BC7 encoder, mode 6 only (one subset, RGBA 7.7.7.7 endpoints + per-endpoint P bit, 4-bit
indices). Good for UI art; vectorised over blocks with numpy.

    encode(rgba) -> bytes      rgba: uint8 array (H, W, 4), H and W multiples of 4
"""
import numpy as np

WEIGHTS4 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int32)


def _fit_endpoints(px):
    """px: (N,16,4) float. Returns (e0, e1) (N,4) along each block's principal axis."""
    mean = px.mean(axis=1, keepdims=True)
    d = px - mean
    cov = np.einsum('nia,nib->nab', d, d) / 16
    # power iteration for the principal eigenvector
    v = np.ones((px.shape[0], 4)) + np.random.default_rng(0).random((px.shape[0], 4)) * 0.01
    for _ in range(8):
        v = np.einsum('nab,nb->na', cov, v)
        n = np.linalg.norm(v, axis=1, keepdims=True)
        v = np.where(n > 1e-9, v / np.maximum(n, 1e-9), np.array([[1.0, 0, 0, 0]]))
    t = np.einsum('nia,na->ni', d, v)
    tmin, tmax = t.min(axis=1), t.max(axis=1)
    e0 = mean[:, 0, :] + v * tmin[:, None]
    e1 = mean[:, 0, :] + v * tmax[:, None]
    return e0, e1


def _refine(px, idx):
    """Least-squares endpoints for fixed indices. px (N,16,4), idx (N,16) ints -> e0, e1 (N,4)."""
    w = WEIGHTS4[idx] / 64.0               # (N,16)
    a = 1 - w
    aa = (a * a).sum(1); ab = (a * w).sum(1); bb = (w * w).sum(1)
    ax = np.einsum('ni,nic->nc', a, px); bx = np.einsum('ni,nic->nc', w, px)
    det = aa * bb - ab * ab
    ok = np.abs(det) > 1e-6
    det = np.where(ok, det, 1.0)
    e0 = np.zeros((px.shape[0], 4)); e1 = np.zeros_like(e0)
    e0[ok] = ((bb * ax.T - ab * bx.T).T / det[:, None])[ok]
    e1[ok] = ((aa * bx.T - ab * ax.T).T / det[:, None])[ok]
    mean = px.mean(1)
    e0[~ok] = mean[~ok]; e1[~ok] = mean[~ok]
    return np.clip(e0, 0, 255), np.clip(e1, 0, 255)


def _quantize(e, p):
    """7-bit endpoint + p bit -> the 8-bit value the decoder reconstructs."""
    q = np.clip(np.round((e - p) / 2.0), 0, 127).astype(np.int32)
    return q, (q << 1) | p


def _indices(px, c0, c1):
    """Best 4-bit index per pixel for reconstructed 8-bit endpoints c0, c1 (N,4)."""
    pal = ((64 - WEIGHTS4)[None, :, None] * c0[:, None, :] + WEIGHTS4[None, :, None] * c1[:, None, :] + 32) >> 6  # (N,16,4)
    diff = px[:, :, None, :] - pal[:, None, :, :].astype(np.float64)            # (N,16px,16idx,4)
    err = (diff * diff).sum(-1)
    idx = err.argmin(-1)
    return idx, np.take_along_axis(err, idx[..., None], -1)[..., 0].sum(1)


def encode(rgba):
    h, w, _ = rgba.shape
    assert h % 4 == 0 and w % 4 == 0
    blocks = rgba.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    return encode_blocks(blocks)


def encode_blocks(blocks):
    """blocks: (N,16,4) uint8, pixels in raster order -> N*16 bytes."""
    blocks = blocks.astype(np.float64)
    n = blocks.shape[0]
    e0, e1 = _fit_endpoints(blocks)
    best_err = np.full(n, np.inf); best = None
    for it in range(3):
        for p0 in (0, 1):
            for p1 in (0, 1):
                q0, c0 = _quantize(e0, p0); q1, c1 = _quantize(e1, p1)
                idx, err = _indices(blocks, c0, c1)
                better = err < best_err
                if best is None:
                    best = [q0.copy(), q1.copy(), np.full(n, p0), np.full(n, p1), idx.copy()]
                    best_err = err
                else:
                    for k, v in enumerate((q0, q1, np.full(n, p0), np.full(n, p1), idx)):
                        best[k][better] = v[better]
                    best_err = np.where(better, err, best_err)
        # refine endpoints from the best indices so far
        e0, e1 = _refine(blocks, best[4])
    q0, q1, p0, p1, idx = best
    # anchor rule: index of pixel 0 must be < 8; otherwise swap endpoints and invert indices
    swap = idx[:, 0] >= 8
    q0s = np.where(swap[:, None], q1, q0); q1s = np.where(swap[:, None], q0, q1)
    p0s = np.where(swap, p1, p0); p1s = np.where(swap, p0, p1)
    idx = np.where(swap[:, None], 15 - idx, idx)
    out = np.zeros((n, 16), dtype=np.uint8)
    for b in range(n):
        bits = 0
        pos = 6; bits |= 1 << pos; pos = 7
        for ch in range(4):
            bits |= int(q0s[b, ch]) << pos; pos += 7
            bits |= int(q1s[b, ch]) << pos; pos += 7
        bits |= int(p0s[b]) << pos; pos += 1
        bits |= int(p1s[b]) << pos; pos += 1
        bits |= int(idx[b, 0]) << pos; pos += 3
        for i in range(1, 16):
            bits |= int(idx[b, i]) << pos; pos += 4
        out[b] = np.frombuffer(bits.to_bytes(16, 'little'), dtype=np.uint8)
    return out.tobytes()


if __name__ == '__main__':
    import io, struct, sys
    from PIL import Image
    im = Image.open(sys.argv[1]).convert('RGBA')
    a = np.array(im)
    data = encode(a)
    # verify through Pillow's BC7 decoder
    h = bytearray(b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, a.shape[0], a.shape[1], len(data), 1, 1) + b'\0' * 44)
    h += struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0) + struct.pack('<5I', 0x1000, 0, 0, 0, 0) + struct.pack('<5I', 98, 3, 0, 1, 0)
    back = np.array(Image.open(io.BytesIO(bytes(h) + data)).convert('RGBA')).astype(np.float64)
    mse = ((back - a) ** 2).mean()
    print(f'{a.shape[1]}x{a.shape[0]}: PSNR {10 * np.log10(255 ** 2 / max(mse, 1e-9)):.1f} dB')
