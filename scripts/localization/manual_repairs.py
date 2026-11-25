#!/usr/bin/env python3
"""Surgical BC7 block restorations for PT-BR manual pages whose source PNGs had artwork erased."""
import argparse
import hashlib
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts' / 'prompts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ftex  # noqa: E402

# 4x4-aligned (x0, y0, x1, y1) regions restored verbatim from US_manual_NNN.ftx.
REPAIRS = {
    2: (
        (2304, 144, 2452, 400),
    ),
    9: (
        (1232, 156, 1852, 292),
        (1972, 136, 2236, 304),
    ),
    10: (
        (136, 132, 336, 268),
        (2632, 1340, 2860, 1420),
    ),
}

# 4x4-aligned (x0, y0, x1, y1) regions re-encoded via BC7 mode 6 on page 10:
# - (336, 156, 488, 252): 'ns' of "Itens" composited cleanly after verbatim USA "Ite"
# - (2168, 1348, 2632, 1416): wrapped 2-line "Pergaminho de Nevasca" description
PAGE10_COMPOSITED_RECTS = (
    (336, 156, 488, 252),
    (2168, 1348, 2632, 1416),
)

# Pinned decoded RGBA SHA-256 digests for Issue #90 regression checks on manual page 9.
PAGE9_REGION_HASHES = {
    'top_left_vivian_bar': ((1232, 156, 1852, 292), '02184b29ecc6e0948ba6a74b0188b6fe5e3d28f4f254cfd3ac5f4949f45c1dd9'),
    'top_right_dropcap_m': ((1972, 136, 2236, 304), '04eb40c3d54fcf31695452a81dd78e30a8ef203e75676500468997c00e3b66fd'),
    'multijogador_heading': ((1972, 136, 2800, 356), 'f4f7373e212234f5568df8e24f726c1e955a90b7fff24a3678dee43466220532'),
    'unaffected_castelo_heading': ((380, 180, 680, 300), '8898b37b200c57393431d22c67b0433c7cb1dbdd9731c9e748ca9e31a44ec96a'),
}

# Pinned decoded RGBA SHA-256 digests for Issue #90 regression checks on manual page 10 (Items / Itens).
PAGE10_REGION_HASHES = {
    'top_left_dropcap_i_te': ((136, 132, 336, 268), '7f8f97418bb5e030a09a144b346694b82757bf5c46ebc769cf4c0a4fe63c2602'),
    'itens_heading': ((136, 132, 488, 268), '13e78889a6386da0777e7e1f07b06982d9ed227f0aa7e8db1224162347bc807c'),
    'renascimento_scroll_icon': ((2632, 1340, 2860, 1420), 'ba8f677c1d3ba7a3538543cf2d131b330b6560d7a4c7c1cdca8de8f818f910fd'),
    'nevasca_wrapped_desc': ((2168, 1348, 2860, 1420), '624e131aea0faa0602ba1e33672973a417f6d23795ecc97f451daaadb4ae37e8'),
    'unaffected_pocoes_region': ((240, 320, 640, 520), 'b3e0c8d8a6b49fadbfca83a45183246cc9e0553cfc9a2ff7b92feb9e776e89aa'),
}

EXPECTED_REPAIRED_CRCS = {
    2: (0x4e391b1c, 0x2dcbee99),
    9: (0x179aa324, 0x0bbd4170),
    10: (0x92be5fe9, 0x7094160b),
}


def _largest_component_dilated(mask, iterations=2):
    """Pure-NumPy largest 4-connected component + 4-connected dilation (avoids scipy dependency)."""
    import numpy as np
    h, w = mask.shape
    visited = np.zeros((h, w), dtype=bool)
    best = []
    for y in range(h):
        for x in range(w):
            if not mask[y, x] or visited[y, x]:
                continue
            stack = [(y, x)]
            visited[y, x] = True
            comp = []
            while stack:
                cy, cx = stack.pop()
                comp.append((cy, cx))
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not visited[ny, nx]:
                        visited[ny, nx] = True
                        stack.append((ny, nx))
            if len(comp) > len(best):
                best = comp
    out = np.zeros((h, w), dtype=bool)
    for y, x in best:
        out[y, x] = True
    for _ in range(iterations):
        padded = np.pad(out, 1, mode='constant', constant_values=False)
        out = (padded[1:-1, 1:-1] | padded[:-2, 1:-1] | padded[2:, 1:-1] |
               padded[1:-1, :-2] | padded[1:-1, 2:])
    return out


