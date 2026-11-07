# Brazilian Portuguese localization

Portuguese (Brazil) is a Menu & Subtitles language of every supported build. The game runs in a native slot, and the
port serves translated copies of that slot's message sheets when the game reads its archive. The archive on disk is
never written (`launcher/game_translation.h`, `runtime_file_overlay`).

| Build | Archive | Slot sheets | Records of the 9,670 applied | Art |
| --- | --- | --- | --- | --- |
| CUSA10454 1.00 USA | `DRACRO_US.CPK` | `msgsheet/*_US.fms` | 9,670 | replaced |
| CUSA10454 1.01 USA | `DRACRO_US.CPK` | `msgsheet/*_US.fms` | not measured: no local archive | replaced where the hash matches |
| CUSA07001 1.00, 1.02, 1.03 Japan | `DRACRO.CPK` | `msgsheet/*.fms` | 9,649 | labels replaced, manual the game's own |
| CUSA10209 1.00 Asia/Hong Kong | `DRACRO_CK.CPK` | `msgsheet/*_CT.fms` | 9,659 | labels replaced, manual the game's own |
| CUSA10209 1.02 Asia/Hong Kong | `DRACRO_CK.CPK` | `msgsheet/*_CT.fms` | not measured: no local archive | replaced where the hash matches, manual the game's own |
| CUSA10487 1.03 Europe | `DRACRO_EU.CPK` | `msgsheet/*_US.fms` | not measured: no local archive | replaced where the hash matches |

The Japanese and Asia/Hong Kong fonts have no accented letters. They are composed at run time from the player's own
font (`launcher/latin_font.cpp`, `src/latin_glyphs.c`); see the Menu & Subtitles section of `docs/LAUNCHER.md`.

The launcher UI, the F1 options, the F10 overlay and the online screens are translated in code
(`launcher/localization/`). The translation is based on the work of Marcelo - Uma Ficha, used with permission.

DefaultBone holds the 366 character message records (1635-1640 and the 15 variants of each class and category
in 1641-2048); names and the blocks' markers stay the game's own. Each line keeps at most 30 UTF-16 units, the
length a character's message field keeps (`ptbr.py validate` refuses longer ones).

Not done yet: trophy text, the remaining baked visual labels, further manual cleanup, the
credits and the Berserk terminology decision.

This folder holds only data written for the port. It never holds English, Spanish or other game text.

| Path | Contents |
| --- | --- |
| `sheets/<Sheet>.json` | Text translations, embedded into the launcher at build time |
| `sources/<source>.json` | For a slot not in English, the hash of each of its records that takes a translation, and why any other does not |
| `visual/*.dcpx.z` | Compressed DCPX patches against the USA archive entries, embedded the same way |
| `visual/base-*.dcpx.z` | The same art for the base stage files of `DRACRO.CPK` (Japan, Asia/Hong Kong): deltas of the labels and their layout quads, written by `scripts/localization/regional_visuals.py` |
| `glossary.tsv` | Terminology decisions: `category`, `source` (a short term, or a `##(KEY)` tag), `pt_br`, `status` (`approved` / `review`), optional `notes` |

## Sheet format

```json
{
  "sheet": "SystemMsg",
  "entries": [
    {"index": 396, "source_sha256": "<SHA-256 of the English record>", "pt_br": "Opções do sistema"},
    {"index": [10, 57], "source_sha256": "<shared English record>", "pt_br": "..."}
  ]
}
```

- **Key:** each entry is tied to the exact English record it was translated from by `source_sha256`.
- **Changed English:** if the installed English record is different, the game keeps the English text for that record and logs why.
- **Other slots:** a Japanese or Chinese record takes the translation at its index when its hash is in the slot's source map. A record whose printf conversions or button glyphs differ from the translation, or that lacks a tag the translation uses, stays in the slot's language, and the map says why.
- **Shared text:** an `index` list shares one translation between records with identical English.

## Workflow

Run these from the repository root. They need the installed game (`game/prepared/v1.03`). Working files go to
`cache/localization/pt-BR/`, which git ignores.

```
python scripts/localization/ptbr.py batches                 # context batches and progress
python scripts/localization/ptbr.py extract --batch SystemMsg-02
#   fill "pt_br" in cache/localization/pt-BR/work/SystemMsg-02.json
python scripts/localization/ptbr.py merge cache/localization/pt-BR/work/SystemMsg-02.json
python scripts/localization/ptbr.py validate                # must end with 0 errors
python scripts/localization/ptbr.py generate                # optional: the sheets as the game will read them
python scripts/localization/ptbr.py sources                 # after a corpus change: the slot maps, from game/prepared
python scripts/localization/ptbr.py parity                  # every build's sheets against the corpus and the maps
```

## Rules

- **Source language:**
  - English is the source.
  - The working files also carry Spanish, French, German, Italian and Japanese, for context only. Use them to settle gender, number, subject and intent.
  - Never translate the Spanish.
- **Control sequences:**
  - Keep every one: `@#(N)`, `##(NAME)`, `@c(..)…@/c`, `@x/@s/@w/@y/@t(..)`, `@/t`, `@C`, `@@`, `$`.
  - Keep printf conversions such as `%d`, `%s` and `%02d`, in the same order.
  - A `%` in text without a conversion is plain text.
  - In text with a conversion, write percentages with `％`, as the English does.
- **Tag keys:** `SystemMsg` records 109-225 are tag keys and are never translated. Records 226-342 are their values.
- **Lines:**
  - Keep the line breaks deliberate.
  - Do not use more lines than any shipped language uses for that record.
- **Characters:** use only ASCII, `á à â ã é ê í ó ô õ ú ü ç` and their capitals, plus characters already present in the English record. Use the ASCII hyphen rather than typographic dashes.
- **Names:** keep proper names unchanged unless the glossary says otherwise.
