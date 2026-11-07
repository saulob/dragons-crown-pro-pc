# English for Japan and Asia/Hong Kong

The Japanese and Asia/Hong Kong builds have no English slot, but English is always listed there. The PC port ships the
game's English text and serves it over the Japanese or Traditional Chinese slot like Portuguese (Brazil): the player
needs no other game and no setup. The USA and European builds play their own English slot under the same `english` key.

| Path | Contents |
| --- | --- |
| `sheets/<Sheet>.json` | The English records the port serves: each record's index, SHA-256 and text |
| `sources/US.json` | The 16-hex SHA-256 prefix of every English record, by sheet and index |
| `sources/<slot>.json` | The slot's map as a delta over `localization/pt-BR/sources/<slot>.json`: the record hashes English adds, and the records it cannot take (`excluded`, with why) |

A record is excluded when its printf conversions, button glyphs or tags differ from the slot's, or when its English uses
a character the slot's own text never uses (the font may not have it); it keeps the slot's text.

```
python scripts/localization/english.py extract --pkg package.pkg   # read the USA package into cache/ only
python scripts/localization/english.py generate                    # write sheets/ from the extracted corpus
python scripts/localization/english.py validate                    # the corpus and its maps; no package needed
python scripts/localization/english.py sources                     # rewrite sources/ from the prepared games
python scripts/localization/english.py parity                      # coverage and readiness, per build
```

`extract` accepts `--game DIR` for a folder that already holds `DRACRO_US.CPK`. The extracted sheets stay under
`cache/` and are never tracked; only `sheets/` and `sources/` are.
