// PSXEmulDS - Phase 1: SD file browser + PS-X EXE loader (no sound)
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ENTRIES 256
#define VISIBLE     20
#define PSX_RAM_SIZE (2 * 1024 * 1024)
#define EXE_HEADER   0x800

typedef struct {
    char name[256];
    bool isDir;
} Entry;

static Entry entries[MAX_ENTRIES];
static int   count;
static char  cwd[512] = "/";
static u8   *psxRam = NULL;

static bool wanted(const char *n) {
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".exe") || !strcasecmp(dot, ".psx") ||
           !strcasecmp(dot, ".bin") || !strcasecmp(dot, ".cue") ||
           !strcasecmp(dot, ".iso");
}

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

static void waitB(void) {
    printf("\nPress B to go back");
    while (1) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B) break;
    }
}

static u32 rd32(const u8 *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void loadExe(const char *path) {
    consoleClear();
    printf("Loading:\n%s\n\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { printf("Cannot open file\n"); waitB(); return; }

    u8 hdr[EXE_HEADER];
    if (fread(hdr, 1, EXE_HEADER, f) != EXE_HEADER ||
        memcmp(hdr, "PS-X EXE", 8) != 0) {
        printf("Not a PS-X EXE file\n(.bin/.cue/.iso not\nsupported yet)\n");
        fclose(f); waitB(); return;
    }

    u32 pc   = rd32(hdr + 0x10);
    u32 gp   = rd32(hdr + 0x14);
    u32 dest = rd32(hdr + 0x18) & 0x1FFFFFFF;
    u32 size = rd32(hdr + 0x1C);
    u32 sp   = rd32(hdr + 0x30);

    if (!psxRam) psxRam = (u8 *)malloc(PSX_RAM_SIZE);
    if (!psxRam) { printf("Out of memory\n"); fclose(f); waitB(); return; }

    if (dest + size > PSX_RAM_SIZE) {
        printf("EXE does not fit in RAM\n");
        fclose(f); waitB(); return;
    }

    memset(psxRam, 0, PSX_RAM_SIZE);
    size_t got = fread(psxRam + dest, 1, size, f);
    fclose(f);

    printf("PC   %08lX\nGP   %08lX\nSP   %08lX\n", pc, gp, sp);
    printf("Dest %08lX\nSize %lu bytes\nRead %u bytes\n",
           dest, size, (unsigned)got);
    printf("\nLoaded OK.\nCPU not implemented yet\n(phase 2)\n");
    waitB();
}

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
                char path[768];
                snprintf(path, sizeof(path), "%s%s%s", cwd,
                         strcmp(cwd, "/") ? "/" : "", e->name);
                loadExe(path);
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
