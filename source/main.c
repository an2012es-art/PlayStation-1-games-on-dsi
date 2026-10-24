// PSXEmulDS - Phase 1.5: SD browser + PS-X EXE and disc image (.cue/.bin/.iso) loader
// No sound.
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ENTRIES  256
#define VISIBLE      20
#define PSX_RAM_SIZE (2 * 1024 * 1024)
#define EXE_HEADER   0x800

typedef struct {
    char name[256];
    bool isDir;
} Entry;

typedef struct {
    u32 pc, gp, dest, size, sp;
} ExeInfo;

typedef struct {
    FILE *f;
    u32   secSize;   // 2048 (iso) or 2352 (raw bin)
    u32   dataOff;   // start of user data inside a sector
} Disc;

static Entry entries[MAX_ENTRIES];
static int   count;
static char  cwd[512] = "/";
static u8   *psxRam = NULL;

// shared scratch buffers (static to keep the small ARM9 stack free)
static u8   hdrBuf[EXE_HEADER];
static u8   secBuf[2048];
static u8   rawBuf[2352];
static u8   cnfBuf[2048];
static char pathBuf[768];

// ---------------------------------------------------------------- helpers

static u32 rd32(const u8 *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static bool hasExt(const char *n, const char *ext) {
    const char *dot = strrchr(n, '.');
    return dot && !strcasecmp(dot, ext);
}

static bool wanted(const char *n) {
    return hasExt(n, ".exe") || hasExt(n, ".psx") || hasExt(n, ".bin") ||
           hasExt(n, ".cue") || hasExt(n, ".iso");
}

static void waitB(void) {
    printf("\nPress B to go back");
    while (1) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B) break;
    }
}

// ---------------------------------------------------------------- browser

static int cmpEntry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->isDir != y->isDir) return y->isDir - x->isDir;
    return strcasecmp(x->name, y->name);
}

static void scanDir(void) {
    count = 0;
    DIR *d = opendir(cwd);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && count < MAX_ENTRIES) {
        if (!strcmp(e->d_name, ".")) continue;
        bool dir = (e->d_type == DT_DIR);
        if (!dir && !wanted(e->d_name)) continue;
        strncpy(entries[count].name, e->d_name, 255);
        entries[count].name[255] = 0;
        entries[count].isDir = dir;
        count++;
    }
    closedir(d);
    qsort(entries, count, sizeof(Entry), cmpEntry);
}

static void draw(int sel, int top) {
    consoleClear();
    printf("PSXEmulDS  %s\n", cwd);
    printf("--------------------------------\n");
    for (int i = top; i < count && i < top + VISIBLE; i++) {
        printf("%c %.28s%s\n", i == sel ? '>' : ' ',
               entries[i].name, entries[i].isDir ? "/" : "");
    }
    if (count == 0) printf("(empty)\n");
    printf("\nA:open  B:back  Up/Down:move");
}

static void goUp(void) {
    char *slash = strrchr(cwd, '/');
    if (!slash || slash == cwd) { strcpy(cwd, "/"); return; }
    *slash = 0;
    if (cwd[0] == 0) strcpy(cwd, "/");
}

// ---------------------------------------------------------------- PS-X EXE

static bool checkHeader(const u8 *hdr, ExeInfo *x) {
    if (memcmp(hdr, "PS-X EXE", 8) != 0) {
        printf("Not a PS-X EXE file\n");
        return false;
    }
    x->pc   = rd32(hdr + 0x10);
    x->gp   = rd32(hdr + 0x14);
    x->dest = rd32(hdr + 0x18) & 0x1FFFFFFF;
    x->size = rd32(hdr + 0x1C);
    x->sp   = rd32(hdr + 0x30);

    if (x->dest + x->size > PSX_RAM_SIZE) {
        printf("EXE does not fit in RAM\n");
        return false;
    }
    if (!psxRam) psxRam = (u8 *)malloc(PSX_RAM_SIZE);
    if (!psxRam) {
        printf("Out of memory\n");
        return false;
    }
    memset(psxRam, 0, PSX_RAM_SIZE);
    return true;
}

