"""Raw MODE1/2352 CD image (e.g. GAME.GOG of the Steam/GOG Tomb Raider 1) -> ISO with 2048 byte sectors.
Sector layout MODE1/2352: 12 bytes sync, 4 bytes header, 2048 bytes data, 288 bytes EDC/ECC.
Usage: python bin2iso.py GAME.GOG game.iso   (reads the source, writes only the target)"""
import sys

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"
src, dst = sys.argv[1], sys.argv[2]
n = bad = 0
with open(src, "rb") as f, open(dst, "wb") as o:
    while True:
        s = f.read(2352)
        if len(s) < 2352:
            break
        if s[:12] != SYNC or s[15] != 1:
            bad += 1
        o.write(s[16:16 + 2048])
        n += 1
print(f"{n} sectors, {bad} without MODE1 sync/header")
