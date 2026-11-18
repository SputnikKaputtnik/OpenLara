"""Builds the music folder for the PSP port from your own Tomb Raider 1 discs.

Output: track_XX.ogg (XX = game track number), 44100 Hz stereo OGG Vorbis, which is what the PSP
build plays (it has no MP3 decoder and does not resample). Copy the files next to EBOOT.PBP.

Sources (one or both):
  --pc DIR     folder with the CD audio of the PC release, as shipped by Steam/GOG: 02.mp3 ... 10.mp3
               (title, level ambience, cutscenes)
  --psx CUE    cue sheet of the PlayStation disc, e.g. from "chdman extractcd -i game.chd -o tr1.cue"
               or any raw bin/cue rip. The PlayStation disc has all music: CD audio track N is
               game track N (2..57), including the music cues, Lara's Home voice and the secret jingle.
               The PC versions of tracks 2, 5 and 22-25 are the same music; the PC files are preferred.

Usage: python make_music.py [--pc DIR] [--psx CUE] [--ffmpeg PATH] [--quality Q] OUTDIR
Needs ffmpeg (https://ffmpeg.org) on the PATH or given with --ffmpeg.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile

# PC CD track -> game track (inverse of TR1_TRACK_MAPPING in src/gameflow.h)
PC_TRACKS = {2: 2, 3: 5, 4: 58, 5: 59, 6: 60, 7: 22, 8: 23, 9: 24, 10: 25}


def read_cue(cue):
    """Returns (bin path, [(track number, type, start sector of INDEX 01)])."""
    text = open(cue, encoding='latin-1').read()
    files = re.findall(r'FILE\s+"([^"]+)"', text)
    if len(files) != 1:
        sys.exit('only single-file cue sheets are supported (found %d FILE entries)' % len(files))
    tracks, cur = [], None
    for line in text.splitlines():
        m = re.match(r'\s*TRACK\s+(\d+)\s+(\S+)', line)
        if m:
            cur = (int(m.group(1)), m.group(2).upper())
            continue
        m = re.match(r'\s*INDEX\s+01\s+(\d+):(\d+):(\d+)', line)
        if m and cur:
            tracks.append((cur[0], cur[1], (int(m.group(1)) * 60 + int(m.group(2))) * 75 + int(m.group(3))))
            cur = None
    return os.path.join(os.path.dirname(os.path.abspath(cue)), files[0]), tracks


def encode(ffmpeg, args, out, quality, data=None):
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y'] + args + \
          ['-c:a', 'libvorbis', '-q:a', str(quality), '-ar', '44100', '-ac', '2', out]
    subprocess.run(cmd, input=data, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pc', help='folder with 02.mp3 ... 10.mp3 (Steam/GOG)')
    ap.add_argument('--psx', help='cue sheet of the PlayStation disc')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg'), help='path to ffmpeg')
    ap.add_argument('--quality', default=3, type=int, help='Vorbis quality 0..10 (default 3, ~112 kbit/s)')
    ap.add_argument('outdir')
    a = ap.parse_args()
    if not a.pc and not a.psx:
        ap.error('give --pc and/or --psx')
    if not a.ffmpeg:
        ap.error('ffmpeg not found, use --ffmpeg')
    os.makedirs(a.outdir, exist_ok=True)
    done = {}

    if a.pc:
        for cd, game in sorted(PC_TRACKS.items()):
            src = os.path.join(a.pc, '%02d.mp3' % cd)
            if not os.path.exists(src):
                print('missing', src)
                continue
            encode(a.ffmpeg, ['-i', src], os.path.join(a.outdir, 'track_%02d.ogg' % game), a.quality)
            done[game] = 'PC %02d.mp3' % cd

    if a.psx:
        binpath, tracks = read_cue(a.psx)
        total = os.path.getsize(binpath) // 2352
        with open(binpath, 'rb') as f:
            for i, (num, kind, start) in enumerate(tracks):
                if kind != 'AUDIO' or num in done:
                    continue
                end = tracks[i + 1][2] if i + 1 < len(tracks) else total
                f.seek(start * 2352)
                pcm = f.read((end - start) * 2352)  # CD audio: 44100 Hz, 16 bit, stereo, little endian
                encode(a.ffmpeg, ['-f', 's16le', '-ar', '44100', '-ac', '2', '-i', '-'],
                       os.path.join(a.outdir, 'track_%02d.ogg' % num), a.quality, pcm)
                done[num] = 'PSX track %d' % num

    for game in sorted(done):
        print('track_%02d.ogg  <- %s' % (game, done[game]))
    print('%d tracks written to %s' % (len(done), a.outdir))


if __name__ == '__main__':
    main()
