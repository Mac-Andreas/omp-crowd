# omp-crowd

The open.mp launcher's **Local crowd** (Settings -> Beta): a Win32 DLL that brings GTA San
Andreas's own pedestrians and traffic back while you play on SA-MP / open.mp servers, the way
GTA Online does it: your game makes the crowd.

**The server decides.** The DLL does nothing unless the server runs the Crowd component and a
script turns it on (`crowd: yes` in the server info); the server sets the density and the
limits. When players are together, the server picks one owner per group and the others see
copies (beta, still being tested with several players).

At start it compares the game's code in memory with `gta_sa.exe` on disk, logs every stretch
SA-MP changed to `omp-crowd.log`, and puts back only what the crowd needs
(`CPopulation::AddPed`, the generate-peds rule in `CGame::Process`, drivers in cars). Random
cars stay locked, and nothing near you vanishes when you look away.

## Install

One file runs inside `gta_sa.exe` (GTA SA 1.0 US), so the same DLL works on Windows, macOS
(CrossOver, Whisky, Wine) and Linux / Steam Deck (Wine, Proton). It only needs Windows' own
libraries (kernel32, msvcrt, ws2_32). Android isn't supported.

- **The launcher** installs and loads it when Local crowd is on, and writes `omp-crowd.ini`.
- **By hand**: from the [release](https://github.com/Mac-Andreas/omp-crowd/releases), put
  `omp-crowd.dll` and `omp-crowd.ini` next to `gta_sa.exe` (in a Wine bottle or prefix:
  `drive_c/.../GTA San Andreas`). Load it with an ASI loader (rename it `omp-crowd.asi`) or
  any injector. It waits until you're in game before it changes anything.

Without `omp-crowd.ini` it does nothing. `[world]`: `enabled`, `peds`, `cars`,
`keep_peds_m` / `keep_cars_m` (nothing that close vanishes), `force_generate` (needed under
Wine), `relay` (the link to the server), `diag` (the log). `omp-crowd-dev.ini` /
`omp-crowd-dev.dll` next to the game are for testing a build without a new launcher.

## Servers

The Crowd component (macOS arm64, Linux x86 for the official server, Linux arm64) and
`omp-crowd.inc` for Pawn are in
[omp-server-macos](https://github.com/Mac-Andreas/omp-server-macos/releases/tag/v1.6.7.3218-macos-arm64).

## Build

`./build.sh` (MinGW in a Debian container) -> `build/omp-crowd.dll`.

GTA SA 1.0 US addresses come from gta-reversed (github.com/gta-reversed/gta-reversed), the
community's reverse of the game; nothing of its code is copied. MIT licence.
