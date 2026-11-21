<div align="center">

# Dragon's Crown Pro - PC Port

### Dragon's Crown Pro, Vanillaware's PS4 side-scrolling beat 'em up, running natively on Windows, and experimentally on Linux and Steam Deck, with a new Boss Battle mode, Online Co-op, the TRG launcher and modern graphics and display options

[![Latest release](https://img.shields.io/github/v/release/saulob/dragons-crown-pro-pc?style=for-the-badge&label=release&color=e0bb32&labelColor=07101f&include_prereleases&cacheSeconds=300)](https://github.com/saulob/dragons-crown-pro-pc/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/dragons-crown-pro-pc/total?style=for-the-badge&color=e0bb32&labelColor=07101f&cacheSeconds=300)](https://github.com/saulob/dragons-crown-pro-pc/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-00acc1?style=for-the-badge&labelColor=07101f)

<sub>The original PS4 game code runs on the PC with a replacement for the PS4 system libraries and a Vulkan renderer derived from <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>. <b>The game itself is not included</b>: bring your own legally obtained Dragon's Crown Pro PS4 package.</sub>

</div>

<img width="1941" height="1156" alt="2026-10-14_23-43" src="https://github.com/user-attachments/assets/984433e5-1bd8-498a-949c-68815e73b901" />

## Features

- **Character Editor** (new, exclusive to this PC port): on the launcher's **Editor** page, pick a save and a character, then recolor individual parts of its appearance, hide supported parts, or edit its name, level, skill points and HP. Each saved character keeps its own look, and other players with a compatible version see it online. See [Editor](#editor)
- **Comms Wheel (Quick Chat)** (new): eight quick messages for online rooms (Ready, Hello, Need help, Let's go, Group up, Follow me, Good job, Thanks), shown in the game's own greeting balloon, from a controller or the keyboard, with no typing or voice chat. See [Comms Wheel (Quick Chat)](#comms-wheel-quick-chat)
- **Boss Battle** (new, exclusive to this PC port): fight any of the game's **20 bosses** straight from the Inn, or all of them in a row with **Fight All**, with your campaign protected and your own attempt statistics. See [Boss Battle](#boss-battle)
- **Unlock Labyrinth of Chaos** and **Unlock Ultimate + Tower of Mirages**: two independent options that open the game's endgame content early, once the Stables are open
- **Mouse Controls**: attack with the mouse buttons, give any mouse button any controller action (Cross, the shoulders, the D-pad and more), and hold **Pointer Control** (Middle Mouse), or choose **Always On** for a trackpad such as the Steam Deck's, to move the game's own red hand pointer with the mouse, with an adjustable **Mouse Sensitivity**
- **Online Co-op**: play the game's own online multiplayer over the Internet, with Friend Codes, friend requests, an optional public **Lobby** to meet other players, and Join a Friend's room. See [Online Co-op](#online-co-op)
- **Regions**: the US (`CUSA10454` 1.00 and 1.01), Japanese (`CUSA07001` 1.00, 1.02 and 1.03) Asia / Hong Kong (`CUSA10209` 1.00 and 1.02) and European (`CUSA10487` 1.03) PS4 releases. European support is new and experimental: it comes from one reporter's package and has not been played yet
- **Native PC port**: the game's own x86-64 code runs directly on the PC. It is not an emulator. Windows is the primary tested platform, and there is an experimental native Linux / Steam Deck AppImage with the same intended feature set. Linux needs no Proton or Wine.
- **TRG launcher** that installs the game directly from your own `.pkg` file, with no Python or other tools needed, and remembers its window position
- **Windowed** and **Borderless Fullscreen**, switched with **F11** while playing, with window size and monitor selection
- **Graphics card selection**: any Vulkan graphics card the renderer can use
- **VSync** on or off
- **Render resolution** from 480p to 4K (3840×2160), with the game's native 1080p as the default
- **Aspect Ratio**: 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3 and 5:4
- **Experimental True Ultrawide** (off by default): 21:9 and 32:9 show more of the world at the sides on the supported builds
- **Frame rate** option: 30, 60 (Original, default), 90, 120, 144, 165, 240 or Unlimited; the game's speed stays at 60 and frames above 60 repeat the last frame
- **Local PS4 trophies**, offline on this PC, shown on the launcher's Trophies page
- **SMAA** anti-aliasing and **anisotropic texture filtering** (Game, 2x, 4x, 8x, 16x)
- **AMD FidelityFX Super Resolution 1** upscaling and **RCAS sharpening** with a strength setting
- **Menu & Subtitles language**: English, French, German, Italian or Spanish on the USA and European builds; Japanese on the Japanese builds; Traditional Chinese or Korean on the Asia/Hong Kong build; English on the Japanese and Asia/Hong Kong builds (built into the port); Brazilian Portuguese and Russian on every supported build
- **Story Skip Offer** and **Tutorial Skip Offer**: the game's own offers to skip the tutorial and start a new character in the second half of the story
- **Unlock Gallery**: an option to see every Gallery picture without changing your save
- Frame rate in the window title, and an in-game **frame counter** (frames per second and frame time, **F2**)
- Xbox controllers without setup, and keyboard support, with **Xbox button prompts** as an option (PlayStation prompts are the default)
- Local multiplayer with up to four controllers; the cheats apply to every local human player, Players 1 to 4
- Portable `user` and `logs` folders next to the program for settings, saves, the shader cache and logs
- **A PC title menu**: Start Game, Options, Gallery and **Exit Game**, with the game's own menu art and animations. Exit Game is in the game's language and closes the game through the same clean shutdown as closing the window (Alt+F4 still works). The PlayStation Network **Upload Save Data** and **Download Save Data** rows are not shown, because the PlayStation Network save transfer is not available in the PC port.
- **Brazilian Portuguese**: game text, menus, subtitles, launcher and F1 options on every supported build. New characters get Portuguese default messages, previewed while they are created; characters already in a save keep their stored data, but their native default messages are shown in Portuguese and any message you wrote yourself stays untouched. Credits, trophies and some baked labels stay in the build's own language, and the translated manual pages are shown on the USA and European builds only
- **Russian**: game text, menus, subtitles and character messages on every supported build, from the fan translation by [Exclusive Studio](https://ex-s.ru/dc.html) with PS4 porting work by AlchemistLab. No fan package is needed. The launcher, F1 options, F10 overlay and online screens stay in English; random adventurer names, credits, trophies and the gold counter stay in the build's own language. See [localization/ru/README.md](localization/ru/README.md)
- **Discord Rich Presence**: your class, level, current place and party in your Discord status, through the Discord desktop app. On by default; `discord_presence = off` in `user\dcp.ini` turns it off. See [docs/DISCORD.md](docs/DISCORD.md)
- **Bug Report page**: send a report with a filtered log from the launcher and follow its status under **My reports**. See [Reporting problems](#reporting-problems)
- **Experimental renderers**: OpenGL, and on Windows Direct3D 11, for systems that cannot run Vulkan. They render only part of the game's features; Vulkan stays the default and the supported renderer
- **Update check**: the launcher looks for a newer release of the port at start and installs it only when you choose to; the game files and the `user` folder are kept

## Boss Battle

Boss Battle is a new mode made for this PC port. It is not part of the original game's flow: pick a boss, fight it right away, and come back to the Inn, without playing through a stage first.

- **Turn it on** with **Enable Boss Battle** on the launcher's **Gameplay** page (or in the F1 Options menu). The Dragon's Haven Inn menu then gains a **Boss Battle** row.
- **20 bosses**: choose any of them from the Boss Battle screen, in the game's own menu style, sounds and language. Prepare your party first with the Inn's own Select player and Choose allies.
- **Fight All**: the twenty bosses one after the other, without going back to the Inn between them. A death, quitting or the last win ends the run.
- **Your campaign is protected**: the save on disk is set aside during a fight and the campaign is put back afterwards, so no boss clear, story progress, route, item or gold from a Boss Battle reaches it. Experience, levels and skill points your character earns in a win are kept.
- **Straight back to the Inn**: a win shows the game's experience award and level-up, then returns to the Boss Battle screen without the results and loot screens. In a fight, **Start** opens the pause menu with **Quit Boss Battle**. **Return to tavern** closes the screen.
- **Statistics**: attempts, wins, deaths, quits, win rate and best time for every boss and character, and runs, clears, best progress and best time for Fight All, kept in `user\boss_battle.dat`.
- **Online (experimental)**: with Online Play on, the host opens Boss Battle and friends join the host's Boss Battle room from the Network Menu; the host chooses the boss and starts the fight. Only the host needs the option, and each player keeps their own campaign and statistics.
- Available on the USA (1.00, 1.01), Japanese (1.00, 1.02, 1.03) and Asia / Hong Kong 1.00 builds. The Asia / Hong Kong 1.02 and European 1.03 builds do not offer it yet, because their executables could not be checked.

See [docs/LAUNCHER.md](docs/LAUNCHER.md) and [docs/ONLINE.md](docs/ONLINE.md#boss-battle-online-experimental) for the details.

## Online Co-op

Dragon's Crown Pro includes Online Co-op. Add your friends with a Friend Code, then meet them in the game and play stages together over the Internet, as on the PS4. No PlayStation Network account is needed.

- The port's online service (running on Cloudflare) handles your online identity, Friends and friend requests, who is online, and finding and joining rooms.
- The gameplay itself goes **directly between the players' PCs** (peer-to-peer). Only when two players cannot connect directly does the online service relay that one connection, automatically and only if it offers relay; players who connect directly never use it.
- Players on different supported game builds and regions can play together, as long as their port versions use the same online protocol. A friend whose version cannot join yours is shown as **Incompatible**.

> **Online play is unlocked by the game, not at the first start.** Dragon's Crown does not offer online multiplayer to a new character right away. It becomes available only after the opening tutorial and story section. If the online options are missing early in a new save, that is the game's normal progression, not a problem with the port.
>
> To reach Online Co-op faster, turn on **Story Skip Offer** and **Tutorial Skip Offer** on the launcher's **Gameplay** page before creating a new character. The game then lets that character skip the tutorial and start from the second half of the story. Existing characters are not affected.
>
> **Early Story Online** (experimental, Off by default, on the launcher's **Online** page) is a different approach: it opens online play for the host right after the opening tutorial, so the first story stage can be played with friends, with the host's story shared and every guest keeping their own saved progress. Only the host needs the option. Early story rooms are friends/join-code only. It is untested in real multiplayer sessions, so treat it as experimental.

### Playing online

1. In the launcher, open the **Online** page and turn **Online play** On.
2. Choose your **Online name**, the name other players see.
3. Share **Your friend code** with the friends you want to play with (**Copy** puts it on the clipboard). Get theirs too.
4. On the **Friends** tab, enter your friend's code and click **Send request**. Your friend sees a **New friend request** card on their **Friends** tab and clicks **Accept**. You now appear in each other's **Friends** list, with whether you are online. To meet new players, turn on **Show me in Lobby** on the **Lobby** tab: it is Off until you choose it, shows your Online name to other players with it on, and lets you send each other friend requests. It is not chat or matchmaking.
5. Click **Run test** under **Connection test**. It is needed once before **PLAY** while Online play is On: it checks the online service and the UDP port the game uses for multiplayer, and on Windows it gives Windows Firewall the chance to ask for network access before the game starts (choose **Allow access** if it does). PLAY becomes available when the test finishes, whatever the result. **Ready for online play** means other players can reach you. Turning Online play Off resets this requirement.
6. Press **PLAY**.
7. Complete the opening tutorial and story section, or skip them with the offers above, until the game's online options are available.
8. Use the game's normal online flow when setting out from the Stables, as on the PS4. One player hosts.
9. The other player picks the host's room in **Join Friends Room**. **Join Random Room** finds only hosts who are already inside a stage.

Both players need **Online play** On. With it Off, the game runs offline exactly as before.

**Testing notes**
- Real-world testing has so far focused on two-player sessions. Dragon's Crown supports up to four players in a room, but four-player online on the PC port has not been fully validated yet.
- Online play on Linux and Steam Deck is experimental: it uses the same code as Windows, but it has not yet been tested on real Linux or Steam Deck hardware (see [Linux and Steam Deck](#linux-and-steam-deck)).

### Comms Wheel (Quick Chat)

- **Controller**: press **D-pad Up** to open the wheel, pick a message with the right stick, **Cross (A)** sends it and **Circle (B)** closes the wheel.
- **Keyboard and mouse**: press **T** (rebindable on the Controls page), pick with the mouse or the direction keys, send with the left mouse button, cancel with Escape or the right mouse button.
- It opens with Online Play on, in a room of at least two players. The message appears in the sender's greeting balloon, in each player's own Menu & Subtitles language. One message every 2 seconds per player.
- Only a message number travels, never text. The online service counts how often each of the eight messages is used, with no player, room or time attached. See [docs/ONLINE.md](docs/ONLINE.md#comms-wheel).

### Ping and frame counter

- **F2** shows or hides the frame counter (frames per second and frame time).
- **F3** shows or hides the **Ping** while Online play is on. **Ping display** on the Online page shows it from the start.
- The Ping is the round trip to the other players over the direct connection, the one gameplay uses. With several players it shows the slowest connection. It is not the time the online service takes to answer. `Ping: --` means no other player is connected yet.

### Cheats and Online play

Cheats are **automatically disabled while Online play is on**. This is intentional, so online sessions are played without them. Turning Online play on switches every saved cheat Off; the Cheats page and the **F10** menu show "Cheats are disabled while Online Play is enabled". Turn Online play off to use cheats again, and turn the cheats you want back on.

### Online troubleshooting

- **The online options are missing in the game**: see the note above. They appear only after the opening tutorial and story section.
- **"Cannot reach the online service"**: check the Internet connection and that a firewall is not blocking the program.
- **Friends cannot join, or the connection test says "Limited"**: the game connects players directly. To get through home routers, the port uses STUN, UDP hole punching and, where the router allows it, UPnP port mapping (UDP port 3658 by default). Allowing the program through your firewall, or turning on UPnP in your router, can help. When both players are behind strict NATs (some mobile, campus or provider networks), the connection goes through the online service's relay instead. Relay is automatic; the Connection test shows "Relay: Available" when the online service offers it.
- **A friend shows Incompatible**: one of you needs another port version. Update both to the latest release.
- For a problem report, attach the log as described in [Reporting problems](#reporting-problems). The log includes the Connection test details.

Technical details are in [docs/ONLINE.md](docs/ONLINE.md).

### Acknowledgements

Online Co-op was implemented for this project with valuable technical research and lobby design references from [OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) by Nezara.

OpenCrown's research helped document Dragon's Crown Pro's online interfaces, while its published lobby design also influenced parts of this project's reconnect behavior, room handling and service limits.

This project's implementation was written separately and does not include copied OpenCrown source code, but its published research and design work were important references during development.

### Privacy

Discord Rich Presence talks only to the Discord desktop app on your PC. Discord then shows your class, level, current place and party to whoever can see your status. Add `discord_presence = off` to `user\dcp.ini` to turn it off.

The online service stores anonymous aggregate usage statistics, such as player counts, sessions, the port version, the platform and the country. These statistics do not store player names, Friend Codes, account IDs or IP addresses; the one exception is Early Story Online, for which the service keeps the Online name, account ID and use counts of each account that hosted or joined an Early Story room. Gameplay stays direct between the players (P2P), except for a pair that cannot connect directly, whose traffic the service forwards without storing or reading it; gameplay activity, saves, Ping and FPS are not collected, apart from an anonymous count of which character classes appear in online parties (classes and party size only, no names or identifiers). To find friends and connect you, the service also keeps your account, Online name and friends, and while you are in a room it uses your address and port. See [docs/ONLINE.md](docs/ONLINE.md#privacy-and-usage-statistics).

The launcher's Bug Report page sends a report only when you press Send report: your description, the version, game build and operating system, and one game log you choose, filtered first. Open preview shows exactly what goes out. No personal user data is intentionally included, and automatic filtering cannot promise to catch everything, so check the preview if you want to be sure. Your account and address are used for the report limits and are not stored with the report. My reports on the same page shows what became of the reports sent from this PC; that list exists only on this PC. See [docs/ONLINE.md](docs/ONLINE.md#bug-reports).

## What you need

- Windows 10 (version 1903 or newer) or Windows 11, 64-bit
- **Or, experimental:** 64-bit Linux on x86-64, including the Steam Deck, on an X11 or Wayland desktop, with glibc 2.31 or newer and a Vulkan 1.3 loader (`libvulkan.so.1`) and graphics driver (see [Linux and Steam Deck](#linux-and-steam-deck)). The game has been run on Ubuntu 24.04 (WSL2), and community testers report it working on Bazzite (KDE Plasma, Wayland, AMD RADV) and on a Steam Deck LCD; other distributions that are new enough may work
- A processor with **AVX**: Intel Core 2nd generation (Sandy Bridge), AMD FX or newer. The game's PS4 code runs directly on it; LZCNT, MOVBE and BMI1, which some of these lack, are handled in software. On a processor without LZCNT, the USA 1.01 and Asia and Hong Kong 1.02 builds start only when their LZCNT instructions are exactly the ones of the other builds, which the game checks at start
- A graphics card and driver with **Vulkan 1.3**, `VK_EXT_robustness2`, `VK_KHR_push_descriptor` and `VK_EXT_vertex_attribute_divisor` on either platform (tested on an NVIDIA GeForce RTX 4070 and Intel UHD Graphics 730 on Windows)
- **Your own legally obtained Dragon's Crown Pro package**: a decrypted PS4 `.pkg` file of this exact game version, and about 6 GB of free space for the installed game:

| | |
| --- | --- |
| Game | Dragon's Crown Pro |
| Platform | PlayStation 4 |
| US builds | Title ID `CUSA10454`, Content ID `UP2611-CUSA10454_00-DRAGONSCROWNPRO1`: version 1.00 is the base package (`gd`, APP_VER `01.00`, version `01.03`, `eboot.bin` SHA-256 `f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a`); version 1.01 is an update (`gp`, APP_VER `01.01`, version `01.03`, SHA-256 `434b3bb43f407990f7b861ed57b8007ba459d63af25b1511ce7489be94c908c0`) installed over 1.00 |
| Japan builds | Title ID `CUSA07001`, Content ID `JP0005-CUSA07001_00-DRAGONSCROWN0001`: version 1.00 is the base package (APP_VER `01.00`, `eboot.bin` SHA-256 `e838e5c551f4ea2d13a13246dcd5474a888c56aa8abf5110f1219c0ceb0ae05f`); version 1.02 is the base plus the 1.02 update (APP_VER `01.02`, SHA-256 `e512685aef4fa1e50eafec8c9699b611992356cbae84e9b0147febceb49843fe`); version 1.03 is the base plus the 1.03 update (APP_VER `01.03`, SHA-256 `dccfd331b02d462f6586330664b230a887928ed1b3909f8625b49623630d1a64`) |
| Asia and Hong Kong builds | Title ID `CUSA10209`, Content ID `HP0177-CUSA10209_00-DRAGONSCROWN0001`: 1.00 is the base package (`gd`, APP_VER `01.00`, version `01.02`, `eboot.bin` SHA-256 `d37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b`); 1.02 is an update (`gp`, APP_VER `01.02`, version `01.00`, SHA-256 `927a3d0f73284b52e87e6f22dc4ecbea6350450182a94c1dc20d76907b086ac5`) installed over 1.00 |
| Europe build | Title ID `CUSA10487`, Content ID `EP2475-CUSA10487_00-DRAGONSCROWNPRO1`: 1.03 is one complete package (`gd`, APP_VER `01.00`, version `01.03`, `eboot.bin` 22283899 bytes, SHA-256 `25c58f473337bc1da768d86e4cd12fe8a15e4af48e0826ac9a857fd9e3c09845`), with the regional archive `DRACRO_EU.CPK` and no update. This is the one package known so far: a copy dumped or re-wrapped differently has another hash and is refused until it is reported |

The Japanese 1.00 base package is a complete game on its own. The 1.02 and 1.03 updates are installed over that base, as are the USA 1.01 update over its 1.00 base and the Asia and Hong Kong 1.02 update over its 1.00 base, each into its own folder; the 1.03 update replaces every file the 1.02 update changes. The Japanese and Asia and Hong Kong builds are not yet fully tested. Every build has the cheats, True Ultrawide and the Xbox controller prompts; on the builds other than USA 1.00 the cheat, NPC voice and True Ultrawide sites are found in the build's own executable, and the cheats are not yet tested in game there. Every build supports the custom Render resolution and Aspect Ratio options; the USA 1.01 and Asia and Hong Kong 1.02 builds find their render size sites by signature. Both Asia and Hong Kong builds offer Traditional Chinese and Korean text. Traditional Chinese uses the bundled Noto Sans TC font; Korean is not yet tested in game, and that font has no Korean characters. The render resolution and Aspect Ratio patches use tables checked against each build's `eboot.bin`, and are not yet tested in game.

The launcher checks the Title ID, Content ID and versions in the package's `param.sfo` before it unpacks anything, and the `eboot.bin` hash before it installs the result. Any other version is refused, because the port is built for this exact game code. A package encrypted for a PS4 console cannot be unpacked on a PC.

The Linux package is the same game and launcher, so the package requirements are the same.

## Installing

1. Download **DragonsCrownProPC-v1.4.0-windows-x64.zip** from the [latest release](https://github.com/saulob/dragons-crown-pro-pc/releases/latest) and extract it anywhere you can write to.
2. Run **dragons_crown_pro.exe**. The launcher opens.
3. On the **Play** page click **Install game...** and select your own Dragon's Crown Pro `.pkg` file.
4. Wait until **Ready to play** appears. The package is unpacked on your PC into the `game` folder next to the program (about 6 GB). The `.pkg` file is only read, and you can delete it afterwards.
5. Press **PLAY**.

**The game itself is not included**: the release contains no game package or executable, and the port does not download the game. It does include the localization resources used for its additional languages (see [Credits](#credits)).

**Reinstall game...** replaces only the installed game files and keeps your settings, saves, shader cache and logs. If an installation fails or is cancelled, the previous one is left as it was.

### Saves and settings

Everything is kept next to the program, so copying the whole folder to another PC takes your data with it:

| Folder | Contents |
| --- | --- |
| `user\dcp.ini` | Launcher and game settings |
| `user\savedata\` | Your saves |
| `user\boss_battle.dat` | Your Boss Battle statistics |
| `user\trophy\` | Your trophy progress and the trophy list and icons unpacked from your game |
| `user\keys.json` | Your Trophy Key, if you configure one |
| `user\cache\CUSA10454\` | Shader cache, so later starts load faster |
| `logs\` | One `dragons_crown_pro_NNN.log` per run |
| `game\` | The installed game (unpacked from your own package) |

These folders are created the first time they are needed and are not part of the download. When updating the port, keep `user` (and `game`, to skip reinstalling).

### Linux and Steam Deck

**Experimental.** This is a native Linux build, not Proton or Wine, and it is intended to provide the same functionality as Windows: the same launcher, installer, display and graphics options and controls; the only Windows-only option is Allow overlay programs. "Experimental" is about testing: the maintainer has no real Linux PC or Steam Deck. Community testers have reported it working on Bazzite (KDE Plasma, Wayland, AMD Radeon RX 9070 XT with RADV, 4K borderless fullscreen) and on a Steam Deck LCD ([issue #5](https://github.com/saulob/dragons-crown-pro-pc/issues/5)). It was also validated automatically under WSL2 with Ubuntu 24.04, in X11 and Wayland environments with a software Vulkan driver: the launcher, installing a package, the game's first screens, saves and the shader cache. Broader testing is still needed on NVIDIA and Intel Vulkan drivers, other distributions and the Steam Deck OLED, gamescope and Gaming Mode, performance, and long sessions. Started without a graphical session (neither `DISPLAY` nor `WAYLAND_DISPLAY` set), the game prints an error and exits instead of crashing. Please report Linux-specific problems with the log (see [Reporting problems](#reporting-problems)).

1. Download **DragonsCrownProPC-v&lt;version&gt;-x86_64.AppImage** from the [latest release](https://github.com/saulob/dragons-crown-pro-pc/releases/latest) and make it executable (right-click > **Properties** > **Permissions** > **Is executable**, or `chmod +x`).
2. Run it. The launcher opens.
3. On the **Play** page click **Install game...** and select your own Dragon's Crown Pro `.pkg` file, exactly as on Windows. The launcher uses the desktop's file chooser (`zenity` or `kdialog`) when there is one, and its own file chooser otherwise, which works with the mouse, the keyboard and a controller. Wait until **Ready to play** appears, then press **PLAY**.
4. `game`, `user` and `logs` are kept next to the AppImage file when that folder is writable (in `$XDG_DATA_HOME/DragonsCrownProPC` otherwise; `DCP_PORTABLE_DIR` chooses another folder). The AppImage itself is never written to.

An AppImage normally mounts itself with FUSE. Where that is not possible it unpacks itself into a temporary folder and runs from there automatically; `APPIMAGE_EXTRACT_AND_RUN=1` forces that mode.

To bring the launcher back after turning **Show this launcher** off, hold **Shift** while starting (in an X11 session) or start the AppImage with `--launcher`. The AppImage's desktop entry also has a **Show launcher** action. On a Steam Deck, which has no keyboard in Gaming Mode, put `--launcher` in the shortcut's **Launch Options** for a run, then remove it.

**Steam Deck:** install the game once from Desktop Mode, then add the AppImage with **Games > Add a Non-Steam Game to My Library** and start it from Gaming Mode. Nothing needs Proton. Requirements, differences from Windows and known limits are in [docs/LINUX.md](docs/LINUX.md).

**Online Co-op on Linux and Steam Deck:** the Linux build has the same Online page, Friends, rooms, direct connections, Ping and cheat lock as Windows, from the same code, and Linux and Windows players can play together. Other players see your platform as Linux or Steam Deck. This is checked by automated tests and builds only: online play on a real Linux PC or Steam Deck has not been tested by the maintainer yet, so reports are very welcome. On a Steam Deck, set up the Online page (Online name, Friend Codes) in Desktop Mode, where a keyboard is at hand.

### Windows SmartScreen

Windows may warn the first time you run the program, because the release is not digitally signed. If you downloaded it from this repository's releases page, choose **More info**, then **Run anyway**.

## The launcher

The launcher opens before the game. Your choices are saved to `user\dcp.ini`. To turn the launcher off, set **Show this launcher** to **Off** on the Play page. Hold **Shift** while starting to bring it back (on Linux, also with `--launcher`, see [Linux and Steam Deck](#linux-and-steam-deck)). The launcher works the same on both platforms (Allow overlay programs is Windows-only); on Linux the folder paths below use `/` instead of a backslash.

### Play
- **Install game...** from your `.pkg` file, with the Title ID, version and `eboot.bin` checks, and **Reinstall game...**
- The **Ready to play** status, and the **Show this launcher** setting

### Display
- **Display mode**: Windowed or Borderless Fullscreen. Press **F11** while playing to switch between them.
- **Window size** (1280×720 to 3840×2160 in 16:9; the choices follow the Aspect Ratio) and **Monitor**. A size that fits the monitor opens unchanged; one that would not fit opens smaller and centred, within its usable area. Your saved size is not changed.
- **Aspect Ratio**: native options 16:9 Original (the default), 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3 and 5:4. The game renders a frame of the ratio's shape at the Render resolution's height. Only 16:9 Original is undistorted: the game's 2D scene is still laid out for its original 1280 × 720 canvas, so 21:9 and 32:9 stretch the gameplay horizontally instead of showing more of the world (issue #1) unless True Ultrawide is on, and 16:10, 4:3 and 5:4 squeeze it. In those narrower shapes the port skips the one draw that painted black rectangles in water (issue #32); 16:9 and wider are untouched. The HUD, menus and prerendered videos are not adjusted either. Targets above 3840×2160 pixels (for example 32:9 at 4K) are reduced in height to fit.
- **True Ultrawide** (experimental, Off by default): with 21:9 or 32:9, the game shows more of the world at the sides instead of stretching it, with the same vertical view and proportions as 16:9. Distant background layers fill the whole width; the playfield and the layers that move with it stop where the stage and the playfield end, showing the background behind them. On the town facility pictures (the guilds, the shop, the inn) the character, titles and text keep their 16:9 size and place and only the room behind them is scaled up evenly to fill the sides, while their top and bottom bars, frame lines and dialogue backing reach the frame edges; the route map keeps its 16:9 composition, centred. The HUD and menus stay in their 16:9 layout; fades, the loading and saving fades and the bottom dialogue and narration bands reach the frame edges, other message bands keep their 16:9 width, and the town map fills the width (at 32:9 as far as its markers stay on screen, black beyond). Available on every supported build.
- **Presentation**: Letterbox (the default) fits the finished frame into the window with its aspect ratio kept and bars where the shapes differ; Stretch fills the whole window with it. It applies in Windowed and Borderless Fullscreen and does not change the Aspect Ratio or the render resolution.
- In Windowed and Borderless Fullscreen, mouse movement, clicks and the wheel show the cursor; keyboard or controller input hides it. Mouse-driven menus and dialogs keep it visible. Holding Pointer Control, or Pointer Control Mode Always On (see Controls), hides it while the mouse moves the game's pointer.
- **Graphics card**: the Vulkan graphics card that renders the game, chosen from the cards the renderer can use. A card that is no longer present falls back to the first available card.
- **VSync** on or off
- **Upscaling**: AMD FSR 1 enlarges the picture when the window is bigger than the render resolution
- **Sharpening** (AMD RCAS) and **Sharpening strength**

### Graphics
- **Renderer**: Vulkan (the default and the supported renderer), or experimental OpenGL, Direct3D 11 (Windows) or Auto. The experimental renderers render only part of the game's features; see [docs/GRAPHICS.md](docs/GRAPHICS.md)
- **Render resolution**: Native (1080p) 1920×1080, 480p 854×480, 720p 1280×720, 1440p 2560×1440, 1800p 3200×1800 or 4K 3840×2160. 1440p and above need a powerful graphics card.
- **Anti-aliasing**: Off or SMAA
- **Texture filtering**: Game, 2x, 4x, 8x or 16x

### Gameplay
- **Menu & Subtitles**: the language of the game's menus and subtitles: English (default), French, German, Italian or Spanish. The Japanese builds (CUSA07001) show Japanese, and the Asia/Hong Kong build (CUSA10209) offers Traditional Chinese (default) and Korean instead. The European build (CUSA10487) offers English (default), French, German, Italian and Spanish. Every build also offers Portuguese (Brazil), the port's translation, which additionally translates the launcher, the F1 options, the F10 overlay and the online screens, and Russian, a fan translation that covers the game's own text only. On the Japanese and Asia/Hong Kong builds English is always listed and is provided by the PC port itself, with no other game or setup needed. The saved `english` choice plays native English on the USA and European builds and the port's English on the others. Menu & Subtitles never changes the spoken voices: Voice Language does.
- **Voice Language**: Game Default (default), English or Japanese. The language of every spoken voice: your own characters, other players, resurrected characters and AI party members, town and event NPCs, enemies, camp voices and the narrator. Each character keeps its own voice and the narrator you picked in the game stays the same narrator; only the language changes. It does not change Menu & Subtitles. Game Default leaves the game's own choice: English on the USA and European builds; the Japanese and Asia/Hong Kong builds derive it from the game's language setting. Both languages are already in every build's installed `DRACRO.CPK`; nothing in the game files or your saves is changed, and a language whose voice files are missing from your installation is not offered. Applies at the next game start. Replaces the former Japanese NPC Voices option; a saved `dcp_japanese_npc_voices = 1` starts as Japanese
- **Skip Intro** (Off by default): skips the startup logo and goes straight to the title screen. Applies at the next game start
- **Frame rate**: 30, 60 (Original, default), 90, 120, 144, 165, 240 or Unlimited frames per second reaching the window. The game makes 60 new frames a second whatever the setting, so game speed does not change and the frames above 60 show the last frame again (no interpolation)
- **Frame rate in title**: shows the frame rate in the window title, for example `Dragon's Crown Pro | FPS: 60.00`
- **Render resolution in title**: shows the game's render resolution in the window title, for example `Dragon's Crown Pro | 1920x1080`
- **Frame counter**: shows the frame rate and frame time in a corner of the game
- **Arachnophobia Safe Mode**: redraws the Giant Spider as two soft cream bodies with two orange eyes, without legs or fangs; gameplay does not change
- **Story Skip Offer** (Off by default): when you create a new character, the game offers to start it from the second half of the story, which it normally offers only after a class on the save has finished the game. Recommended if you mainly want Online Co-op
- **Tutorial Skip Offer** (Off by default): the first character of a save is also asked whether to begin the tutorial, instead of starting it automatically. Existing characters are not affected by either offer
- **Unlock Gallery** (Off by default): shows all 57 pictures of the title menu's Gallery while it is on, without changing your save. Pictures you have not really found are only shown; turn it Off and the Gallery goes back to exactly what your save holds. Trophies and progress are not affected. Applies at the next game start
- **Enable Boss Battle** (Off by default): adds the Boss Battle row to the Inn menu (see [Boss Battle](#boss-battle)). In the F1 Options menu it applies the next time the Inn menu opens
- **Unlock Labyrinth of Chaos** (Off by default): once the character's story has opened the Stables, the Stables list the Labyrinth of Chaos from its first floor, with the game's own floors, rewards and saves
- **Unlock Ultimate + Tower of Mirages** (Off by default): once the Stables are open, the Temple offers the game's own change to Ultimate difficulty (and back to Normal); on Ultimate the Labyrinth entry is the Tower of Mirages. It does not need Unlock Labyrinth of Chaos, and that option does not need it. A character left on Ultimate stays on it when the option is turned Off, so change back to Normal at the Temple first. Both unlocks apply at the next game start and mark nothing as cleared
- **Allow overlay programs** (Windows, Off by default): starts the game as a separate process from the launcher, which recording, chat, store and performance overlays can attach to cleanly. Turn it on if the game closes right after Play while such a program is running

The game runs at its own 60 frames per second unless the Frame rate setting asks for more; its logic stays at 60 either way.

### Online
- **Online play** (Off by default), then three tabs: **Settings** (**Online name**, **Your friend code**, **Connection test**, **Ping display**), **Friends** with friend requests, and **Lobby** (**Show me in Lobby**, Off by default). See [Online Co-op](#online-co-op) for how to play and [docs/ONLINE.md](docs/ONLINE.md) for the details.

### Cheats
- **Infinite Health**, **Infinite Ally Health**, **Infinite Mana**, **Infinite Ally Mana**, **Infinite Lives**, **Infinite Durability**, **Infinite Ally Durability**, **Infinite Arrows**, **Infinite Skill Points**, **Infinite Skill Uses**, **Infinite Items**, **Infinite Ally Skill Uses**, **Infinite Ally Items** and **Infinite Gold** (every local human player, except the ally cheats, which act on AI allies; gold is shared by the save), all Off by default. **Movement Speed** (Off, 2x or 4x), **Experience Multiplier** (Off, 2x, 5x, 10x or 100x), **Damage Taken** (100%, 75%, 50%, 25% or 0%, every local human player) and **Damage Multiplier** (Off, 2x, 5x, 10x or 100x; damage to enemies) are settings of their own. Press **F10** while playing to open the Cheats menu; it also has the one-shot **Fill Health**, **Fill Ally Health**, **Set Low Health**, **Set Ally Low Health**, **Add 100 Skill Points**, **Add 1,000 Gold** and **Level Up (+1 Level)**. What you change there applies only to the current session, and the next start uses the values saved here. They are meant for offline and local play: while Online Play is on (the setting or `DCP_ONLINE`), every cheat is Off and F10 only shows "Cheats are disabled while Online Play is enabled" See [docs/CHEATS.md](docs/CHEATS.md).

### Controls
- Xbox controllers work without setup. Another device (a remote-play virtual pad, for example) needs a line in `user/gamecontrollerdb.txt`, in the format of [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB); the log names its GUID
- **Controller prompts**: PlayStation (original) or Xbox. Xbox changes the button pictures the game shows (A/B/X/Y, LB/RB, LT/RT, MENU and VIEW) in the port's own copy of the game; the touchpad prompt becomes VIEW as well, because View performs that action. PlayStation puts the originals back. Your `.pkg` file is never changed.
- The **keyboard** is always on, together with the controller. The keys below are the defaults; change any of them on the launcher's **Controls** page (click a key, press the new one, Esc cancels) and use **Reset to defaults** to restore them:

| Action | Keys |
| --- | --- |
| Left stick | W A S D |
| Right stick | Arrow keys |
| Cross (A) / Circle (B) | Space / Left Shift |
| Square (X) / Triangle (Y) | E / Q |
| L1 / R1 | 1 / 3 |
| L2 / R2 | R / F |
| L3 / R3 | Z / C |
| D-pad up / down / left / right | I / K / J / L |
| Options (Start) | Enter |
| Touchpad left / right (Back) | Tab / Backspace |

- The **mouse** is Player 1's too, together with the keyboard and the controller. The defaults below can be changed under **Mouse** on the **Controls** page (each mouse button has a row; choose the control it presses):

| Action | Mouse |
| --- | --- |
| Square (primary action) | Left Mouse |
| Circle (secondary action) | Right Mouse |
| Pointer Control (Hold mode) | Middle Mouse |
| Pointer Interaction (click) | Left Mouse, while the pointer is active |
| Mouse 4, Mouse 5 | unbound |

  Any of the five mouse buttons can press one controller control: Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, the four D-pad directions, Options, or Touchpad left or right. Choosing a control a button already has elsewhere moves it. **Pointer Control Mode** is **Hold** by default, which keeps the behaviour described below. **Always On** moves the game's pointer with the mouse whenever the mouse is free for gameplay, with no button held; it is meant for a trackpad such as the Steam Deck's. On a Steam Deck in Gaming Mode the trackpad drives the pointer this way by itself, and its click is the Pointer Interaction; set **Steam Deck Trackpad** to Off on the launcher's Controls page to keep ordinary mouse behavior, for example with a plugged-in mouse. In Always On the Pointer Interaction button always performs the pointer interaction (so put Square on another button), the Pointer Control button is not used but its setting is kept for Hold, and Alt+Tab, the Options or Cheats menu, a text dialog or a window mode switch pause it until the game has the mouse again. Touching a controller's touchpad takes the pointer for as long as the finger is down.

  In Hold mode, moving the mouse alone does not move the player. Hold **Pointer Control** and move the mouse to move the game's native red hand pointer; the cursor is hidden and kept inside the window meanwhile. While it is held, **Pointer Interaction** performs the native pointer interaction instead of attacking, and a button used that way stays out of the normal actions until it is released. Releasing Pointer Control returns to normal mouse attacks at once. Alt+Tab, the Options or Cheats menu, a text dialog, switching between Windowed and Borderless Fullscreen, or touching a controller's own touchpad also end it; controller buttons and sticks do not, so the pointer keeps moving while they are held. **Mouse Sensitivity** is a slider from 25% to 200% in 5% steps (100% by default) with a **Default** button, also a live row in F1; it sets how fast mouse movement moves the pointer. **Mouse Controls** (On by default, also in the F1 Options menu) turns all of this off for gameplay without affecting the launcher and menus. Each wheel direction can press a controller button (off by default). The mouse works the same in Online Play, because the game only sees its own controller input.

- **Japanese Menu Buttons** (Controls page, Off by default): Off, Cross (A) confirms and Circle (B) cancels in the menus on every game build, including the Japanese and Asia/Hong Kong builds; On swaps them, as in the Japanese release. Combat controls do not change. Applies at the next game start
- **F1** (or **L3 + R3** on a controller) opens the Options menu: most of the launcher's Display, Graphics and Gameplay settings, a Controls page that lists the hotkeys, and the Cheats page of the F10 menu. Changes are saved to the same `dcp.ini` as the launcher. The Renderer, Render resolution, Aspect Ratio, True Ultrawide, Texture filtering, Frame rate and a few Gameplay options are saved and apply the next time the game starts (close it and start it again); the rest apply at once. See [docs/LAUNCHER.md](docs/LAUNCHER.md). **F11** switches Windowed and Borderless Fullscreen. **F10** opens the Cheats menu (both can be changed under **Port hotkeys** on the Controls page). **F2** shows or hides the frame counter. **F3** shows or hides the Ping while Online Play is on (see [docs/ONLINE.md](docs/ONLINE.md)). An experimental AMD water workaround for black regions under characters standing in water is On on AMD graphics cards and Off elsewhere. Window size, monitor, graphics card, language and the online settings are set in the launcher.
- **Local multiplayer**: up to four players, one controller each (Player 1 is the first controller found, and the keyboard is always Player 1's). A controller connected later takes the lowest free player slot; a player stays logged in when their controller is unplugged.

### Editor
- Finds the installed game's saves by itself. Choose the **Save Data** (DATA slot), then a character from the list.
- **Character Editor**: the character's name, level, skill points and HP.
- **Edit Appearance**: click a part on the preview (or choose it from the list), then set its **Part Color** or make it **Hidden** where the part supports it. **Random** and **Reset Part** act on one part, **All Colors Random** and **Reset Custom** on the whole character. With no custom change a character is drawn exactly as the game draws it. A Fighter whose helmet is hidden shows the game's own bare head.
- Nothing is written until **Apply Changes**, which backs up the save first; **Discard Changes** restores what was there. Custom part colors and hidden parts are kept in `user\character_appearance.dat`; the save itself receives only the name, level, skill points, HP and the game's own base color.
- Online, other players see your custom look when their port version supports it; older versions draw your character with its normal colors.
- The in-game looks need the game's character drawing code, found on the USA, Japanese and Asia / Hong Kong 1.00 builds. The Asia / Hong Kong 1.02 and European 1.03 executables could not be checked yet; if that code is not found there, characters are drawn with their normal look.

### Trophies
- The game's PlayStation 4 trophies are tracked locally and offline, on this PC only: no PSN connection, no account, no sync. The list and icons come from your own game package, and the **Trophies** launcher page shows your progress, the trophies and their grades.
- The trophy list is encrypted in the package. The port does not include, generate or download the Trophy Key it needs: enter your own on the Trophies page, or import it from a shadPS4 `keys.json` or `config.toml`. It is stored only in `user\keys.json`, and the launcher never shows it again. Without it the game still records your unlocks, and the page shows them once a key works.
- There is no in-game "Trophy unlocked" notification yet. See [docs/TROPHIES.md](docs/TROPHIES.md).

### Bug Report
- Describe the problem, choose a game log and press **Send report**. **Open preview** shows exactly what goes out, after the privacy filter. **My reports** shows the status of what you sent. After a Windows crash the log also offers **Attach crash report**. See [Reporting problems](#reporting-problems)

### About
- The port version, game version, renderer and library versions
- Open the game folder, the saves and settings folder, or the logs folder
- **Show settings file**, and **Reset all settings**

## Screenshots

<img width="1962" height="1170" alt="2026-10-14_23-45" src="https://github.com/user-attachments/assets/91c30616-e700-478f-a56b-c286344e6502" />
<img width="1935" height="1161" alt="2026-10-14_23-44_1" src="https://github.com/user-attachments/assets/3f9fbc08-a505-4caa-8346-d5560b82d94d" />


## Reporting problems

The easiest way is the launcher's **Bug Report** page: describe the problem, choose the log of that session and press **Send report**. It filters the log first, and **Open preview** shows exactly what goes out. You get a report ID, and **My reports** shows its status.

You can also open an [issue](https://github.com/saulob/dragons-crown-pro-pc/issues). Attach the newest `dragons_crown_pro_*.log` file from the `logs` folder next to the program, and say what you were doing when the problem happened.

On Windows, a game crash also writes a `.dmp` crash file next to the log (`dragons_crown_pro_NNN.dmp`, up to 8 MiB can be sent). The Bug Report page marks that log **Crash detected** and offers **Attach crash report**, on by default. A crash file may contain process memory used for diagnostics and is not filtered like the log, so untick the box if you would rather not send it. Linux keeps the text log only.

If the launcher says your Dragon's Crown Pro version is not supported yet, it has already written technical diagnostic information about that version to the latest log. Attach that log when you open the issue.

For a Linux or Steam Deck problem, please also give your Linux distribution, your GPU and its driver (for example Mesa and RADV, or NVIDIA), and on a Steam Deck whether you were in Desktop Mode or Gaming Mode.

## Building it yourself

```powershell
git clone https://github.com/saulob/dragons-crown-pro-pc.git
```

1. Install [MSYS2](https://www.msys2.org) with the CLANG64 toolchain. `.\tools\setup.ps1` checks it and prints the `pacman` command for anything missing.
2. `.\tools\build.ps1` fetches the pinned dependencies (TRG Launcher, Dear ImGui and a few small libraries) and builds `build\cmake\clang64\bin\dragons_crown_pro.exe`.
3. Start that executable yourself: the build only builds, and puts every DLL the game loads beside it. Install the game from the launcher's Play page, as in a release; the development build keeps it in `game\` and the boot image in `build\image\` of the repository.
4. `.\tools\package.ps1` makes the release folder, ZIP and `.zip.sha256` file in `dist\` from the same build: the program without debug information (the symbols stay in `dist\`), the DLLs it needs and the licences. It is local only and never uploads, tags or publishes anything. The release version is the project `VERSION` in `CMakeLists.txt`.
5. `.\tools\package-release.ps1` prepares the whole release in `dist\`: that Windows ZIP, and the Linux AppImage built in WSL2 (`tools/package-release-linux.sh`, which runs `tools/package-linux.sh`), each with its `.sha256` file. It needs the Ubuntu 20.04 WSL2 distribution `DcpFocalBuild`, prepared once as [docs/LINUX.md](docs/LINUX.md#building) describes, installs nothing, fails if either platform fails, and never uploads anything.

`.\tools\test.ps1` runs the focused tests. It is also the supported-build parity gate: `dcp-build-parity-tests` holds one contract row per supported build (capabilities, mappings, base and update parity with its listed exceptions), reads any local `build\image` and `game\prepared` folders without changing them, and the script ends with a per-build `PASS real` / `PASS synthetic` summary and a non-zero exit code when anything fails. Adding a supported build means adding its contract row. Python is not needed to build, test or play. Only the optional development scripts in `scripts\` use it, such as the Python cross-checks of the package extractor and the boot image builder (`.\tools\setup.ps1 -InstallPython`).

### Linux

```sh
git clone https://github.com/saulob/dragons-crown-pro-pc.git
cd dragons-crown-pro-pc
sudo bash packaging/linux/ci-deps.sh   # Ubuntu 20.04: toolchain and pinned libraries (docs/LINUX.md)
bash tools/build.sh               # fetches the pinned dependencies, builds build/cmake/linux/bin/dragons_crown_pro
bash tools/build.sh --test        # also builds and runs the focused tests
bash tools/package-linux.sh       # dist/DragonsCrownProPC-v<version>-x86_64.AppImage and its .sha256 (--out-dir DIR: elsewhere)
```

`tools/build.sh --run` starts the loader that was built. Python is not needed. The AppImage is built on Ubuntu 20.04, whose glibc (2.31) is the oldest one it runs on, and `tools/package-linux.sh` checks every ELF file in it against that; `.github/workflows/linux-appimage.yml` does the same in GitHub Actions (started by hand, publishes nothing).

The technical notes are in [docs](docs): [launcher and settings](docs/LAUNCHER.md), [online play](docs/ONLINE.md), [Linux port](docs/LINUX.md), [graphics](docs/GRAPHICS.md), [OpenGL renderer: origin and independence](docs/opengl-renderer.md), [Direct3D 11 renderer](docs/d3d11-renderer.md), [first boot](docs/FIRST_BOOT.md), [bootstrap](docs/BOOTSTRAP.md) and [references](docs/REFERENCES.md).

## Sponsoring

Dragon's Crown Pro PC is maintained and developed in my free time. If you enjoy the project or find it useful, consider supporting its continued development.

<p align="center">
<a href="https://github.com/sponsors/saulob" target="_blank"><img src="https://img.shields.io/badge/GitHub%20Sponsors-Sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors"></a>
</p>

## Credits

- Dragon's Crown Pro by Vanillaware, published by Atlus (2018). This is an unofficial fan-made PC port. It is not affiliated with or endorsed by Vanillaware, Atlus, SEGA, Sony Interactive Entertainment or any other rights holder, and it contains no game package or executable.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) for the Vulkan renderer, shader recompiler and GPU command processing the video core is built on, and for the PS4 package extractor the installer is adapted from, as it was before upstream commit `be22674f` removed it (GPL-2.0-or-later)
- [gow3_pc](https://github.com/FelipePrado0/gow3_pc) and its lineage, uncharted-ndc-native, MediEvil PC and the HayatoG ports, for the loader, runtime and video core this port grew from (see [docs/REFERENCES.md](docs/REFERENCES.md))
- [OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) by Nezara for published Dragon's Crown Pro reverse-engineering research used as a technical reference, including parts of the online/lobby research and the original CUSA10454 1.00 story/tutorial skip patch-site research. Dragon's Crown Pro PC's implementations were written separately for this project
- The experimental OpenGL renderer is an independent backend on the shadPS4-derived video core; the projects it was informed by, and what was and was not taken from each, are listed in [docs/opengl-renderer.md](docs/opengl-renderer.md)
- [ANGLE](https://chromium.googlesource.com/angle/angle) runs the experimental Direct3D 11 renderer on Windows (BSD-3-Clause); see [docs/d3d11-renderer.md](docs/d3d11-renderer.md)
- Brazilian Portuguese translation, images and manual based on the work of [Marcelo - Uma Ficha](https://www.youtube.com/@umaficha1), used with permission
- Russian localization based on the fan translation by [Exclusive Studio](https://ex-s.ru/dc.html), with PS4 porting work by AlchemistLab ([Zone of Games](https://www.zoneofgames.ru/games/dragons_crown/files/13583.html)). It is a fan translation, not an official one; the full credits are in [localization/ru/README.md](localization/ru/README.md)
- Discord Rich Presence started from the contribution in [PR #55](https://github.com/saulob/dragons-crown-pro-pc/pull/55) by [Shmubbier](https://github.com/Shmubbier), with commits by na-om
- [bloodborne_pc_windows](https://github.com/Phroster/bloodborne_pc_windows) for the anisotropic filtering approach
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al., and AMD FidelityFX Super Resolution 1
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, licensed under the MIT License
- The launcher and installer follow the approach of the [Hard Corps: Uprising](https://github.com/saulob/hard-corps-uprising-recompiled) and [Geometry Wars: Retro Evolved 2](https://github.com/saulob/geometry-wars-retro-evolved-2-recompiled) PC ports
- The licenses of the libraries the release includes are in the `licenses` folder of the ZIP

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed, tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 or later (GPL-2.0-or-later). See [LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See [licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).

The game package and executable are not included. Dragon's Crown Pro and its assets are not covered by this project's license. The localization resources shipped for the port's additional languages are credited under [Credits](#credits).
