#ifndef H_PSP_DEVTOOLS
#define H_PSP_DEVTOOLS

// Developer tools for the PSP port, only built with "make DEVTOOLS=1":
//  - perf.log  : every 5 s frame rate, CPU+GU time per frame, worst frame, slow frames, heap usage
//  - boot.log  : startup markers written synchronously next to the EBOOT (survives crashes, works from the XMB)
//  - auto test : "./OpenLara.prx auto [tokens]" via psplink, plays through levels and logs per-level stats
//                tokens: level names (GYM, 1, 10C, ...), tN = seconds per level, still, walk, title, noperf

#include <malloc.h>
#include <stdarg.h>

namespace Dev {

    u64 tick() {
        u64 t;
        sceRtcGetCurrentTick(&t);
        return t;
    }

    float ms(u64 t) { return float(t) * 1000.0f / float(osTimerFreq); }

// boot log
    bool bootToFile = false;

    void mark(const char *fmt, ...) {
        char line[200];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (n > int(sizeof(line)) - 1) n = sizeof(line) - 1;
        fputs(line, stdout);
        if (!bootToFile) return;
        SceUID fd = sceIoOpen("boot.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, line, n);
            sceIoClose(fd);
        }
    }

// perf log
    namespace Perf {
        char  buf[8192];
        int   len = 0;
        u64   tickStart, windowStart, lastFlush, lastFrameEnd;
        int   frames, slow, heapPeak;
        u64   workSum, workMax, frameMax;
        int   lastLevel = -2;

        void flush() {
            if (!len) return;
            FILE *f = fopen("perf.log", "ab");
            if (f) {
                fwrite(buf, 1, len, f);
                fclose(f);
            }
            len = 0;
        }

        void add(const char *fmt, ...) {
            if (len > (int)sizeof(buf) - 256) flush();
            va_list args;
            va_start(args, fmt);
            len += vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
            va_end(args);
        }

        void resetWindow() {
            frames = slow = 0;
            workSum = workMax = frameMax = 0;
        }

        void begin() {
            tickStart = windowStart = lastFlush = lastFrameEnd = tick();
            heapPeak = 0;
            resetWindow();
            add("=== OpenLara perf log (build " __DATE__ " " __TIME__ ") ===\n");
        }

        // work = ticks for update + render + GU sync, t = time after the buffer swap
        void frame(u64 work, u64 t) {
            int levelId = Game::level ? int(Game::level->level.id) : -1;
            if (levelId != lastLevel) {
                const char *name = (levelId >= 0 && levelId < TR::LVL_MAX) ? TR::LEVEL_INFO[levelId].name : "-";
                add("--- %7.1fs  level %s ---\n", ms(t - tickStart) / 1000.0f, name);
                lastLevel = levelId;
                flush();
                windowStart = lastFrameEnd = tick(); // loading time is not a slow frame
                resetWindow();
                return;
            }

            u64 frameTime = t - lastFrameEnd;
            lastFrameEnd = t;

            frames++;
            workSum += work;
            if (work > workMax)       workMax  = work;
            if (frameTime > frameMax) frameMax = frameTime;
            if (ms(frameTime) > 25.0f) slow++;

            if (ms(t - windowStart) >= 5000.0f) {
                struct mallinfo mi = mallinfo();
                int heapKB = mi.uordblks / 1024;
                if (heapKB > heapPeak) heapPeak = heapKB;

                float secs = ms(t - windowStart) / 1000.0f;
                add("%7.1fs  fps %4.1f  work avg %4.1f max %4.1f ms  frame max %5.1f ms  slow %3d  heap %5d KB (peak %5d)\n",
                    ms(t - tickStart) / 1000.0f, frames / secs, ms(workSum) / frames, ms(workMax), ms(frameMax), slow, heapKB, heapPeak);

                windowStart = t;
                resetWindow();

                if (ms(t - lastFlush) >= 60000.0f) { // write to the memory stick at most once per minute
                    flush();
                    lastFlush = lastFrameEnd = tick();
                }
            }
        }
    }

// auto test
    namespace AutoTest {
        const TR::LevelID ALL_LEVELS[] = {
            TR::LVL_TR1_GYM, TR::LVL_TR1_1,  TR::LVL_TR1_2,  TR::LVL_TR1_3A,  TR::LVL_TR1_3B,
            TR::LVL_TR1_4,   TR::LVL_TR1_5,  TR::LVL_TR1_6,  TR::LVL_TR1_7A,  TR::LVL_TR1_7B,
            TR::LVL_TR1_8A,  TR::LVL_TR1_8B, TR::LVL_TR1_8C, TR::LVL_TR1_10A, TR::LVL_TR1_10B, TR::LVL_TR1_10C,
        };
        const int ALL_COUNT = sizeof(ALL_LEVELS) / sizeof(ALL_LEVELS[0]);

