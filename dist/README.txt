omp-crowd @VERSION@
===============
GTA San Andreas's own pedestrians and traffic back on SA-MP / open.mp servers, the way
GTA Online does it: your game makes the crowd. It is one Win32 DLL that runs inside
gta_sa.exe, so the same file works on every platform GTA SA runs on.

Needs: GTA San Andreas 1.0 US (gta_sa.exe), SA-MP 0.3.7 / open.mp client, and a server
running the Crowd component with the crowd turned on (it shows "crowd: yes"). Anywhere
else it does nothing.

The easy way
------------
The open.mp launcher (Settings > Beta > Local crowd) installs and loads it for you.

By hand
-------
1. Copy omp-crowd.dll and omp-crowd.ini into the GTA San Andreas folder (next to gta_sa.exe).
2. Get the game to load it, either way:
   - with an ASI loader (Silent's ASI Loader, or the one CLEO / SA-MP addon packs ship):
     rename omp-crowd.dll to omp-crowd.asi;
   - or with any DLL injector, after samp.dll.
   It waits until you are in game before it changes anything, so loading early is fine.
3. Join a crowd server. omp-crowd.log next to gta_sa.exe says what it did.

Where the GTA folder is
-----------------------
Windows:              wherever GTA San Andreas is installed.
macOS (CrossOver, Whisky, Wine, the open.mp macOS launcher):
                      inside the bottle, drive_c/Program Files/Rockstar Games/GTA San Andreas
                      (or wherever it was installed in the bottle).
Linux, Steam Deck (Wine, Proton, Lutris, Bottles):
                      inside the prefix, drive_c/... the same way.
Android: not supported (the Android clients are not gta_sa.exe).

To switch it off: enabled=0 in omp-crowd.ini, or delete the two files.

Servers: the Crowd component (macOS arm64, Linux x86 for the official server, Linux arm64)
and omp-crowd.inc for Pawn are in the omp-server-macos release:
https://github.com/Mac-Andreas/omp-server-macos/releases/tag/v1.6.7.3218-macos-arm64

MIT licence (LICENSE). Game addresses from gta-reversed, the community reverse of GTA SA;
none of its code is copied.
