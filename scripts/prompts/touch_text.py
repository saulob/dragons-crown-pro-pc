#!/usr/bin/env python3
"""Xbox wording for the touchpad in the message sheets. The touchpad button is View on an Xbox pad, and the atlas paints
the glyph @#(34) as VIEW, so a translated sentence that names the touch pad gets the glyph the English sentence has.
The strings of a message sheet are independent NUL-terminated records (a pointer table filled in at load follows the
header), so a sentence is blank-padded after its end and keeps its size: make_dcpx.py can then express the change as a
block patch, and every other record stays byte for byte as it was.

    touch_text.py SystemMsg_ES.fms out.fms
    touch_text.py selftest
"""
import argparse
import struct
from pathlib import Path

TOUCHPAD = '@#(34)'

REPLACEMENTS = {
    'SystemMsg_DE.fms': [
        ('Drücke die Touchpad-Taste, um die ##(COLOR_TOWN_MENU) zu öffnen.', f'Drücke die {TOUCHPAD}-Taste, um die ##(COLOR_TOWN_MENU) zu öffnen.'),
        ('Du kannst sie mit der Touchpad-Taste öffnen.', f'Du kannst sie mit der {TOUCHPAD}-Taste öffnen.'),
        ('Drücke die Touchpad-Taste um sie zu öffnen.', f'Drücke die {TOUCHPAD}-Taste um sie zu öffnen.'),
        ('Typ d. Touchpad-Taste bearbeiten', f'Typ d. {TOUCHPAD}-Taste bearbeiten'),
        ('Steuerung d. Touchpad-Taste wechseln', f'Steuerung d. {TOUCHPAD}-Taste wechseln'),
    ],
    'SystemMsg_ES.fms': [
        ('Pulsa el botón del panel táctil para abrir ##(COLOR_TOWN_MENU).', f'Pulsa el botón {TOUCHPAD} para abrir ##(COLOR_TOWN_MENU).'),
        ('Ábrelo con el botón del panel táctil.', f'Ábrelo con el botón {TOUCHPAD}.'),
        ('##(COLOR_TOUCH) para abrirlo o pulsa el botón del panel táctil.', f'##(COLOR_TOUCH) para abrirlo o pulsa el botón {TOUCHPAD}.'),
        ('Botón del panel táctil: editar fuente', f'Botón {TOUCHPAD}: editar fuente'),
        ('Botón del panel táctil: cambiar control', f'Botón {TOUCHPAD}: cambiar control'),
    ],
    'SystemMsg_FR.fms': [
        ('Appuyez sur la touche pavé tactile pour ouvrir la ##(COLOR_TOWN_MENU).', f'Appuyez sur la touche {TOUCHPAD} pour ouvrir la ##(COLOR_TOWN_MENU).'),
        ("Vous pouvez l'ouvrir avec la touche pavé tactile.", f"Vous pouvez l'ouvrir avec la touche {TOUCHPAD}."),
        ('##(COLOR_TOUCH) pour l\'ouvrir ou appuyer sur la touche pavé tactile.', f"##(COLOR_TOUCH) pour l'ouvrir ou appuyer sur la touche {TOUCHPAD}."),
        ('Touche pavé tactile : Modifier type', f'Touche {TOUCHPAD} : Modifier type'),
        ('Touche pavé tactile : Changer les commandes', f'Touche {TOUCHPAD} : Changer les commandes'),
    ],
    'SystemMsg_IT.fms': [
        ('Premi il pulsante touch pad per aprire la ##(COLOR_TOWN_MENU).', f'Premi il pulsante {TOUCHPAD} per aprire la ##(COLOR_TOWN_MENU).'),
        ('Puoi accedervi col pulsante touch pad.', f'Puoi accedervi col pulsante {TOUCHPAD}.'),
        ('##(COLOR_TOUCH)lo o premi il pulsante touch pad per accedervi.', f'##(COLOR_TOUCH)lo o premi il pulsante {TOUCHPAD} per accedervi.'),
        ('Modifica tipo col pulsante touch pad', f'Modifica tipo col pulsante {TOUCHPAD}'),
        ('Cambia controllo col pulsante touch pad', f'Cambia controllo col pulsante {TOUCHPAD}'),
    ],
}