static void showInfo(const ExeInfo *x) {
    printf("PC   %08lX\nGP   %08lX\nSP   %08lX\n",
           (unsigned long)x->pc, (unsigned long)x->gp, (unsigned long)x->sp);
    printf("Dest %08lX\nSize %lu bytes\n",
           (unsigned long)x->dest, (unsigned long)x->size);
    printf("\nLoaded OK.\nCPU not implemented yet\n(phase 2)\n");
}

static void loadExe(const char *path) {
    consoleClear();
    printf("Loading:\n%s\n\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { printf("Cannot open file\n"); waitB(); return; }

    ExeInfo x;
    if (fread(hdrBuf, 1, EXE_HEADER, f) != EXE_HEADER || !checkHeader(hdrBuf, &x)) {
        fclose(f); waitB(); return;
    }
    size_t got = fread(psxRam + x.dest, 1, x.size, f);
    fclose(f);
    if (got != x.size) printf("Warning: short read\n");
    showInfo(&x);
    waitB();
}

// ---------------------------------------------------------------- disc images

// Read the 2048 user bytes of one sector.
static bool discRead(Disc *d, u32 lba, u8 *out) {
    if (fseek(d->f, (long)lba * (long)d->secSize + (long)d->dataOff, SEEK_SET)) return false;
    return fread(out, 1, 2048, d->f) == 2048;
}

static bool discOpen(Disc *d, const char *path) {
    d->f = fopen(path, "rb");
    if (!d->f) return false;

    // raw 2352-byte sectors (typical .bin)
    static const u8 sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    d->secSize = 2352;
    if (fseek(d->f, 16L * 2352, SEEK_SET) == 0 &&
        fread(rawBuf, 1, 2352, d->f) == 2352 &&
        memcmp(rawBuf, sync, 12) == 0) {
        u32 off = (rawBuf[15] == 2) ? 24 : 16;   // mode 2 : mode 1
        if (memcmp(rawBuf + off + 1, "CD001", 5) == 0) {
            d->dataOff = off;
            return true;
        }
    }
    // plain 2048-byte sectors (.iso)
    d->secSize = 2048;
    d->dataOff = 0;
    if (fseek(d->f, 16L * 2048, SEEK_SET) == 0 &&
        fread(secBuf, 1, 2048, d->f) == 2048 &&
        memcmp(secBuf + 1, "CD001", 5) == 0) {
        return true;
    }
    fclose(d->f);
    d->f = NULL;
    return false;
}

// Find a file inside one ISO9660 directory.
static bool findInDir(Disc *d, u32 lba, u32 size, const char *name,
                      u32 *outLba, u32 *outSize) {
    for (u32 off = 0; off < size; off += 2048) {
        if (!discRead(d, lba + off / 2048, secBuf)) return false;
        u32 p = 0;
        while (p < 2048) {
            u32 len = secBuf[p];
            if (len == 0) break;                       // padding to next sector
            u32 nl = secBuf[p + 32];
            char nm[40];
            u32 n = nl < 39 ? nl : 39;
            memcpy(nm, secBuf + p + 33, n);
            nm[n] = 0;
            char *semi = strchr(nm, ';');
            if (semi) *semi = 0;
            if (strcasecmp(nm, name) == 0) {
                *outLba  = rd32(secBuf + p + 2);
                *outSize = rd32(secBuf + p + 10);
                return true;
            }
            p += len;
        }
    }
    return false;
}

// BOOT = cdrom:\SLUS_006.91;1  ->  "SLUS_006.91"
static void parseBoot(const char *txt, char *out, size_t n) {
    out[0] = 0;
    const char *p = txt;
    while (*p && strncasecmp(p, "cdrom:", 6) != 0) p++;
    if (!*p) return;
    p += 6;
    while (*p == '\\' || *p == '/') p++;
    size_t i = 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n' && *p != ' ' && i < n - 1)
        out[i++] = *p++;
    out[i] = 0;
}

// FILE "game.bin" BINARY  ->  full path of game.bin next to the cue
static bool cueToBin(const char *cue, char *out, size_t n) {
    FILE *f = fopen(cue, "rb");
    if (!f) return false;
    static char txt[2048];
    size_t got = fread(txt, 1, sizeof(txt) - 1, f);
    fclose(f);
    txt[got] = 0;

    char *p = txt;
    while (*p && strncasecmp(p, "FILE", 4) != 0) p++;
    if (!*p) return false;
    char *q1 = strchr(p, '"');
    if (!q1) return false;
    char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    *q2 = 0;

    strncpy(out, cue, n - 1);
    out[n - 1] = 0;
    char *slash = strrchr(out, '/');
    if (slash) slash[1] = 0; else out[0] = 0;
    if (strlen(out) + strlen(q1 + 1) + 1 > n) return false;
    strcat(out, q1 + 1);
    return true;
}

static bool exeFromDisc(Disc *d, u32 lba) {
    if (!discRead(d, lba, hdrBuf)) { printf("Read error\n"); return false; }
    ExeInfo x;
    if (!checkHeader(hdrBuf, &x)) return false;

    u32 remaining = x.size, pos = x.dest, cur = lba + 1;
    while (remaining) {
        if (!discRead(d, cur, secBuf)) { printf("Read error\n"); return false; }
        u32 n = remaining < 2048 ? remaining : 2048;
        memcpy(psxRam + pos, secBuf, n);
        pos += n; remaining -= n; cur++;
    }
    showInfo(&x);
    return true;
}

static void loadDisc(const char *path) {
    consoleClear();
    printf("Disc:\n%.60s\n\n", path);

    static char binPath[768];
    const char *img = path;
    if (hasExt(path, ".cue")) {
        if (!cueToBin(path, binPath, sizeof(binPath))) {
            printf("No FILE entry in cue\n"); waitB(); return;
        }
        img = binPath;
    }

    Disc d;
    if (!discOpen(&d, img)) {
        printf("Cannot read disc image\n(need a PS1 .bin/.iso)\n");
        waitB(); return;
    }

    if (!discRead(&d, 16, secBuf)) { printf("Read error\n"); fclose(d.f); waitB(); return; }
    u32 rootLba  = rd32(secBuf + 156 + 2);
    u32 rootSize = rd32(secBuf + 156 + 10);

    char boot[40];
    boot[0] = 0;
    u32 l, s;
    if (findInDir(&d, rootLba, rootSize, "SYSTEM.CNF", &l, &s) &&
        discRead(&d, l, cnfBuf)) {
        cnfBuf[2047] = 0;
        parseBoot((const char *)cnfBuf, boot, sizeof(boot));
    }
    if (!boot[0]) strcpy(boot, "PSX.EXE");

    if (!findInDir(&d, rootLba, rootSize, boot, &l, &s)) {
        printf("Boot file not found:\n%s\n", boot);
        fclose(d.f); waitB(); return;
    }
    printf("Boot: %s\n\n", boot);
    exeFromDisc(&d, l);
    fclose(d.f);
    waitB();
}

// ---------------------------------------------------------------- main

int main(void) {
    consoleDemoInit();

    if (!fatInitDefault()) {
        printf("SD init failed.\nRun in DSi mode or\npatch with DLDI.\n");
        while (1) swiWaitForVBlank();
    }

    scanDir();
    int sel = 0, top = 0;
    draw(sel, top);

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        u32 k = keysDown() | keysDownRepeat();
        bool redraw = false;

        if ((k & KEY_DOWN) && sel < count - 1) { sel++; redraw = true; }
        if ((k & KEY_UP)   && sel > 0)         { sel--; redraw = true; }
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;

        if ((keysDown() & KEY_A) && count > 0) {
            Entry *e = &entries[sel];
            if (e->isDir) {
                if (strcmp(e->name, "..") == 0) goUp();
                else {
                    if (strcmp(cwd, "/") != 0) strcat(cwd, "/");
                    strcat(cwd, e->name);
                }
                scanDir(); sel = top = 0;
            } else {
                snprintf(pathBuf, sizeof(pathBuf), "%s%s%s", cwd,
                         strcmp(cwd, "/") ? "/" : "", e->name);
                if (hasExt(e->name, ".cue") || hasExt(e->name, ".bin") ||
                    hasExt(e->name, ".iso"))
                    loadDisc(pathBuf);
                else
                    loadExe(pathBuf);
            }
            redraw = true;
        }
        if (keysDown() & KEY_B) {
            goUp(); scanDir(); sel = top = 0; redraw = true;
        }
        if (redraw) draw(sel, top);
    }
    return 0;
}
