"""Builds src/title_layout_data.inc, the four-row title menu patch for the game's title layout (stage/title.mbs).

The title art lays out six menu rows (y = -80 + 50 * row). The port's menu keeps four of them (Start Game, Options,
Gallery, Exit Game), so in the parts the title uses:
  - the plates of rows 4 and 5 are dropped from the part's layer list;
  - the selected-row layers (strip, highlighted plate) of the Options, Gallery and Exit rows move up to rows 1, 2 and 3
    by pointing their keyframes at the transform with the same attributes 100 px higher.
Layers, keyframes and transforms are the same in every language's title layout; only the parts' bounds differ.

    python scripts/title_layout_patch.py title.mbs [more.mbs ...]     checks the patch on each layout
    python scripts/title_layout_patch.py --emit src/title_layout_data.inc title.mbs
"""
import struct
import sys

ROWS = [-80, -30, 20, 70, 120, 170]
KEEP_ROWS = 4
PLATE_ELEMENTS = {43, 44, 53, 56}
SELECTED_ELEMENT = 53
STRIP_ELEMENTS = {45, 46}
LAYER_SIZE = 24

# part -> (selected row before, after); None: the part has no selected row
PARTS = {
    "TITLE_1": None,
    "START_0": (0, 0), "START_1": (0, 0), "START_2": None,
    "OPTION_0": (3, 1), "OPTION_1": (3, 1), "OPTION_2": None,
    "GALLERY_0": (4, 2), "GALLERY_1": (4, 2), "GALLERY_2": None,
    "PSSTORE_0": (5, 3),
}


class Layout:
    def __init__(self, data):
        self.d = bytearray(data)
        offsets = [struct.unpack_from("<Q", self.d, 0xb0 + 8 * i)[0] for i in range(11)]
        self.transforms_at, self.keyframes_at, self.parts_at, self.layers_at = offsets[7], offsets[8], offsets[9], offsets[10]
        self.transform_count = struct.unpack_from("<H", self.d, 0x5e)[0]
        self.keyframe_count = struct.unpack_from("<H", self.d, 0x60)[0]
        self.part_count = struct.unpack_from("<H", self.d, 0x64)[0]
        self.layer_count = struct.unpack_from("<H", self.d, 0x66)[0]

    def part(self, i):
        r = self.d[self.parts_at + 48 * i: self.parts_at + 48 * i + 48]
        return dict(name=bytes(r[16:40]).split(b"\0")[0].decode(), off=struct.unpack_from("<H", r, 40)[0], cnt=r[42], master=r[43])

    def layer_bytes(self, i):
        return bytes(self.d[self.layers_at + LAYER_SIZE * i: self.layers_at + LAYER_SIZE * (i + 1)])

    def keyframe_range(self, i):
        return struct.unpack_from("<HH", self.layer_bytes(i), 0)

    def keyframe(self, i):
        return bytes(self.d[self.keyframes_at + 32 * i: self.keyframes_at + 32 * i + 32])

    def transform(self, i):
        return bytes(self.d[self.transforms_at + 36 * i: self.transforms_at + 36 * i + 36])


def layer_info(lay, li):
    start, count = lay.keyframe_range(li)
    elements, transforms = [], []
    for j in range(count):
        k = lay.keyframe(start + j)
        elements.append(struct.unpack_from("<I", k, 0)[0])
        transforms.append(struct.unpack_from("<H", k, 4)[0])
    return elements, transforms


def classify(lay, li):
    elements, transforms = layer_info(lay, li)
    xy = [struct.unpack_from("<ff", lay.transform(t), 0) for t in transforms]
    if elements and all(e in STRIP_ELEMENTS for e in elements):
        ys = {round(y) for x, y in xy}
        assert len(ys) == 1 and ys.pop() in ROWS, li
        return "strip", ROWS.index(round(xy[0][1]))
    if all(e in PLATE_ELEMENTS for e in elements):
        rows = {ROWS.index(round(y)) for e, (x, y) in zip(elements, xy) if e == 44 and round(x) == 0 and round(y) in ROWS}
        if len(rows) == 1:
            return "plate", rows.pop()
    return "other", None


def transform_shifter(lay):
    index = {}
    for t in range(lay.transform_count):
        b = lay.transform(t)
        y = round(struct.unpack_from("<f", b, 4)[0])
        index.setdefault((b[:4], b[8:]), {}).setdefault(y, t)

    def shift(t, dy):
        b = lay.transform(t)
        y = round(struct.unpack_from("<f", b, 4)[0])
        found = index.get((b[:4], b[8:]), {}).get(y + dy)
        assert found is not None, ("no transform", t, y, dy)
        return found
    return shift


