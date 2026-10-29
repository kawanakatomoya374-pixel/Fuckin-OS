# PSXFunkin for C-OS

A port of [PSXFunkin](https://github.com/IgorSou3000/psxfunkin) (Friday Night Funkin' for the PlayStation, C, game code under MPL 2.0) to C-OS.

The game code is used **unchanged** except for one line pair in `boot/mem.h` (`tools/patch_upstream.sh`). Everything the game takes from the
PlayStation is a small set of functions (graphics, audio, input, time, files, memory; the PC defines of the game already abstract the types), and this directory
re-implements them:

| file | what |
|---|---|
| `backend/gfx.c` | software renderer for the GPU calls (rectangles, sprites, textured quads, the four semi-transparency modes); replays the ordering table backwards |
| `backend/audio.c` | music: the `.mus` description lists the files of the channels; mp3/ogg are decoded (minimp3, stb_vorbis); `Audio_GetTime()` is the position of the sound being heard |
| `backend/io.c` | `"\\CHAR\\X.ARC;1"` style names -> files (`FILES.TXT`), and the overlay data stream (`OVERLAYS.TXT`) |
| `backend/pad.c`, `timer.c`, `psx.c`, `mem.c`, `movie.c` | pad = keyboard, real elapsed time per frame, ... |
| `plat.h` | what the backend needs from the system |
| `host/plat_host.c` | headless PC platform (frames as PPM files, simulated 60 Hz clock, scripted keys): `host/build_host.sh`, then `PSXF_KEYS=... PSXF_FRAMES=... /tmp/psxf_host` |
| `cos/plat_cos.c` | the C-OS platform (window, keyboard, audio device, `cos_time_us()`) |
| `tools/prep_assets.py` | builds the data directory from a built PSXFunkin tree |

The game's own tools make the textures (`.tim`), charts and archives from the sources of the PSXFunkin repository
(`make -f Makefile.tools; make -f Makefile.tim; make -f Makefile.cht; make -f Makefile.toh`).

## Licences
Code of the game: MPL 2.0 (file level; the patched `mem.h` stays MPL). The backend in this directory is part of C-OS. **The game's assets (graphics, music, charts) are not ours**:
they belong to the Friday Night Funkin' team and the authors of the songs; PSXFunkin states that some of them are used with permission. They are not part of the C-OS image
and are not distributed by C-OS; the data directory is made on the user's side from the PSXFunkin repository.

## Data on the user's device (`setup/`)
The app pack carries only the game and `psxsetup.c-os` with `recipe.txt` (made by `tools/make_recipe.py`: every file the game needs, its place in the PSXFunkin repository at a
pinned commit, its SHA-256). On the first start the game finds no `/apps/psxfunkin/data/FILES.TXT` and starts the setup, which downloads the files from raw.githubusercontent.com, refuses
any whose hash differs, converts the PNG textures to TIM (the same conversion as the repository's `funkintimpak`; the result is byte-identical) and writes the data directory.
Finished files are skipped on a second run. `setup/base_url.txt` can point to a test server. `make psx-pack` builds the pack.
