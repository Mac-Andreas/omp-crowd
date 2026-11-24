**omp-crowd @VERSION@**: the client DLL behind the open.mp launcher's **Local crowd**. GTA San Andreas's own pedestrians and traffic come back on SA-MP / open.mp servers, GTA Online style: your game makes the crowd.

**The server decides.** The DLL does nothing unless the server runs the Crowd component and a script turns it on (`crowd: yes` in the server info). The server sets the density and the limits.

### Downloads

| File | What it is |
|---|---|
| `omp-crowd-@VERSION@.zip` | Everything for a manual install: `omp-crowd.dll`, a sample `omp-crowd.ini`, `README.txt`, `LICENSE` |
| `omp-crowd.dll` | The DLL on its own (what the launcher ships) |
| `omp-crowd.ini` | The sample settings on their own |
| `SHA256SUMS.txt` | Checksums for the three files above |

### Platforms

There is one build. It's a 32-bit Windows DLL that runs inside `gta_sa.exe` (GTA SA 1.0 US) and needs only Windows' own libraries (kernel32, msvcrt, ws2_32), so the same file works everywhere GTA SA runs:

- **Windows**: in the GTA San Andreas folder.
- **macOS** (CrossOver, Whisky, Wine, the macOS open.mp launcher): in the GTA folder inside the bottle (`drive_c/...`).
- **Linux and Steam Deck** (Wine, Proton, Lutris, Bottles): in the GTA folder inside the prefix.
- Android isn't supported.

### Install

- **The launcher** (Settings → Beta → Local crowd) installs and loads it for you.
- **By hand**: put `omp-crowd.dll` and `omp-crowd.ini` next to `gta_sa.exe`. Load the DLL with an ASI loader (rename it `omp-crowd.asi`) or any injector. It waits until you're in game before it changes anything. `omp-crowd.log` beside the game lists what it did. Set `enabled=0` to switch it off.

**Servers**: the Crowd component (macOS arm64, Linux x86 for the official server, Linux arm64) and `omp-crowd.inc` are in [omp-server-macos](https://github.com/Mac-Andreas/omp-server-macos/releases/tag/v1.6.7.3218-macos-arm64).

Built by CI from this tag (`scripts/compile.sh`, MinGW on Debian bookworm, no PE timestamp, so a rebuild gives the same bytes). Game addresses come from gta-reversed, the community reverse of GTA SA; none of its code is copied. MIT licence.
