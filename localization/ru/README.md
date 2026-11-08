# Russian localization

Russian is a Menu & Subtitles language of every supported build. It is the fan translation by
[Exclusive Studio](https://ex-s.ru/dc.html), ported to the PS4 resources by AlchemistLab, as released in their
CUSA10454 update package (version 0.9.0, 27.07.2020). It is not an official localization, and the PC port did not
translate it. See also the [Zone of Games page](https://www.zoneofgames.ru/games/dragons_crown/files/13583.html) and the
[discussion](https://forum.zoneofgames.ru/topic/49567-dragon-s-crown/).

The player needs no fan package: the port serves this corpus over the build's own slot, like Portuguese (Brazil) and
the built-in English, and never writes the installed archive.

| Build | Slot | Records of the 9,946 applied | Letters |
| --- | --- | --- | --- |
| CUSA10454 1.00 USA | English | 9,946 | over the kana cells the fan package redraws |
| CUSA10454 1.01 USA | English | static: the fan update is this build | same cells, already drawn by the update |
| CUSA07001 1.00, 1.02, 1.03 Japan | Japanese | 9,926 | blank cells after the Portuguese letters |
| CUSA10209 1.00 Asia/Hong Kong | Traditional Chinese | 9,791 | blank cells after the Portuguese letters |
| CUSA10209 1.02 Asia/Hong Kong | Traditional Chinese | static: no local archive | blank cells after the Portuguese letters |
| CUSA10487 1.03 Europe | English | static: no local archive | over the kana cells, when the font is the USA one |

## Credits

Translation: Exclusive Studio

- Leader: CeHbKA
- Editing: djonmarvel, Melissan
- Artist: slavik274
- Programming: Fothsid, djonmarvel
- Resource research: Fothsid, djonmarvel, eol, X_tra
- Translators: ElenaM, Melissan, udeaff_34, SanfordSallah, natali336, okitan
- Texture translation: djonmarvel, Melissan

Port of the translation to the PSV/PS3/PS4 resources, and font corrections: AlchemistLab

## How the fan package writes Russian

The package's main font draws Cyrillic in the cells of 66 kana, and its sheets write each letter as that kana:
U+3060-U+3080 are the capitals and U+30A9-U+30C9 the small letters, in alphabetical order except that the soft sign
comes before the hard sign (Ь Ъ Ы). Its executable sets those kana's widths. Text the system font draws (the
character messages and a few SystemMsg records) is real Unicode there.

The corpus here is Unicode throughout. `russian.py extract` decodes a run of kana only when every character in it is
a redrawn one, so the Japanese in the developer labels of EventMsg stays Japanese. On the USA and European builds the
port writes the letters back as those kana and draws the fan glyphs into the same cells; elsewhere the letters keep
their codes and get a page of their own in the executable's glyph table (`src/cyrillic_glyphs.c`). The OpenType fonts
the system font uses already have Cyrillic in every region.

Left out of the corpus, so those records keep the build's own text:

- the 1,635 DefaultBone bone names and the staff roll: the package writes them for its name font, with Cyrillic in
  place of ASCII punctuation;
- SystemMsg 746, the gold counter: the package redraws the middle font's `t` and `v` for it;
- SystemMsg 100 and 1473, which drop a button glyph, and SystemMsg 1374, which adds a printf conversion;
- the 407 records the package leaves in English (mostly developer placeholders) and the tag keys.

A lone `%` in text the game formats is written as `％`, as the English does. Three character messages are 31 UTF-16
units long; a character's message field keeps 30.

The package has no trophy text (its trophy file is the original), no texture or layout changes and no launcher text:
the launcher, the F1 options, the F10 overlay and the online screens stay in English.

| Path | Contents |
| --- | --- |
| `sheets/<Sheet>.json` | `index`, `source_sha256` (the English record it translates), `ru`, and `system_font` where the package keeps Unicode |
| `sources/<slot>.json` | The slot's map as a delta over the English one (`localization/en/sources`): records Russian adds, and records it cannot take |
| `font/cyrillic.glyphs.z` | The 66 letters as the fan font draws them: code, kana, width and spacing bytes, and the cell's BC7 blocks |

## Workflow

```
python scripts/localization/russian.py extract --pkg PACKAGE.pkg   # read-only; only cache/localization/ru/ is written
python scripts/localization/russian.py generate                    # sheets/ and font/, from the extracted corpus
python scripts/localization/russian.py sources                     # sources/, from the prepared Japanese and Chinese games
python scripts/localization/russian.py validate                    # the corpus, glyphs and maps; no game needed
python scripts/localization/russian.py parity                      # every build's coverage and font fit
```

`extract` needs the repository's Python environment (`tools/setup.ps1 -InstallPython`). It refuses any package that is
not the fan update (CUSA10454, APP_VER 01.01, executable `434b3bb4...`).
