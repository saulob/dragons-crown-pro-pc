Dragon's Crown Pro - PC port (Linux)
Version @VERSION@

Dragon's Crown Pro for PlayStation 4, running natively on Linux x86-64: the
game's own code runs on the PC with a replacement for the PS4 system libraries
and a Vulkan renderer. This is a native build, not Wine or Proton. The same
AppImage runs on desktop Linux and on the Steam Deck.

Source code and issue tracker:
https://github.com/saulob/dragons-crown-pro-pc


NO GAME FILES ARE INCLUDED
--------------------------
You need your own legally obtained copy of the game: a decrypted Dragon's
Crown Pro PS4 package (.pkg). This port runs these releases:

  US:                 Title ID CUSA10454, version 1.00 (APP_VER 01.00, VERSION 01.03)
  Japan:              Title ID CUSA07001, versions 1.00 (the base package), 1.02 and 1.03 (the base package, then the update)
  Asia, Hong Kong:    Title ID CUSA10209, versions 1.00 (APP_VER 01.00) and 1.02 (APP_VER 01.02)

Any other version is refused with "Different version of Dragon's Crown Pro". The Japanese and Asia, Hong Kong releases
are not yet fully tested; their cheats and Xbox controller prompts are not available.
A package still encrypted for a PS4 console cannot be unpacked on a PC.


REQUIREMENTS
------------
- 64-bit Linux (x86-64) with glibc 2.31 or later and a Vulkan loader for
  Vulkan 1.3 (libvulkan.so.1; Ubuntu 20.04's own is too old). So far the
  game has only been run on Ubuntu 24.04; other distributions that are new
  enough may work
- A graphics card and Mesa or proprietary driver with Vulkan 1.3,
  VK_EXT_robustness2, VK_KHR_push_descriptor and
  VK_EXT_vertex_attribute_divisor (AMD RADV on the Steam Deck qualifies)
- FUSE 2 to start an AppImage (libfuse2; SteamOS has it). Without it:
  ./DragonsCrownProPC-*.AppImage --appimage-extract-and-run
- Your own Dragon's Crown Pro package, plus about 6 GB of free disk space next
  to the AppImage for the installed game
- Nothing else: no Python or other tools are needed; the AppImage carries its
  libraries (SDL3, FreeType, libpng, OpenSSL and others). GPU drivers and the
  Vulkan loader come from your system


INSTALL
-------
1. Put the AppImage in a folder you can write to (it keeps its game files,
   saves and logs beside itself) and make it executable:

     chmod +x DragonsCrownProPC-v@VERSION@-x86_64.AppImage

2. Run it (double-click, or from a terminal). The launcher opens.
3. On the Play page, click "Install game..." and select your .pkg file. The
   launcher uses the desktop's file chooser (zenity or kdialog) when there is
   one, and its own file chooser otherwise (mouse, keyboard or controller).
4. Wait while the launcher checks the package and unpacks it into the "game"
   folder next to the AppImage. The .pkg file is only read and can be deleted
   afterwards.
5. When the Play page says "Ready to play", press PLAY.

To repair the game files, use "Reinstall game..." on the Play page. Your
settings and saves are kept. If an installation fails or is cancelled, the
previous one is left as it was.


STEAM DECK
----------
Desktop Mode: copy the AppImage to a folder such as ~/Games/DragonsCrownPro,
make it executable (right-click > Properties > Permissions) and run it once to
install the game and set the options with the launcher.

Gaming Mode: in Desktop Mode open Steam, "Add a Game" > "Add a Non-Steam
Game...", then Browse and choose the AppImage (select "All Files" in the
chooser). The launcher opens first; its "Show this launcher" switch (Play
page) turns it off once everything is set.

This Linux build is EXPERIMENTAL: it is meant to offer everything the Windows
version does, but it has only been tested automatically under WSL2 (Ubuntu
24.04, X11 and Wayland, software Vulkan), not on a real Linux PC or Steam Deck.
If something does not work, please attach the newest log from the "logs" folder
to an issue, with your distribution, GPU and driver.


PLAYING
-------
- F11 switches between Windowed and Borderless Fullscreen.
- Insert (or R3 + L2 on a controller) opens the in-game settings menu.
- Controllers are read through SDL's gamepad support: Xbox controllers and the
  Steam Deck's own controls.
- The keyboard works too; the launcher's Controls page lists the keys.
- The Controls page also chooses the button prompts the game shows: PlayStation
  (original) or Xbox. It can be switched back at any time.
- Local multiplayer: up to four players, one controller each; the keyboard is always
  Player 1's. A controller connected later takes the lowest free player slot, and a
  player stays logged in when their controller is unplugged. Online play is not
  supported.
- F10 opens the Cheats menu (meant for offline and local play); the launcher has a
  Cheats page for the values saved for the next start.
- To open the launcher again after turning it off, hold Shift while starting
  (X11 sessions), start the AppImage with the option --launcher, or use the
  "Show launcher" action of its desktop entry. On the Steam Deck: Steam > the
  game's Properties > Launch Options: --launcher; remove it again afterwards.


WHERE THINGS ARE
----------------
- Game files:     the "game" folder next to the AppImage file
- Logs:           the "logs" folder next to the AppImage file
- Settings (dcp.ini), saves, trophy progress, the Trophy Key (keep it
                  private) and shader cache:
                  the "user" folder next to the AppImage file

These folders are created when they are first needed and are not part of the
download. If the AppImage's folder is read-only, they are kept in
$XDG_DATA_HOME/DragonsCrownProPC (~/.local/share/DragonsCrownProPC) instead.
Setting DCP_PORTABLE_DIR to a folder chooses the place explicitly.

When updating the port, keep "user" (and "game", to skip reinstalling).

If the game does not start or crashes, please attach the newest
dragons_crown_pro_*.log file from the "logs" folder to a report at
https://github.com/saulob/dragons-crown-pro-pc/issues


LICENSE
-------
The original project code of this port is licensed under the GNU General
Public License v2.0 or later (GPL-2.0-or-later). The full text is in
usr/share/licenses/DragonsCrownProPC/LICENSE.txt inside the AppImage
(run it with --appimage-extract to see the files).

Third-party components keep their own licenses. Their license files and
notices are in the same folder, starting with THIRD-PARTY-LICENSES.txt.

Game files are not included and are not covered by this project's license.
This port is not affiliated with or endorsed by Vanillaware, Atlus, SEGA,
Sony Interactive Entertainment or any other rights holder of Dragon's Crown
Pro.