        bool        active       = false;
        TR::LevelID LEVELS[ALL_COUNT];
        int         COUNT        = 0;
        int         secsPerLevel = 25;
        bool        walk         = false; // no fly mode: Lara walks, camera stays level
        bool        viaTitle     = false; // enter the level through the title menu with real button presses
        bool        still        = false; // no movement
        bool        noPerf       = false;
        bool        tour         = false; // fly Lara through the rooms of the level, camera turning
        int         tourStep     = -1;
        int         tourRoom     = -1;
        int         frameMaxRoom = -1;
        bool        invWasActive = false;
        float       frameMaxAt   = 0.0f;
        char        pending[512];         // log text collected during a level, written at its end
        u64         excluded     = 0;     // time spent writing screenshots (not part of the measurement)
        bool        titleDone    = false;
        u64         titleT       = 0;
        int         cur          = 0;
        u64         levelStart   = 0;
        bool        dozySet      = false;
        u64         tPattern     = 0;
        int         shotsTaken   = 0;
        // per level stats (from 2 s after the level start)
        int         frames, slow;
        u64         workSum, workMax, frameMax, lastT;
        float       loadSecs;

        void line(const char *fmt, ...) {
            char tmp[256];
            va_list args;
            va_start(args, fmt);
            vsnprintf(tmp, sizeof(tmp), fmt, args);
            va_end(args);
            LOG("%s", tmp);
            FILE *f = fopen("autotest.log", "ab");
            if (f) {
                fputs(tmp, f);
                fclose(f);
            }
        }

        void resetStats() {
            frames = slow = 0;
            workSum = workMax = frameMax = 0;
            lastT = 0;
            dozySet = false;
            shotsTaken = 0;
            tourStep = tourRoom = frameMaxRoom = -1;
            excluded = 0;
        }

        // log text collected during the measurement, written at the end of the level
        void note(const char *fmt, ...) {
            int len = strlen(pending);
            va_list args;
            va_start(args, fmt);
            vsnprintf(pending + len, sizeof(pending) - len, fmt, args);
            va_end(args);
        }

        // tour: place the (flying) Lara in an open sector near the room centre
        void tourPlace(Lara *lara, int roomIndex, float angle) {
            TR::Room &r = Game::level->level.rooms[roomIndex];
            int cx = r.xSectors / 2, cz = r.zSectors / 2;
            int bx = -1, bz = -1;
            for (int d = 0; d < max(r.xSectors, r.zSectors) && bx < 0; d++)
                for (int x = cx - d; x <= cx + d && bx < 0; x++)
                    for (int z = cz - d; z <= cz + d && bx < 0; z++) {
                        if (x < 1 || z < 1 || x >= r.xSectors - 1 || z >= r.zSectors - 1) continue;
                        TR::Room::Sector &s = r.sectors[x * r.zSectors + z];
                        // floorIndex 0 = no floor data, so no triggers (a level end trigger would finish the level)
                        if (s.floorIndex == 0 && s.floor != TR::NO_FLOOR && s.floor > s.ceiling + 2) { bx = x; bz = z; }
                    }
            if (bx < 0) return; // no open sector: keep the previous room
            TR::Room::Sector &s = r.sectors[bx * r.zSectors + bz];
            float y = max(float(s.ceiling * 256 + 256), float(s.floor * 256 - 768));
            vec3 pos(float(r.info.x + bx * 1024 + 512), y, float(r.info.z + bz * 1024 + 512));
            lara->reset(roomIndex, pos, angle, Lara::STAND_UNDERWATER);
            lara->dozy = true;
            tourRoom = roomIndex;
        }

