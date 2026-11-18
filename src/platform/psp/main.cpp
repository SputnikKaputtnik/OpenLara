#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pspkernel.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <pspdebug.h>

PSP_MODULE_INFO("OpenLara", 0, 1, 1);
PSP_HEAP_SIZE_KB(20480);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU | PSP_THREAD_ATTR_NO_FILLSTACK);

#include "game.h"

#define BUF_WIDTH   (512)
#define SCR_WIDTH   (480)
#define SCR_HEIGHT  (272)

int exitCallback(int arg1, int arg2, void *common) {
    Core::quit();
    return 0;
}

int callbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
    sceKernelStartThread(thid, 0, 0);
    return thid;
}


// multi-threading
void* osMutexInit() {
    SceUID *mutex = new SceUID();
    *mutex = sceKernelCreateSema(NULL, 0, 1, 1, 0);
    return mutex;
}

void osMutexFree(void *obj) {
    sceKernelDeleteSema(*(SceUID*)obj);
    delete (SceUID*)obj;
}

void osMutexLock(void *obj) {
    sceKernelWaitSema(*(SceUID*)obj, 1, NULL);
}

void osMutexUnlock(void *obj) {
    sceKernelSignalSema(*(SceUID*)obj, 1);
}

void* osRWLockInit() {
    return osMutexInit();
}

void osRWLockFree(void *obj) {
    osMutexFree(obj);
}

void osRWLockRead(void *obj) {
    osMutexLock(obj);
}

void osRWUnlockRead(void *obj) {
    osMutexUnlock(obj);
}

void osRWLockWrite(void *obj) {
    osMutexLock(obj);
}

void osRWUnlockWrite(void *obj) {
    osMutexUnlock(obj);
}

// timing
int osStartTime = 0;
int osTimerFreq;

int osGetTimeMS() {
    u64 time;
    sceRtcGetCurrentTick(&time);
    return int(time * 1000 / osTimerFreq - osStartTime);
}

// input
bool osJoyReady(int index) {
    return index == 0;
}

void osJoyVibrate(int index, float L, float R) {
    //
}

