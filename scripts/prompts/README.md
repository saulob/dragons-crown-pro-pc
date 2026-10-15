# Controller prompts: how `launcher/prompts/xbox-prompts.dcpx` is made

The game draws every button prompt from one 1024 × 512 BC7 atlas, `Interface_arranged000`, held in both
`stage/Interface.ftx` and `stage/Interface_US.ftx` inside `DRACRO_US.CPK` (and `DRACRO.CPK`). The sprite definitions in
`Interface_US.mbs` cut fixed rectangles out of it (`BUTTON00`–`BUTTON46`), so painting an Xbox glyph into the same
rectangle as the PlayStation one changes the prompts without touching anything else. The launcher applies the result as
a block patch to the installed archives (`launcher/cpk_patch.h`, `launcher/controller_prompts.h`).

These scripts are development tools (Python 3.11+, `numpy`, `scipy`, `Pillow`); players never run them. None of them
is needed to build the port: the patch is committed.

| Script | What it does |
| --- | --- |
| `cpk.py` | CRI CPK reader: `list`, `header`, `extract`, `extract-matching`, `selftest` (standard library only). |
| `ftex.py` | Vanillaware FTEX packs: `list` the textures, `dump` them as PNG (GNF, BC7, Thin_1dThin tiling). |
| `draw_dcp.py` | Paints the Xbox glyphs into the decoded atlas, in place, and writes a mask of the changed pixels. |
| `bc7.py` | A BC7 mode 6 encoder (numpy). |
| `patch_ftx.py` | Re-encodes only the changed 4 × 4 blocks, re-tiles them and splices them into the pack. |
| `make_dcpx.py` | Diffs the original and patched packs into the `.dcpx` block patch the launcher embeds. |
| `patch_cpk.py` | The launcher's apply/revert, as a script: `apply <folder> <patch.dcpx>`, `revert`, `status`. Same undo record format, so the launcher and the script recognise each other's work. |

## Rebuilding the patch

From the installed game (`game\prepared\v1.03`), with the Poppins Bold font (SIL OFL, fonts.google.com/specimen/Poppins)
at hand:

```
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO_US.CPK stage/Interface.ftx work
python scripts/prompts/cpk.py extract game/prepared/v1.03/DRACRO_US.CPK stage/Interface_US.ftx work
python scripts/prompts/ftex.py dump work/Interface_US.ftx work/atlas          # Interface_arranged000.png among others
python scripts/prompts/draw_dcp.py work/atlas/Interface_arranged000.png work/atlas_xbox.png --font Poppins-Bold.ttf
python scripts/prompts/patch_ftx.py work/Interface.ftx    Interface_arranged000.gnf work/atlas_xbox.png work/atlas_xbox.changed.npy work/Interface.xbox.ftx    --verify
python scripts/prompts/patch_ftx.py work/Interface_US.ftx Interface_arranged000.gnf work/atlas_xbox.png work/atlas_xbox.changed.npy work/Interface_US.xbox.ftx --verify
python scripts/prompts/make_dcpx.py launcher/prompts/xbox-prompts.dcpx \
    "stage/Interface.ftx=work/Interface.ftx:work/Interface.xbox.ftx" \
    "stage/Interface_US.ftx=work/Interface_US.ftx:work/Interface_US.xbox.ftx"
```

`draw_dcp.py` also writes `dcp_compare.png` next to its output, the atlas before and after, to check the result before
encoding. `--verify` on `patch_ftx.py` decodes the spliced pack again and reports the PSNR of the changed pixels (about
34 dB with mode 6 alone) and that untouched pixels are untouched.

The patch records the CRC32 of each pack before and after, so it only ever applies to the packs it was made from
(version 1.03 of the game). Another style (Nintendo, Steam Deck) is another `draw_dcp.py` glyph table and another
`.dcpx` target set; the launcher side takes a style and a patch.

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