def records(data):
    """The NUL-terminated strings of a message sheet, from the end of the pointer table to the FEOC trailer (zero padding follows the last one)."""
    if data[:4] != b'FMSB':
        raise ValueError('not a message sheet')
    count = struct.unpack_from('<I', data, 0x14)[0]
    end = 0x20 + struct.unpack_from('<I', data, 4)[0]
    if data[end:end + 4] != b'FEOC':
        raise ValueError('no FEOC trailer where the header says')
    out = data[0x20 + count * 8:end].split(bytes([0]))
    out.pop()
    if len(out) < count or any(out[count:]):
        raise ValueError(f'{len(out)} records, the header says {count}')
    return out[:count]


def replace_sentence(data, old, new):
    """Replaces a sentence that ends its NUL-terminated record; the new text is blank-padded to the old length."""
    old_b, new_b = old.encode('utf-8'), new.encode('utf-8')
    if len(new_b) > len(old_b):
        raise ValueError(f'replacement longer than the original: {old!r}')
    tail = old_b + b'\x00'
    count = data.count(tail)
    if count != 1:
        raise ValueError(f'{count} sentences match {old!r}')
    padded = new_b + b' ' * (len(old_b) - len(new_b))
    return data.replace(tail, padded + b'\x00')


def check(before, after, sentences):
    """Fails unless the sheets differ in the records that end with one of `sentences` and nowhere else."""
    count = struct.unpack_from('<I', before, 0x14)[0]
    table_end = 0x20 + count * 8
    if len(before) != len(after) or before[:table_end] != after[:table_end]:
        raise ValueError('size, header or pointer table changed')
    pairs = [(old.encode('utf-8'), new.encode('utf-8')) for old, new in sentences]
    changed = 0
    for old, new in zip(records(before), records(after)):
        if old == new:
            continue
        if not any(old.endswith(o) and new == old[:-len(o)] + n + b' ' * (len(o) - len(n)) for o, n in pairs):
            raise ValueError(f'unintended change: {old!r} -> {new!r}')
        changed += 1
    if changed != len(pairs):
        raise ValueError(f'{changed} records changed, expected {len(pairs)}')


def patched(data, sentences):
    out = data
    for old, new in sentences:
        out = replace_sentence(out, old, new)
    check(data, out, sentences)
    return out


def selftest():
    def sheet(*strings):
        body = b''.join(s.encode('utf-8') + b'\x00' for s in strings)
        table = b'\x00' * (8 * len(strings))
        head = b'FMSB' + struct.pack('<IIII', len(table) + len(body), 0x20, 0, 0)
        head += struct.pack('<II', len(strings), 3) + b'\x00' * 4
        return head + table + body + b'FEOC' + b'\x00' * 12

    original = sheet('Appraise', 'Sell price', 'Pulsa el botón del panel táctil.', 'Rank', '@#(34) Tipo')
    sentence = [('Pulsa el botón del panel táctil.', f'Pulsa el botón {TOUCHPAD}.')]
    out = patched(original, sentence)
    assert len(out) == len(original) and out != original
    after = records(out)
    assert after[2].startswith(f'Pulsa el botón {TOUCHPAD}.'.encode()) and after[2].endswith(b' ')
    assert [after[i] for i in (0, 1, 3, 4)] == [records(original)[i] for i in (0, 1, 3, 4)]
    for bad in ([('Pulsa el botón del panel táctil.', 'Pulsa el botón del panel táctil, ahora más largo')],
                [('Rank', 'Rank'), ('Missing', 'x')]):
        try:
            patched(original, bad)
        except ValueError:
            continue
        raise AssertionError('accepted an unsafe replacement')
    damaged = bytearray(out)
    damaged[len(original) - 20] ^= 1
    try:
        check(original, bytes(damaged), sentence)
    except ValueError:
        print('touch_text selftest ok')
        return
    raise AssertionError('missed an unrelated change')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('fms'); ap.add_argument('out', nargs='?')
    args = ap.parse_args()
    if args.fms == 'selftest':
        selftest()
        return
    src = Path(args.fms)
    sentences = REPLACEMENTS.get(src.name, [])
    data = patched(src.read_bytes(), sentences)
    Path(args.out).write_bytes(data)
    print(f'{src.name}: {len(sentences)} sentences -> {args.out} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
