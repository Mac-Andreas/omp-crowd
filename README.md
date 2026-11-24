# omp-crowd

The open.mp launcher's **Local crowd** (Settings -> Beta): a Win32 DLL that brings GTA San
Andreas's own pedestrians back while you play on SA-MP / open.mp servers - locally, as GTA
Online does it: your game makes its own crowd, which only you see.

The launcher injects it with samp.dll and omp-client.dll when the switch is on and writes
`omp-crowd.ini` next to `gta_sa.exe` (without that file the DLL does nothing). At start it
compares the game's code in memory with `gta_sa.exe` on disk, logs every stretch SA-MP changed
to `omp-crowd.log`, and puts back only what the crowd needs (`CPopulation::AddPed`, the
generate-peds rule in `CGame::Process`). Peds only, no ambient cars yet.

Later: a ped you interact with handed to the server, so everyone sees it.

`omp-crowd.ini` (`[world]`): `enabled`, `diag` (log the patches), `peds`, `ped_density` (0-1),
`max_peds` (at most 40), `force_generate`. `omp-crowd-dev.ini` / `omp-crowd-dev.dll` next to
the game: for testing a build without a new launcher.

GTA SA 1.0 US addresses come from gta-reversed (github.com/gta-reversed/gta-reversed), the
community's reverse of the game; nothing of its code is copied.

Build: `./build.sh` (MinGW in a Debian container) -> `build/omp-crowd.dll`.
