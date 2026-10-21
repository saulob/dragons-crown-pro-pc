# Controller prompts: how `launcher/prompts/xbox-prompts.dcpx` is made

The game draws every button prompt from one 1024 × 512 BC7 atlas, `Interface_arranged000`, held in
`stage/Interface.ftx` (in `DRACRO.CPK`) and in each language's `stage/Interface_XX.ftx` (DE, ES, FR, IT, UK, US; in
`DRACRO_US.CPK`). The language packs hold the same bytes, so one patch serves them all. The sprite definitions in
`Interface_XX.mbs` cut fixed rectangles out of the atlas (`BUTTON00`–`BUTTON46`), so painting an Xbox glyph into the same
rectangle as the PlayStation one changes the prompts without touching anything else. The launcher applies the result as
a block patch to the installed archives (`launcher/cpk_patch.h`, `launcher/controller_prompts.h`).

These scripts are development tools (Python 3.11+, `numpy`, `scipy`, `Pillow`); players never run them. None of them
is needed to build the port: the patch is committed.

| Script | What it does |
| --- | --- |
| `cpk.py` | CRI CPK reader: `list`, `header`, `extract`, `extract-matching`, `selftest` (standard library only). |
| `ftex.py` | Vanillaware FTEX packs: `list` the textures, `dump` them as PNG (GNF, BC7, Thin_1dThin tiling). |
| `draw_dcp.py` | Paints the Xbox glyphs into the decoded atlas, in place (the touchpad surface and word take the VIEW oval and label), and writes a mask of the changed pixels. |
| `paint_add_word.py` | Paints the VIEW label over the TOUCH PAD word of `Interface_Add_arranged000` (the second interface atlas), with the mask. |
| `touch_text.py` | Xbox wording of the touchpad button in the translated message sheets: the five sentences that name it get the glyph `@#(34)` that the English sentences have. Same file size; every other record is checked to be unchanged (`touch_text.py selftest` covers the checker). |
| `bc7.py` | A BC7 mode 6 encoder (numpy). |
| `draw_spider.py` | Arachnophobia Safe Mode: draws the replacement Giant Spider atlas (`launcher/prompts/arachnophobia-spider-atlas.png`, plus a preview) from the decoded original, leaving web and smoke effects; `patch_ftx.py` and `make_dcpx.py` turn that exact PNG into `launcher/prompts/arachnophobia-spider.dcpx`. |
| `patch_ftx.py` | Re-encodes only the changed 4 × 4 blocks, re-tiles them and splices them into the pack. |
| `make_dcpx.py` | Diffs the original and patched packs into the `.dcpx` block patch the launcher embeds. |
| `patch_cpk.py` | The launcher's apply/revert, as a script: `apply <folder> <patch.dcpx>`, `revert`, `status`. Same undo record format, so the launcher and the script recognise each other's work. |

## Rebuilding the patch

From the installed game (`game\prepared\v1.03`), with the Poppins Bold font (SIL OFL, fonts.google.com/specimen/Poppins)
at hand. `DRACRO.CPK` holds the shared pack, `DRACRO_US.CPK` the English and translated ones:

```
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO.CPK stage/Interface.ftx work/orig/shared
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO_US.CPK stage/Interface_US.ftx work/orig
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO_US.CPK stage/Interface_DE.ftx work/orig  # and ES, FR, IT, UK
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO_US.CPK msgsheet/SystemMsg_DE.fms work/orig  # and ES, FR, IT
python scripts/prompts/ftex.py dump work/orig/Interface_US.ftx work/atlas
python scripts/prompts/draw_dcp.py work/atlas/Interface_arranged000.png work/atlas_xbox.png --font Poppins-Bold.ttf
python scripts/prompts/patch_ftx.py work/orig/shared/Interface.ftx Interface_arranged000.gnf work/atlas_xbox.png work/atlas_xbox.changed.npy work/Interface.xbox.ftx --verify
python scripts/prompts/patch_ftx.py work/orig/Interface_US.ftx Interface_arranged000.gnf work/atlas_xbox.png work/atlas_xbox.changed.npy work/Interface_US.xbox.ftx --verify
python scripts/prompts/paint_add_word.py work/atlas/Interface_Add_arranged000.png work/atlas_xbox.png work/atlas_add_xbox.png
python scripts/prompts/patch_ftx.py work/Interface_US.xbox.ftx Interface_Add_arranged000.gnf work/atlas_add_xbox.png work/atlas_add_xbox.changed.npy work/Interface_US.xbox.ftx --verify
python scripts/prompts/touch_text.py work/orig/SystemMsg_DE.fms work/orig/SystemMsg_DE.xbox.fms  # and ES, FR, IT
python scripts/prompts/make_dcpx.py launcher/prompts/xbox-prompts.dcpx \
    "stage/Interface.ftx=work/orig/shared/Interface.ftx:work/Interface.xbox.ftx" \
    "stage/Interface_US.ftx=work/orig/Interface_US.ftx:work/Interface_US.xbox.ftx" \
    "stage/Interface_DE.ftx=work/orig/Interface_DE.ftx:work/Interface_US.xbox.ftx" \
    "stage/Interface_ES.ftx=work/orig/Interface_ES.ftx:work/Interface_US.xbox.ftx" \
    "stage/Interface_FR.ftx=work/orig/Interface_FR.ftx:work/Interface_US.xbox.ftx" \
    "stage/Interface_IT.ftx=work/orig/Interface_IT.ftx:work/Interface_US.xbox.ftx" \
    "stage/Interface_UK.ftx=work/orig/Interface_UK.ftx:work/Interface_US.xbox.ftx" \
    "msgsheet/SystemMsg_DE.fms=work/orig/SystemMsg_DE.fms:work/orig/SystemMsg_DE.xbox.fms" \
    "msgsheet/SystemMsg_ES.fms=work/orig/SystemMsg_ES.fms:work/orig/SystemMsg_ES.xbox.fms" \
    "msgsheet/SystemMsg_FR.fms=work/orig/SystemMsg_FR.fms:work/orig/SystemMsg_FR.xbox.fms" \
    "msgsheet/SystemMsg_IT.fms=work/orig/SystemMsg_IT.fms:work/orig/SystemMsg_IT.xbox.fms"
```

