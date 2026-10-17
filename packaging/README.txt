Dragon's Crown Pro - PC port
Version @VERSION@

Dragon's Crown Pro for PlayStation 4, running natively on Windows: the game's
own code runs on the PC with a replacement for the PS4 system libraries and a
Vulkan renderer.

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
- Windows 10 version 1903 or later, or Windows 11, 64-bit
- A graphics card and driver with Vulkan 1.3, VK_EXT_robustness2,
  VK_KHR_push_descriptor and VK_EXT_vertex_attribute_divisor
- Your own Dragon's Crown Pro package, plus about 6 GB of free disk space next
  to the program for the installed game
- Nothing else: no Python or other tools are needed


INSTALL
-------
1. Extract the ZIP anywhere you can write to (not "Program Files").
2. Run dcp-loader.exe. The launcher opens.
3. On the Play page, click "Install game...".
4. Select your Dragon's Crown Pro .pkg file.
5. Wait while the launcher checks the package and unpacks it into the "game"
   folder next to the program. The .pkg file is only read and can be deleted
   afterwards.
6. When the Play page says "Ready to play", press PLAY.

To repair the game files, use "Reinstall game..." on the Play page. Your
settings and saves are kept. If an installation fails or is cancelled, the
previous one is left as it was.


WINDOWS SMARTSCREEN
-------------------
The first time you run dcp-loader.exe, Windows SmartScreen may show a warning,
because the program is not digitally signed. If you downloaded the release
from the project's GitHub releases page, choose "More info", then
"Run anyway".


PLAYING
-------
- F11 switches between Windowed and Borderless Fullscreen.
- Insert (or R3 + L2 on a controller) opens the in-game settings menu.
- Xbox controllers work without setup.
- The keyboard works too; the launcher's Controls page lists the keys.
- The Controls page also chooses the button prompts the game shows: PlayStation
  (original) or Xbox. It can be switched back at any time.
- Local multiplayer: up to four players, one controller each; the keyboard is always
  Player 1's. A controller connected later takes the lowest free player slot, and a
  player stays logged in when their controller is unplugged. Online play is not
  supported.
- F10 opens the Cheats menu (meant for offline and local play); the launcher has a
  Cheats page for the values saved for the next start.
- To open the launcher again after turning it off, hold Shift while starting.


WHERE THINGS ARE
----------------
- Game files:     the "game" folder next to dcp-loader.exe
- Logs:           the "logs" folder next to dcp-loader.exe
- Settings (dcp.ini), saves, trophy progress, the Trophy Key (keep it
                  private) and shader cache:
                  the "user" folder next to dcp-loader.exe

These folders are created when they are first needed and are not part of the
download. When updating the port, keep "user" (and "game", to skip
reinstalling).

If the game does not start or crashes, please attach the newest
dragons_crown_pro_*.log file from the "logs" folder to a report at
https://github.com/saulob/dragons-crown-pro-pc/issues


LICENSE
-------
The original project code of this port is licensed under the GNU General
Public License v2.0 or later (GPL-2.0-or-later). The full text is in
LICENSE.txt.

Third-party components keep their own licenses. Their license files and
notices are in the "licenses" folder, starting with THIRD-PARTY-LICENSES.txt.

Game files are not included and are not covered by this project's license.
This port is not affiliated with or endorsed by Vanillaware, Atlus, SEGA,
Sony Interactive Entertainment or any other rights holder of Dragon's Crown
Pro.
