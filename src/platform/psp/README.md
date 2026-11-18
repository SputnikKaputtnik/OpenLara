# OpenLara for PSP: Tomb Raider 1 PC data

This port runs the PC version of Tomb Raider 1 (for example the Steam or GOG release) on a PSP.
Tested on a PSP-1000 (custom firmware 5.00 M33-6), started from the XMB.

No game data is included. You need your own copy of Tomb Raider 1.

## Installation

1. Copy `EBOOT.PBP` to `ms0:/PSP/GAME/OpenLara/` on the memory stick.
2. Copy the level files of the game into `ms0:/PSP/GAME/OpenLara/DATA/`:
   all `*.PHD` files (`TITLE.PHD`, `GYM.PHD`, `LEVEL1.PHD` ... `LEVEL10C.PHD`, `CUT1-4.PHD`) and the `*.PCX` pictures.
3. Optional, music (see below): put the `track_XX.ogg` files into `ms0:/PSP/GAME/OpenLara/`.

The Steam/GOG release keeps the level files inside the CD image `GAME.GOG` (MODE1/2352 raw track).
Convert it to an ISO with `tools/bin2iso.py GAME.GOG game.iso` and extract the `DATA` folder with any ISO tool (for example 7-Zip).

Controls: D-pad / analog stick move, CROSS action, SQUARE jump, CIRCLE roll, TRIANGLE draw weapon,
L look, R walk, SELECT inventory, START pause. They can be changed in the controls menu.
Hold L while the game starts to choose a level directly.

## Music

The PSP build plays OGG files only, and they must be **44100 Hz stereo** (the decoder does not resample).
Files are named after the game's track numbers: `track_02.ogg` ... `track_60.ogg`, next to `EBOOT.PBP`.

`tools/make_music.py` builds them from your own discs (needs Python 3 and ffmpeg):

    python make_music.py --pc "<Steam>/steamapps/common/Tomb Raider (I)" --psx tr1.cue music

- `--pc`: the Steam/GOG release ships CD audio tracks 2-10 (`02.mp3` ... `10.mp3`): title, level ambience
  and cutscenes.
- `--psx`: the PlayStation disc has the complete soundtrack as CD audio, track N = game track N (2-57):
  the music cues that triggers start during play, the secret jingle, Lara's Home voice and the cistern
  ambience. Use the cue sheet of a bin/cue rip, or extract a CHD image first with
  `chdman extractcd -i game.chd -o tr1.cue`.

With both sources you get all 59 tracks (about 24 MB). The PC files are used where both have the
same music (game tracks 2, 5, 22-25); the ambience tracks 58-60 exist only on the PC disc.

Manual conversion of the PC tracks is also possible:

| CD track | file name     |
|----------|---------------|
| 02.mp3   | track_02.ogg  |
| 03.mp3   | track_05.ogg  |
| 04.mp3   | track_58.ogg  |
| 05.mp3   | track_59.ogg  |
| 06.mp3   | track_60.ogg  |
| 07.mp3   | track_22.ogg  |
| 08.mp3   | track_23.ogg  |
| 09.mp3   | track_24.ogg  |
| 10.mp3   | track_25.ogg  |

Example with ffmpeg: `ffmpeg -i 03.mp3 -c:a libvorbis -q:a 3 -ar 44100 -ac 2 track_05.ogg`

Without the PlayStation tracks the music cues, the secret jingle and Lara's Home voice stay silent.

Tested on a PSP-1000 with the Steam tracks plus the tracks of the US PlayStation disc (Rev 2).
That PSX track N = game track N was checked by comparing the PC and PSX versions of tracks 5 and 22.

## Building

Needs the PSP toolchain (pspdev). In `src/platform/psp`:

    make            # EBOOT.PBP + OpenLara.prx
    make DEVTOOLS=1 # adds perf.log, boot.log and the psplink auto test (devtools.h)

## Changes compared to the upstream PSP port

- TR1 PC levels: 8-bit tiles with a 256 color palette (GU_PSM_T8) next to the TR1 PSX format.
- Scissor: `sceGuScissor` takes x, y, width, height. Passing end coordinates made the GU draw past the
  depth buffer into texture memory in eDRAM.
- GU command buffer: libpspgu writes the display list through the uncached address. The buffer is now
  cache line aligned and its cache lines are invalidated once; dirty lines of reused heap memory were
  written back over the commands (crash on the first `sceGuFinish` when started from the XMB).
- Inventory: no division by zero on an empty page (the PSP FPU raises an exception).
- Music start does not stall the game: tracks are opened in a low priority thread, the OGG decoder is
  created outside the mixer lock, missing tracks are only searched once.
- Health / air bars: drawn with their original colors (the GU path has no texture atlas).
- On-screen button names show the PSP buttons.