        // screenshot of the displayed frame as 24-bit BMP (working directory, host0: under psplink)
        void saveShot(const char *name) {
            void *top;
            int bufWidth, fmt;
            sceDisplayGetFrameBuf(&top, &bufWidth, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
            if (!top || fmt != PSP_DISPLAY_PIXEL_FORMAT_565) return;
            const uint16 *src = (const uint16*)((uint32)top | 0x40000000); // uncached
            const int W = 480, H = 272, rowBytes = W * 3;
            static uint8 row[480 * 3];
            FILE *f = fopen(name, "wb");
            if (!f) return;
            uint8 hdr[54] = { 'B', 'M' };
            *(uint32*)(hdr + 2)  = 54 + rowBytes * H;
            *(uint32*)(hdr + 10) = 54;
            *(uint32*)(hdr + 14) = 40;
            *(int32*)(hdr + 18)  = W;
            *(int32*)(hdr + 22)  = H;
            *(uint16*)(hdr + 26) = 1;
            *(uint16*)(hdr + 28) = 24;
            fwrite(hdr, 1, 54, f);
            for (int y = H - 1; y >= 0; y--) {
                const uint16 *p = src + y * bufWidth;
                for (int x = 0; x < W; x++) {
                    uint16 c = p[x]; // GU_PSM_5650
                    row[x * 3 + 0] = ((c >> 11) & 31) << 3;
                    row[x * 3 + 1] = ((c >> 5)  & 63) << 2;
                    row[x * 3 + 2] = ( c        & 31) << 3;
                }
                fwrite(row, 1, rowBytes, f);
            }
            fclose(f);
        }

        void begin(int argc, char *argv[]) {
            COUNT = 0;
            for (int i = 2; i < argc; i++) {
                const char *a = argv[i];
                if (!a) continue;
                if (a[0] == 't' && a[1] >= '0' && a[1] <= '9') { secsPerLevel = atoi(a + 1); continue; }
                if (!strcmp(a, "noperf")) { noPerf   = true; continue; }
                if (!strcmp(a, "still"))  { still    = true; continue; }
                if (!strcmp(a, "title"))  { viaTitle = true; continue; }
                if (!strcmp(a, "walk"))   { walk     = true; continue; }
                if (!strcmp(a, "tour"))   { tour     = true; continue; }
                // level name: GYM, 1, L1, 10C, LEVEL2 ...
                char up[16];
                int n = 0;
                const char *p = (a[0] == 'L' && a[1] != 'E') ? a + 1 : a;
                for (; *p && n < 15; p++) up[n++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
                up[n] = 0;
                char full[24];
                snprintf(full, sizeof(full), (!strncmp(up, "LEVEL", 5) || !strcmp(up, "GYM")) ? "%s" : "LEVEL%s", up);
                for (int k = 0; k < ALL_COUNT && COUNT < ALL_COUNT; k++)
                    if (!strcmp(TR::LEVEL_INFO[ALL_LEVELS[k]].name, full)) LEVELS[COUNT++] = ALL_LEVELS[k];
            }
            if (!COUNT)
                for (int k = 0; k < ALL_COUNT; k++) LEVELS[COUNT++] = ALL_LEVELS[k];

            active = true;
            cur = 0;
            resetStats();
            line("=== AUTOTEST start (build %s %s), %d levels, %d s each ===\n", __DATE__, __TIME__, COUNT, secsPerLevel);
            line("%-12s %6s %5s %10s %10s %10s %5s %8s\n", "level", "load", "fps", "work avg", "work max", "frame max", "slow", "heap KB");
        }

        void frame(u64 work, u64 t) {
            if (!active || !Game::level) return;
            TR::LevelID want = LEVELS[cur];

            if (viaTitle && !titleDone) {
                // input() presses X in the title menu like a player; the test measures the level that gets loaded
                if (Game::level->level.isTitle()) {
                    if (Game::level->nextLevel == TR::LVL_MAX) {
                        if (!titleT) titleT = t;
                        if (ms(t - titleT) > 15000.0f) {
                            line("title: still in the title menu after 15 s, abort\n");
                            active = false;
                            Core::quit();
                        }
                    }
                } else if (Game::level->nextLevel == TR::LVL_MAX) {
                    LEVELS[cur] = Game::level->level.id;
                    titleDone = true;
                    titleT = 0;
                }
                return;
            }

            // still loading: wrong level, or the old level object (nextLevel stays set until it is rebuilt)
            if (Game::level->level.id != want || Game::level->nextLevel != TR::LVL_MAX) {
                if (!levelStart) levelStart = t;
                return;
            }
            if (!lastT) {
                loadSecs = levelStart ? ms(t - levelStart) / 1000.0f : 0.0f;
                levelStart = lastT = tPattern = t;
                return;
            }

            float elapsed = ms(t - levelStart) / 1000.0f;

            if (inventory && inventory->isActive() != invWasActive) {
                invWasActive = inventory->isActive();
                Lara *lara = (Lara*)Game::level->getLara(0);
                note("  %.1fs inventory %s: page %d, %d items on it, lara health %.0f room %d\n", elapsed,
                    invWasActive ? "opened" : "closed", int(inventory->page), inventory->getItemsCount(inventory->page),
                    lara ? lara->health : -1.0f, lara ? lara->getRoomIndex() : -1);
            }

            if (tour && elapsed > 1.0f) {
                // 1.5 s per stop, stops spread evenly over all rooms; camera turns 240 deg/s
                Lara *lara = (Lara*)Game::level->getLara(0);
                int rooms  = Game::level->level.roomsCount;
                int stops  = max(1, int((secsPerLevel - 1) / 1.5f));
                int step   = int((elapsed - 1.0f) / 1.5f);
                float ang  = (elapsed - 1.0f) * (240.0f * DEG2RAD);
                if (lara && rooms > 0) {
                    if (step != tourStep) {
                        tourStep = step;
                        tourPlace(lara, (step * rooms / stops) % rooms, ang);
                    }
                    lara->angle.y = ang;
                }
            }

            if (!walk && !tour && !dozySet && elapsed > 1.0f) {
                Lara *lara = (Lara*)Game::level->getLara(0);
                if (lara) lara->setDozy(true);
                dozySet = true;
            }

            // two screenshots per level: after 4 s and shortly before the end
            if ((shotsTaken == 0 && elapsed >= 4.0f) || (shotsTaken == 1 && elapsed >= float(secsPerLevel) - 1.0f)) {
                char name[48];
                snprintf(name, sizeof(name), "at_%s_%d.bmp", TR::LEVEL_INFO[want].name, shotsTaken + 1);
                saveShot(name);
                shotsTaken++;
                u64 now = tick();
                if (elapsed > 2.0f) excluded += now - t; // writing time counts neither as a slow frame nor in the fps
                lastT = now;
                return;
            }

            u64 frameTime = t - lastT;
            lastT = t;
            if (elapsed > 2.0f) {
                frames++;
                workSum += work;
                if (work > workMax)       workMax  = work;
                if (frameTime > frameMax) {
                    Lara *lara = (Lara*)Game::level->getLara(0);
                    frameMax     = frameTime;
                    frameMaxRoom = lara ? lara->getRoomIndex() : -1;
                    frameMaxAt   = elapsed;
                }
                if (ms(frameTime) > 25.0f) slow++;
            }

            if (elapsed >= float(secsPerLevel)) {
                struct mallinfo mi = mallinfo();
                float secs = elapsed - 2.0f - ms(excluded) / 1000.0f;
                // file writes only here, after the measurement: on the memory stick they take 100+ ms
                if (pending[0]) {
                    line("%s", pending);
                    pending[0] = 0;
                }
                line("%-12s %5.1fs %5.1f %8.2fms %8.2fms %8.2fms %5d %8d\n",
                    TR::LEVEL_INFO[want].name, loadSecs, frames / secs,
                    frames ? ms(workSum) / frames : 0.0f, ms(workMax), ms(frameMax), slow, mi.uordblks / 1024);
                line("  frame max at %.1fs in room %d", frameMaxAt, frameMaxRoom);
                if (tour)
                    line(", tour: %d rooms, %d stops", Game::level->level.roomsCount, tourStep + 1);
                line("\n");

                cur++;
                levelStart = 0;
                resetStats();
                if (cur >= COUNT) {
                    line("=== AUTOTEST end: %d levels ===\n", COUNT);
                    active = false;
                    Core::quit();
                } else if (viaTitle) {
                    titleDone = false;
                    Game::level->loadLevel(TR::LVL_TR1_TITLE);
                } else
                    Game::level->loadLevel(LEVELS[cur]);
            }
        }

        // movement pattern: always forward, turn every 3 s (alternating left/right), occasional action button
        void input(SceCtrlData &pad) {
            if (!active) return;
            pad.Buttons = 0;
            pad.Lx = pad.Ly = 128;
            if (viaTitle && !titleDone) {
                if (titleT) {
                    float tt = ms(tick() - titleT) / 1000.0f;
                    if ((tt > 3.0f && tt < 3.15f) || (tt > 5.0f && tt < 5.15f) || (tt > 8.0f && tt < 8.15f))
                        pad.Buttons |= PSP_CTRL_CROSS;
                }
                return;
            }
            if (!Game::level || !lastT || still || tour) return;
            float t = ms(tick() - tPattern) / 1000.0f;
            float phase = fmodf(t, 3.0f);
            int   cycle = int(t / 3.0f);
            pad.Buttons |= PSP_CTRL_UP;
            if (phase > 2.3f)
                pad.Buttons |= (cycle & 1) ? PSP_CTRL_LEFT : PSP_CTRL_RIGHT;
            if ((cycle % 4) == 3 && phase < 0.4f)
                pad.Buttons |= PSP_CTRL_CROSS;
        }
    }

// interface used by main.cpp
    void init(int argc, char *argv[]) {
        if (argc > 0 && argv[0] && !strncmp(argv[0], "ms0:", 4)) {
            bootToFile = true;
            sceIoRemove("boot.log");
        }
        mark("boot: %s, build %s %s\n", argc > 0 && argv[0] ? argv[0] : "-", __DATE__, __TIME__);
        if (argc > 1 && argv[1] && !strcmp(argv[1], "auto"))
            AutoTest::begin(argc, argv);
    }

    // returns true if the auto test decides how to start: out = level file, or empty = title menu
    bool startLevel(char *out, int outSize) {
        if (!AutoTest::active) return false;
        if (AutoTest::viaTitle)
            out[0] = 0;
        else
            snprintf(out, outSize, "DATA/%s.PHD", TR::LEVEL_INFO[AutoTest::LEVELS[0]].name);
        return true;
    }

    void input(SceCtrlData &pad) {
        AutoTest::input(pad);
    }

    void started() {
        if (!AutoTest::noPerf) Perf::begin();
    }

    void frame(u64 work, u64 t) {
        static int frameNo = 0;
        if (!AutoTest::noPerf) Perf::frame(work, t);
        AutoTest::frame(work, t);
        frameNo++;
        if (frameNo <= 3 || frameNo == 60 || frameNo == 600)
            mark("boot: frame %d, level %s\n", frameNo, Game::level ? TR::LEVEL_INFO[Game::level->level.id].name : "-");
    }

    void shutdown() {
        if (!AutoTest::noPerf) {
            Perf::add("=== end ===\n");
            Perf::flush();
        }
    }
}

// called by Level::playTrack (level.h): which music track was requested, how long the request blocked
// Only buffered here: this can run while another thread reads a file over usbhostfs, and two
// file operations at the same time desync the psplink USB link. AutoTest::frame() writes it out.
void devTrackLog(int track, int ms, bool playing) {
    if (Dev::AutoTest::active)
        Dev::AutoTest::note("  music track %d: %d ms, %s\n", track, ms, playing ? "playing" : "not found");
}

#endif