def build_patch(data):
    lay = Layout(data)
    shift = transform_shifter(lay)
    by_name = {}
    for i in range(lay.part_count):
        name = lay.part(i)["name"]
        if name:
            by_name[name] = i
    patch = []
    for name, rows in PARTS.items():
        pi = by_name[name]
        p = lay.part(pi)
        layers = list(range(p["off"], p["off"] + p["cnt"]))
        cls = {li: classify(lay, li) for li in layers}
        plates = {r: li for li, (c, r) in cls.items() if c == "plate"}
        strips = [li for li, (c, r) in cls.items() if c == "strip"]
        assert sorted(plates) == list(range(6)), (name, sorted(plates))
        selected = [r for r, li in plates.items() if SELECTED_ELEMENT in layer_info(lay, li)[0]]
        assert len(selected) <= 1 and len(strips) <= 1, name
        keyframe_edits = {}

        def edit(li, dy):
            start, count = lay.keyframe_range(li)
            for j in range(count):
                t = struct.unpack_from("<H", lay.keyframe(start + j), 4)[0]
                moved = shift(t, dy)
                if moved != t:
                    keyframe_edits[start + j] = (t, moved)

        source = {r: plates[r] for r in range(KEEP_ROWS)}
        if rows is None:
            assert not strips and not selected, name
        else:
            old_row, new_row = rows
            assert all(cls[li][1] == old_row for li in strips), name
            assert not selected or selected[0] == old_row, name
            dy = ROWS[new_row] - ROWS[old_row]
            if old_row != new_row:
                if selected:
                    source[new_row] = plates[old_row]
                    edit(plates[old_row], dy)
                    if old_row < KEEP_ROWS:
                        source[old_row] = plates[new_row]
                        edit(plates[new_row], -dy)
                for li in strips:
                    edit(li, dy)
        order = []
        for li in layers:
            kind, row = cls[li]
            if kind != "plate":
                order.append(li)
            elif row < KEEP_ROWS:
                order.append(source[row])
        master = layers[p["master"]]
        assert master in order, (name, "the master layer is dropped")
        patch.append(dict(name=name, index=pi, off=p["off"], old_cnt=p["cnt"], new_cnt=len(order), old_master=p["master"],
                          new_master=order.index(master), order=[li - p["off"] for li in order],
                          old_layers=b"".join(lay.layer_bytes(li) for li in layers),
                          keyframes=sorted((k, t, m) for k, (t, m) in keyframe_edits.items())))
    return patch


def apply_patch(data, patch):
    lay = Layout(data)
    d = lay.d
    for part in patch:
        old = part["old_layers"]
        for pos, src in enumerate(part["order"]):
            o = lay.layers_at + LAYER_SIZE * (part["off"] + pos)
            d[o:o + LAYER_SIZE] = old[LAYER_SIZE * src: LAYER_SIZE * (src + 1)]
        for k, t, m in part["keyframes"]:
            o = lay.keyframes_at + 32 * k + 4
            assert struct.unpack_from("<H", d, o)[0] == t
            struct.pack_into("<H", d, o, m)
        o = lay.parts_at + 48 * part["index"]
        d[o + 42], d[o + 43] = part["new_cnt"], part["new_master"]
    return bytes(d)


def check(data):
    patch = build_patch(data)
    lay = Layout(apply_patch(data, patch))
    for part in patch:
        p = lay.part(part["index"])
        plates, strip, selected = [], None, None
        for k in range(p["cnt"]):
            kind, row = classify(lay, p["off"] + k)
            if kind == "plate":
                plates.append(row)
                if SELECTED_ELEMENT in layer_info(lay, p["off"] + k)[0]:
                    selected = row
            elif kind == "strip":
                strip = row
        assert sorted(plates) == list(range(KEEP_ROWS)), (part["name"], plates)
        expect = PARTS[part["name"]]
        if expect:
            assert strip in (None, expect[1]) and selected in (None, expect[1]), part["name"]
    return patch


def emit(patch, path):
    out = ["/* Generated by scripts/title_layout_patch.py from the game's stage/title.mbs. Do not edit. */",
           "#define TITLE_LAYOUT_PART_COUNT %d" % len(patch), ""]
    pool, order_at, layers_at = bytearray(), {}, {}
    orders = bytearray()
    for part in patch:
        order_at[part["name"]] = len(orders)
        orders += bytes(part["order"])
        layers_at[part["name"]] = len(pool)
        pool += part["old_layers"]
    keyframes = [(k, t, m) for part in patch for (k, t, m) in part["keyframes"]]

    def rows(data, per=16):
        return ",\n".join("    " + ", ".join("0x%02x" % b for b in data[i:i + per]) for i in range(0, len(data), per))
    out += ["static const uint8_t kTitleLayoutOrder[] = {", rows(orders), "};", "",
            "static const uint8_t kTitleLayoutOldLayers[] = {", rows(bytes(pool)), "};", "",
            "static const TitleLayoutPart kTitleLayoutParts[TITLE_LAYOUT_PART_COUNT] = {"]
    for part in patch:
        out.append('    {"%s", %d, %d, %d, %d, %d, %d, %d, %d},' % (
            part["name"], part["index"], part["off"], part["old_cnt"], part["new_cnt"], part["old_master"], part["new_master"],
            order_at[part["name"]], layers_at[part["name"]]))
    out += ["};", "", "#define TITLE_LAYOUT_KEYFRAME_COUNT %d" % len(keyframes),
            "static const TitleLayoutKeyframe kTitleLayoutKeyframes[TITLE_LAYOUT_KEYFRAME_COUNT] = {"]
    out += ["    {%d, %d, %d}," % kf for kf in keyframes]
    out.append("};")
    with open(path, "wb") as f:
        f.write(("\n".join(out) + "\n").encode())


def main(argv):
    emit_path = None
    if argv and argv[0] == "--emit":
        emit_path, argv = argv[1], argv[2:]
    first = None
    for path in argv:
        with open(path, "rb") as f:
            patch = check(f.read())
        if first is None:
            first = patch
        assert patch == first, "%s patches differently" % path
        print("%s: %d parts, %d keyframe edits" % (path, len(patch), sum(len(p["keyframes"]) for p in patch)))
    if emit_path and first:
        emit(first, emit_path)


if __name__ == "__main__":
    main(sys.argv[1:])