def _apply_page10_composites(usa_blob, pt_blob, repaired_blob, tex):
    """Composite 'ns' of 'Itens' and wrap 'Pergaminho de Nevasca' description into page 10."""
    import numpy as np
    import patch_ftx  # noqa: E402

    us = np.array(ftex.decode(usa_blob, tex).convert('RGBA'))
    pt = np.array(ftex.decode(pt_blob, tex).convert('RGBA'))
    out = np.array(ftex.decode(repaired_blob, tex).convert('RGBA'))
    changed = np.zeros(out.shape[:2], dtype=bool)

    # 1. Composite 'n' (from PT-BR) and 's' (from USA) onto clean interpolated USA background
    # inside (336, 156, 488, 252), directly following verbatim USA 'Ite' (136, 132, 336, 268).
    top_bg = us[150:156, 336:488, :3].astype(np.float32).mean(axis=0)
    bot_bg = us[252:258, 336:488, :3].astype(np.float32).mean(axis=0)
    t_head = np.linspace(0.0, 1.0, 252 - 156)[:, None, None]
    bg_head = top_bg[None, :, :] * (1.0 - t_head) + bot_bg[None, :, :] * t_head
    out[156:252, 336:488, :3] = np.clip(np.round(bg_head), 0, 255).astype(np.uint8)
    out[156:252, 336:488, 3] = 255

    n_raw = pt[168:245, 228:286, :3].astype(np.float32)
    n_lum = n_raw.mean(axis=2)
    keep_n = _largest_component_dilated(n_lum > 150, iterations=2)
    alpha_n = np.where(keep_n, np.clip((n_lum - 18.0) / (250.0 - 18.0), 0.0, 1.0), 0.0)[..., None]
    dst_n = out[165:242, 336:394, :3].astype(np.float32)
    out[165:242, 336:394, :3] = np.clip(np.round(dst_n * (1.0 - alpha_n) + 255.0 * alpha_n), 0, 255).astype(np.uint8)

    s_raw = us[165:244, 429:482, :3].astype(np.float32)
    s_lum = s_raw.mean(axis=2)
    alpha_s = np.clip((s_lum - 16.0) / (250.0 - 16.0), 0.0, 1.0)[..., None]
    dst_s = out[165:244, 392:445, :3].astype(np.float32)
    out[165:244, 392:445, :3] = np.clip(np.round(dst_s * (1.0 - alpha_s) + 255.0 * alpha_s), 0, 255).astype(np.uint8)
    changed[156:252, 336:488] = True

    # 2. Wrap "Libera uma saraivada de ataques de gelo." onto 2 lines inside (2168, 1348, 2632, 1416)
    # so it no longer collides with the verbatim-restored Pergaminho do Renascimento icon (2632..2860).
    top_nb = pt[1344:1350, 2168:2632, :3].astype(np.float32).mean(axis=0)
    bot_nb = pt[1414:1420, 2168:2632, :3].astype(np.float32).mean(axis=0)
    t_nev = np.linspace(0.0, 1.0, 1416 - 1348)[:, None, None]
    bg_nev = top_nb[None, :, :] * (1.0 - t_nev) + bot_nb[None, :, :] * t_nev
    # Feather the rightmost 12 px (2620..2632) smoothly into the verbatim USA restoration at x=2632.
    feather = np.linspace(0.0, 1.0, 12)[None, :, None]
    bg_nev[:, -12:, :] = (bg_nev[:, -12:, :] * (1.0 - feather) +
                          us[1348:1416, 2620:2632, :3].astype(np.float32) * feather)
    out[1348:1416, 2168:2632, :3] = np.clip(np.round(bg_nev), 0, 255).astype(np.uint8)
    out[1348:1416, 2168:2632, 3] = 255

    l1_raw = pt[1361:1388, 2170:2620, :3].astype(np.float32)
    l1_alpha = np.clip((l1_raw.mean(axis=2) - 35.0) / (230.0 - 35.0), 0.0, 1.0)[..., None]
    dst_l1 = out[1352:1379, 2170:2620, :3].astype(np.float32)
    out[1352:1379, 2170:2620, :3] = np.clip(np.round(dst_l1 * (1.0 - l1_alpha) + 255.0 * l1_alpha), 0, 255).astype(np.uint8)

    de_raw = pt[1361:1388, 2470:2512, :3].astype(np.float32)
    de_alpha = np.clip((de_raw.mean(axis=2) - 35.0) / (230.0 - 35.0), 0.0, 1.0)[..., None]
    dst_de = out[1385:1412, 2170:2212, :3].astype(np.float32)
    out[1385:1412, 2170:2212, :3] = np.clip(np.round(dst_de * (1.0 - de_alpha) + 255.0 * de_alpha), 0, 255).astype(np.uint8)

    gelo_pt = pt[1361:1388, 2667:2768, :3].astype(np.float32)
    gelo_us = us[1361:1388, 2667:2768, :3].astype(np.float32)
    gelo_est = np.clip((gelo_pt - gelo_us) / np.maximum(255.0 - gelo_us, 40.0), 0.0, 1.0).mean(axis=2)
    gelo_alpha = np.clip((gelo_est - 0.15) / (0.85 - 0.15), 0.0, 1.0)[..., None]
    dst_ge = out[1385:1412, 2212:2313, :3].astype(np.float32)
    out[1385:1412, 2212:2313, :3] = np.clip(np.round(dst_ge * (1.0 - gelo_alpha) + 255.0 * gelo_alpha), 0, 255).astype(np.uint8)
    changed[1348:1416, 2168:2632] = True

    patched, _ = patch_ftx.patch(repaired_blob, tex, out, changed)
    return patched


