# Brazilian Portuguese localization

Portuguese (Brazil) is a Menu & Subtitles language for CUSA10454 USA 1.00 only. It is not offered for USA 1.01, the
Japan builds (CUSA07001), the Asia/Hong Kong builds (CUSA10209) or the Europe build (CUSA10487). The game runs in its English slot, and the port
serves translated copies of the English message sheets (`msgsheet/*_US.fms`) and replacement textures and layouts when
the game reads `DRACRO_US.CPK`. The archive on disk is never written (`launcher/game_translation.h`,
`runtime_file_overlay`).

The launcher UI, the F1 options, the F10 overlay and the online screens are translated in code
(`launcher/localization/`). The translation is based on the work of Marcelo - Uma Ficha, used with permission.

Not done yet: DefaultBone dialogue, trophy text, the remaining baked visual labels, further manual cleanup, the
credits and the Berserk terminology decision.

This folder holds only data written for the port. It never holds English, Spanish or other game text.

| Path | Contents |
| --- | --- |
| `sheets/<Sheet>.json` | Text translations, embedded into the launcher at build time |
| `visual/*.dcpx.z` | Compressed DCPX patches (textures and layouts) against the USA archive entries, embedded the same way |
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
