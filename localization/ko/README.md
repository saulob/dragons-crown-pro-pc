# Korean localization

Korean (`korean`) is a Menu & Subtitles language of the USA and European builds. It is the game's official Korean
translation, whose message sheets are the USA ones record for record. The port serves it over the English slot like
Portuguese (Brazil) and Russian, and never writes the installed archive.

| Path | Contents |
| --- | --- |
| `sheets/<Sheet>.json` | The Korean records the port serves: each record's index, the SHA-256 of the USA record it replaces, and `ko` |
| `font/hangul.glyphs.z` | The Hangul syllables the sheets use, drawn for the USA font atlas (below) |

## How the text was taken

The Korean sheets come with Japanese sheets that are the PS4 Japanese ones (`DRACRO.CPK`) record for record, and the
USA sheets are index-aligned with those. A USA record takes the Korean record at its index when the two Japanese records
there are the same text (all of them are).

| Sheet | Records | Korean |
| --- | --- | --- |
| EventMsg | 1,045 | 1,045 |
| SystemMsg | 1,611 | 1,494 (the tag keys 109–225 are never translated) |
| CustomItem | 5,238 | 5,238 |
| DefaultBone | 2,049 | 2,049 |
| AbilityList | 807 | 807 |
| ItemList | 740 | 740 |
| ChallengeQuest | 200 | 161 (the Korean sheet stops there) |
| others | 424 | 419 (every record that is not empty) |

The Korean text words its `##(...)` and colour tags its own way, so, as for Russian, a record must share only the USA
record's printf conversions and button glyphs (`SameArguments`); every tag it uses exists on the PS4 builds. Eleven
records show a button or icon the USA record does not (or the reverse); `FIXES` in `korean.py` edits them to match. The
39 challenge quests the Korean sheet does not have stay in English. The staff roll is not translated, as in the other
languages.

## Letters

The USA font atlas has no Hangul. Like the official Korean font, which keeps the ASCII and kana cells and puts its
Hangul where the Japanese font has kanji, the launcher draws each syllable the sheets use into an atlas cell that only a
kanji uses (`launcher/hangul_font.cpp`), and the loader adds their code pages (U+AC00–U+D7A3) to the executable's glyph
table (`src/hangul_glyphs.c`), which checks that every cell is listed by kanji only. The syllables are drawn with Noto
Sans CJK KR (weight 700) to the kanji's box: full width 60, spacing 2, baseline at y = 60 of the 68 px cell, BC7.

The character messages are drawn by the system font, not the atlas: while Korean is selected, the European system font
set opens the embedded Noto Sans KR instead of Noto Sans (`src/runtime_font.c`).

## Rebuilding

```
python scripts/localization/korean.py map --source DIR --game game/prepared/v1.03   # sheets/
python scripts/localization/korean.py glyphs --game game/prepared/v1.03             # font/hangul.glyphs.z
```

DIR holds the Korean message archives from the user's own copy: `DRACRO_PATCH_CK.CPK` (`msgsheet/*_KR.fms`) and
`DRACRO_PATCH.CPK` (the Japanese sheets they are aligned with). They are read only, and nothing else of them is stored.