`draw_dcp.py` also writes `dcp_compare.png` next to its output, the atlas before and after, to check the result before
encoding. `--verify` on `patch_ftx.py` decodes the spliced pack again and reports the PSNR of the changed pixels and that
untouched pixels are untouched.

The committed patch was made by applying only the touchpad steps to the packs of the previous patch:
`draw_dcp.py ... --touchpad-only` over the already-painted `Interface_arranged000`, then `paint_add_word.py` and the same
`patch_ftx.py` / `make_dcpx.py` steps. That keeps the existing letters byte for byte, because another Pillow or FreeType
build can rasterise them slightly differently.

The patch records the CRC32 of each pack before and after, so it only ever applies to the packs it was made from
(version 1.03 of the game). Another style (Nintendo, Steam Deck) is another `draw_dcp.py` glyph table and another
`.dcpx` target set; the launcher side takes a style and a patch.

## Rebuilding the Arachnophobia Safe Mode patch

```
python scripts/prompts/cpk.py extract game/prepared/CUSA10209-1.00/DRACRO.CPK character/GiantSpider00.ftx work
python scripts/prompts/ftex.py dump work/GiantSpider00.ftx work/atlas
python scripts/prompts/draw_spider.py work/atlas/GiantSpider00_arranged000.png launcher/prompts/arachnophobia-spider-atlas.png launcher/prompts/arachnophobia-spider-preview.png work/changed.npy
python scripts/prompts/patch_ftx.py work/GiantSpider00.ftx GiantSpider00_arranged000.gnf launcher/prompts/arachnophobia-spider-atlas.png work/changed.npy work/GiantSpider00.safe.ftx
python scripts/prompts/make_dcpx.py launcher/prompts/arachnophobia-spider.dcpx character/GiantSpider00.ftx=work/GiantSpider00.ftx:work/GiantSpider00.safe.ftx
```

The atlas PNG is the only drawing path: the patch is encoded from that exact file. The preview is for looking at.

## Formats, briefly

- **CPK**: a `CPK ` section (16-byte header: magic, u32, u64 size) with an encrypted `@UTF` table of header fields
  (`TocOffset`, `ContentOffset`, `Align`, `EnableTocCrc`...), a `TOC ` section with one row per file (`DirName`,
  `FileName`, `FileSize`, `ExtractSize`, `FileOffset`, `ID`), then the data. `@UTF` tables are big-endian; some are
  XOR-encrypted (seed 0x5F, step 0x15). A file's position is `FileOffset` plus the lower of `ContentOffset` and
  `TocOffset` (at most 0x800). Entries with `FileSize < ExtractSize` are CRILAYLA-compressed: the stream is read
  backwards into the output backwards, and the file's first 0x100 bytes are stored raw after it.
- **FTEX**: a header (u32 at 8: first `FTX0` offset, u32 at 12: count), 0x30-byte entries from 0x20 (32-byte name,
  u32 0, u32 section size, 8 flag bytes), then `FTX0` sections (u32 size, u32 header size 0x40) each holding a GNF
  file: 0x100-byte header with the texture descriptor at +16 (format, width, height, tiling, pitch), then the data.
  Every texture is BC7, tile mode 13 (Thin_1dThin), no mipmaps.
- **DCPX**: `DCPX1\0\0\0`, u32 targets; per target u16 name length, name, u32 size, u32 CRC32 before, u32 CRC32 after,
  u32 runs, then runs of u32 offset + 16 bytes. All little-endian.
- **Undo record** (`<archive>.undo`): `DCPUNDO1`, u64 original length, u64 TOC position, u32 length + original TOC
  bytes, u64 header position, u32 length + original header bytes, u32 length + the patched entries' names.