void joyInit() {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

#ifdef PSP_DEVTOOLS
    #include "devtools.h"
    #define DEV_MARK(...) Dev::mark(__VA_ARGS__)
#else
    namespace Dev {
        inline void init(int, char**)          {}
        inline bool startLevel(char*, int)     { return false; }
        inline void input(SceCtrlData&)        {}
        inline void started()                  {}
        inline u64  tick()                     { return 0; }
        inline void frame(u64, u64)            {}
        inline void shutdown()                 {}
    }
    #define DEV_MARK(...)
#endif

void joyUpdate() {
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    Dev::input(pad);

    Input::setJoyDown(0, jkUp,     (pad.Buttons & PSP_CTRL_UP));
    Input::setJoyDown(0, jkDown,   (pad.Buttons & PSP_CTRL_DOWN));
    Input::setJoyDown(0, jkLeft,   (pad.Buttons & PSP_CTRL_LEFT));
    Input::setJoyDown(0, jkRight,  (pad.Buttons & PSP_CTRL_RIGHT));
    Input::setJoyDown(0, jkA,      (pad.Buttons & PSP_CTRL_CROSS));
    Input::setJoyDown(0, jkB,      (pad.Buttons & PSP_CTRL_CIRCLE));
    Input::setJoyDown(0, jkX,      (pad.Buttons & PSP_CTRL_SQUARE));
    Input::setJoyDown(0, jkY,      (pad.Buttons & PSP_CTRL_TRIANGLE));
    Input::setJoyDown(0, jkLB,     (pad.Buttons & PSP_CTRL_LTRIGGER));
    Input::setJoyDown(0, jkRB,     (pad.Buttons & PSP_CTRL_RTRIGGER));
    Input::setJoyDown(0, jkStart,  (pad.Buttons & PSP_CTRL_START));
    Input::setJoyDown(0, jkSelect, (pad.Buttons & PSP_CTRL_SELECT));

    vec2 stick = vec2(float(pad.Lx), float(pad.Ly)) / 128.0f - 1.0f;
    if (fabsf(stick.x) < 0.2f && fabsf(stick.y) < 0.2f)
        stick = vec2(0.0f);
    Input::setJoyPos(0, jkL, stick);
}

void sndFill(void* buf, unsigned int length, void *userdata) {
    Sound::fill((Sound::Frame*)buf, length);
}

void sndInit() {
    pspAudioInit();
    pspAudioSetChannelCallback(0, sndFill, NULL);
}

// Start level (skips the title menu), in order of priority:
//  1. argument:  ./OpenLara.prx 10B  (psplink)
//  2. level menu: hold L while the game starts
//  3. file startlevel.txt next to the EBOOT, first line e.g. "10B"
// Short names: "10B" -> DATA/LEVEL10B.PHD, "GYM" -> DATA/GYM.PHD; paths are used as they are.
static bool fileExists(const char *name) {
    FILE *f = fopen(name, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool resolveLevelName(const char *in, char *out, int outSize) {
    char name[48];
    int n = 0;
    for (const char *p = in; *p && n < (int)sizeof(name) - 1; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') break;
        name[n++] = (*p >= 'a' && *p <= 'z') ? (*p - 32) : *p;
    }
    name[n] = 0;
    if (!n) return false;

    if (strchr(name, '/') || strchr(name, '.')) {
        snprintf(out, outSize, "%s", in);
        out[strcspn(out, "\r\n \t")] = 0;
        return fileExists(out);
    }

    const char *candidates[] = { "DATA/LEVEL%s.PHD", "DATA/%s.PHD", "DATA/LEVEL%s.PSX", "PSXDATA/%s.PSX" };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        snprintf(out, outSize, candidates[i], name);
        if (fileExists(out)) return true;
    }
    LOG("start level \"%s\" not found\n", name);
    return false;
}

static const struct { const char *file; const char *title; } LEVEL_MENU[] = {
    { "DATA/GYM.PHD",      "Lara's Home"               },
    { "DATA/LEVEL1.PHD",   "1  Caves"                  },
    { "DATA/LEVEL2.PHD",   "2  City of Vilcabamba"     },
    { "DATA/LEVEL3A.PHD",  "3  Lost Valley"            },
    { "DATA/LEVEL3B.PHD",  "4  Tomb of Qualopec"       },
    { "DATA/LEVEL4.PHD",   "5  St. Francis' Folly"     },
    { "DATA/LEVEL5.PHD",   "6  Colosseum"              },
    { "DATA/LEVEL6.PHD",   "7  Palace Midas"           },
    { "DATA/LEVEL7A.PHD",  "8  The Cistern"            },
    { "DATA/LEVEL7B.PHD",  "9  Tomb of Tihocan"        },
    { "DATA/LEVEL8A.PHD",  "10 City of Khamoon"        },
    { "DATA/LEVEL8B.PHD",  "11 Obelisk of Khamoon"     },
    { "DATA/LEVEL8C.PHD",  "12 Sanctuary of the Scion" },
    { "DATA/LEVEL10A.PHD", "13 Natla's Mines"          },
    { "DATA/LEVEL10B.PHD", "14 Atlantis"               },
    { "DATA/LEVEL10C.PHD", "15 The Great Pyramid"      },
    { "DATA/EGYPT.PHD",    "Unfinished Business: Return to Egypt"   },
    { "DATA/CAT.PHD",      "Unfinished Business: Temple of the Cat" },
};

static bool levelMenu(char *out, int outSize) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    if (!(pad.Buttons & PSP_CTRL_LTRIGGER))
        return false;

    int idx[sizeof(LEVEL_MENU) / sizeof(LEVEL_MENU[0])];
    int count = 0;
    for (int i = 0; i < (int)(sizeof(LEVEL_MENU) / sizeof(LEVEL_MENU[0])); i++)
        if (fileExists(LEVEL_MENU[i].file))
            idx[count++] = i;
    if (!count)
        return false;

    pspDebugScreenInit();
    int sel = 0, top = 0;
    const int visible = 26;
    uint32 prev = pad.Buttons;
    bool redraw = true;

    while (true) {
        if (redraw) {
            if (sel < top) top = sel;
            if (sel >= top + visible) top = sel - visible + 1;
            pspDebugScreenClear();
            pspDebugScreenSetTextColor(0xFF40C0FF);
            pspDebugScreenPrintf(" OpenLara - level select\n");
            pspDebugScreenSetTextColor(0xFF808080);
            pspDebugScreenPrintf(" D-pad: select   X: start   O: title menu\n\n");
            for (int i = top; i < count && i < top + visible; i++) {
                pspDebugScreenSetTextColor(i == sel ? 0xFFFFFFFF : 0xFFA0A0A0);
                pspDebugScreenPrintf(" %s %s\n", i == sel ? ">" : " ", LEVEL_MENU[idx[i]].title);
            }
            redraw = false;
        }

        sceDisplayWaitVblankStart();
        sceCtrlReadBufferPositive(&pad, 1);
        uint32 pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        if (pressed & PSP_CTRL_DOWN)  { sel = (sel + 1) % count;         redraw = true; }
        if (pressed & PSP_CTRL_UP)    { sel = (sel + count - 1) % count; redraw = true; }
        if (pressed & PSP_CTRL_RIGHT) { sel = min(sel + 5, count - 1);   redraw = true; }
        if (pressed & PSP_CTRL_LEFT)  { sel = max(sel - 5, 0);           redraw = true; }
        if (pressed & PSP_CTRL_CIRCLE) {
            pspDebugScreenClear();
            return false;
        }
        if (pressed & PSP_CTRL_CROSS) {
            snprintf(out, outSize, "%s", LEVEL_MENU[idx[sel]].file);
            pspDebugScreenClear();
            return true;
        }
    }
}

static bool getStartLevel(int argc, char *argv[], char *out, int outSize) {
    if (argc > 1 && argv[1] && resolveLevelName(argv[1], out, outSize))
        return true;

    if (levelMenu(out, outSize))
        return true;

    FILE *f = fopen("startlevel.txt", "rb");
    if (!f) return false;
    char buf[64] = { 0 };
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return resolveLevelName(buf, out, outSize);
}

// level file name -> level id ("DATA/LEVEL10C.PHD" -> LVL_TR1_10C)
static TR::LevelID levelIdByFile(const char *file) {
    char base[32];
    int n = 0;
    const char *p = strrchr(file, '/');
    for (p = p ? p + 1 : file; *p && *p != '.' && n < 31; p++)
        base[n++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    base[n] = 0;
    for (int i = 0; i < TR::LVL_MAX; i++)
        if (TR::LEVEL_INFO[i].name && !strcmp(TR::LEVEL_INFO[i].name, base))
            return TR::LevelID(i);
    return TR::LVL_MAX;
}

int main(int argc, char *argv[]) {
    cacheDir[0] = saveDir[0] = contentDir[0] = 0;

    Dev::init(argc, argv);

    scePowerSetClockFrequency(333, 333, 166);
    setupCallbacks();

    // determine the start level before the GU is initialized (the level menu uses pspDebugScreen)
    char startLevel[64];
    bool directLoad = false;
    TR::LevelID pendingId = TR::LVL_MAX;
    if (Dev::startLevel(startLevel, sizeof(startLevel))) {
        directLoad = startLevel[0] != 0;
    } else if (getStartLevel(argc, argv, startLevel, sizeof(startLevel))) {
        // Start with the title as usual and load the level from there, the same way the passport does.
        // This path is verified on hardware from the XMB; loading the level directly in Game::init is not.
        pendingId = levelIdByFile(startLevel);
    }
    DEV_MARK("boot: start level %s\n", directLoad ? startLevel : (pendingId != TR::LVL_MAX ? TR::LEVEL_INFO[pendingId].name : "(title)"));

    sceGuInit();

    GAPI::beginCmdBuf();

    sceGuDrawBuffer(GU_PSM_5650, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)(BUF_WIDTH * SCR_HEIGHT * 2), BUF_WIDTH);
    sceGuDepthBuffer((void*)(BUF_WIDTH * SCR_HEIGHT * 2 * 2), BUF_WIDTH);

    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    sndInit();
    joyInit();

    osTimerFreq = sceRtcGetTickResolution();
    osStartTime = Core::getTime();

    if (directLoad)
        Game::init(startLevel);
    else
        Game::init();
    DEV_MARK("boot: Game::init ok\n");

    GAPI::submitCmdBuf();
    DEV_MARK("boot: first display list done\n");

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    GAPI::curBackBuffer = 0;

    Dev::started();
    int pendingFrames = 0;

    while (!Core::isQuit) {
        u64 t0 = Dev::tick();
        GAPI::beginCmdBuf();

        // pending start level: as soon as the title menu is ready, load it like the passport does
        if (pendingId != TR::LVL_MAX && Game::level && Game::level->level.isTitle() &&
            inventory->isActive() && inventory->titleTimer == 0.0f && ++pendingFrames > 20) {
            inventory->nextLevel = pendingId;
            inventory->toggle();
            pendingId = TR::LVL_MAX;
        }

        joyUpdate();
        Game::update();
        Game::render();
        GAPI::submitCmdBuf();
        u64 t1 = Dev::tick();
        Core::waitVBlank();
        GAPI::curBackBuffer = sceGuSwapBuffers();
        Dev::frame(t1 - t0, Dev::tick());
    }

    Dev::shutdown();

    Game::deinit();

    sceGuTerm();
    sceKernelExitGame();

    return 0;
}