def apply(page, usa_blob, pt_blob):
    rects = REPAIRS.get(page)
    if not rects:
        return pt_blob
    us_texs = ftex.textures(usa_blob)
    pt_texs = ftex.textures(pt_blob)
    if len(us_texs) != 1 or us_texs != pt_texs:
        raise ValueError(f'manual page {page}: FTX texture structure mismatch')
    tex = pt_texs[0]
    pb = tex['pitch'] // 4
    base = tex['data']
    out = bytearray(pt_blob)
    for x0, y0, x1, y1 in rects:
        if any(v % 4 != 0 for v in (x0, y0, x1, y1)):
            raise ValueError(f'manual page {page}: rect {(x0, y0, x1, y1)} is not 4x4 aligned')
        if not (0 <= x0 < x1 <= tex['width'] and 0 <= y0 < y1 <= tex['height']):
            raise ValueError(f'manual page {page}: rect {(x0, y0, x1, y1)} out of bounds')
        for by in range(y0 // 4, y1 // 4):
            for bx in range(x0 // 4, x1 // 4):
                o = base + ftex.tiled_offset(bx, by, pb)
                out[o:o + 16] = usa_blob[o:o + 16]
    repaired = bytes(out)
    if page == 10:
        repaired = _apply_page10_composites(usa_blob, pt_blob, repaired, tex)
    return repaired


def region_sha256(rgba, rect):
    x0, y0, x1, y1 = rect
    return hashlib.sha256(rgba[y0:y1, x0:x1].tobytes()).hexdigest()


def check():
    import numpy as np
    import visual_assets as va  # noqa: E402
    cpk = va.Cpk(va.ARCHIVE)
    errors = []
    for page, (source_path, target_label) in enumerate(va.MANUALS, 1):
        patch_path = va.VISUAL / f'manual-us-{page:03d}.dcpx.z'
        usa = va.read_archive(cpk, target_label)
        raw_pt = source_path.read_bytes()
        expected = apply(page, usa, raw_pt)

        try:
            targets = va.parse_dcpx(va.unpack_patch(patch_path.read_bytes()))
        except Exception as exc:
            errors.append(f'{patch_path.name}: {exc}')
            continue
        if len(targets) != 1:
            errors.append(f'{patch_path.name}: expected 1 target, got {len(targets)}')
            continue
        target = targets[0]
        if target['name'] != target_label:
            errors.append(f'{patch_path.name}: target name {target["name"]} != {target_label}')
        if target['size'] != len(usa):
            errors.append(f'{patch_path.name}: size {target["size"]} != {len(usa)}')
        if any(off + 16 > len(usa) for off, _ in target['runs']):
            errors.append(f'{patch_path.name}: out-of-bounds run')
        patched = va.apply_patch(usa, target)
        if patched is None:
            errors.append(f'{patch_path.name}: failed to apply to {target_label}')
            continue
        if patched != expected:
            errors.append(f'{patch_path.name}: patched bytes differ from canonical repaired FTX')
        if va.ftx_structure(usa) != va.ftx_structure(patched):
            errors.append(f'{patch_path.name}: FTX/GNF structure mismatch')
        tex = ftex.textures(patched)[0]
        if (tex['width'], tex['height']) != (3840, 2160):
            errors.append(f'{patch_path.name}: unexpected dimensions {tex["width"]}x{tex["height"]}')

        if page in EXPECTED_REPAIRED_CRCS:
            exp_before, exp_after = EXPECTED_REPAIRED_CRCS[page]
            if (target['before'], target['after']) != (exp_before, exp_after):
                errors.append(f'{patch_path.name}: CRC {target["before"]:08x}->{target["after"]:08x} != '
                              f'{exp_before:08x}->{exp_after:08x}')
        if page in REPAIRS:
            us_rgba = np.array(ftex.decode(usa, tex).convert('RGBA'))
            pt_rgba = np.array(ftex.decode(raw_pt, tex).convert('RGBA'))
            rep_rgba = np.array(ftex.decode(patched, tex).convert('RGBA'))
            repaired_mask = np.zeros(rep_rgba.shape[:2], dtype=bool)
            for x0, y0, x1, y1 in REPAIRS[page]:
                repaired_mask[y0:y1, x0:x1] = True
                if not np.array_equal(rep_rgba[y0:y1, x0:x1], us_rgba[y0:y1, x0:x1]):
                    errors.append(f'manual page {page}: repaired rect {(x0, y0, x1, y1)} does not match USA artwork')
            if page == 10:
                for x0, y0, x1, y1 in PAGE10_COMPOSITED_RECTS:
                    repaired_mask[y0:y1, x0:x1] = True
            if not np.array_equal(rep_rgba[~repaired_mask], pt_rgba[~repaired_mask]):
                errors.append(f'manual page {page}: pixels changed outside repaired rectangles')
            if page == 9:
                for label, (rect, expected_hash) in PAGE9_REGION_HASHES.items():
                    digest = region_sha256(rep_rgba, rect)
                    if expected_hash is None:
                        print(f'page 9 {label} {rect}: {digest}')
                    elif digest != expected_hash:
                        errors.append(f'manual page 9: {label} hash {digest} != {expected_hash}')
            if page == 10:
                for label, (rect, expected_hash) in PAGE10_REGION_HASHES.items():
                    digest = region_sha256(rep_rgba, rect)
                    if expected_hash is None:
                        print(f'page 10 {label} {rect}: {digest}')
                    elif digest != expected_hash:
                        errors.append(f'manual page 10: {label} hash {digest} != {expected_hash}')

        print(f'manual-us-{page:03d}.dcpx.z: source {target["before"]:08x} -> {target["after"]:08x} '
              f'({len(target["runs"])} runs, {patch_path.stat().st_size} bytes)')

    for err in errors:
        print('error:', err)
    print(f'11 manual patches, {len(errors)} errors')
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs='?', default='check', choices=('check',))
    args = parser.parse_args()
    if args.command == 'check':
        return check()
    return 0


if __name__ == '__main__':
    sys.exit(main())
