// Простой файловый браузер в текстовой консоли libnx.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "browser.h"

#define MAX_ENTRIES 2048
#define LIST_ROWS   36

typedef struct { char name[256]; bool dir; } Entry;
static Entry s_entries[MAX_ENTRIES];
static int   s_count;

static bool is_video(const char *n) {
    static const char *exts[] = { ".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".ts",
                                  ".m2ts", ".flv", ".wmv", ".mpg", ".mpeg", ".3gp", NULL };
    const char *ext = strrchr(n, '.');
    if (!ext) return false;
    for (int i = 0; exts[i]; i++) if (strcasecmp(ext, exts[i]) == 0) return true;
    return false;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan_dir(const char *cwd) {
    s_count = 0;
    DIR *d = opendir(cwd);
    if (!d) return;
    struct dirent *e;
    char full[1024];
    while ((e = readdir(d)) && s_count < MAX_ENTRIES) {
        if (e->d_name[0] == '.') continue;
        snprintf(full, sizeof(full), "%s%s", cwd, e->d_name);
        struct stat st;
        bool dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        if (!dir && !is_video(e->d_name)) continue;
        snprintf(s_entries[s_count].name, sizeof(s_entries[0].name), "%s", e->d_name);
        s_entries[s_count].dir = dir;
        s_count++;
    }
    closedir(d);
    qsort(s_entries, s_count, sizeof(Entry), entry_cmp);
}

// "sdmc:/a/b/" -> "sdmc:/a/"
static void go_parent(char *cwd) {
    size_t len = strlen(cwd);
    if (len < 2) return;
    char *root = strstr(cwd, ":/");
    if (root && (size_t)(root - cwd) + 2 >= len) return; // уже корень
    cwd[len - 1] = 0;
    char *slash = strrchr(cwd, '/');
    if (slash) slash[1] = 0;
}

static void draw(const char *cwd, int sel, int top, const char *status) {
    printf("\x1b[2J\x1b[1;1H");
    printf("\x1b[36m NX VR Player\x1b[0m  -  video / 3D / VR180 / VR360\n");
    printf(" \x1b[33m%.76s\x1b[0m\n", cwd);
    printf("--------------------------------------------------------------------------------");
    if (s_count == 0) printf("\n  (no video files here)\n");
    for (int i = top; i < s_count && i < top + LIST_ROWS; i++) {
        const Entry *e = &s_entries[i];
        printf("\x1b[%d;1H", 4 + (i - top));
        if (i == sel) printf("\x1b[47;30m");
        printf(" %s %-74.74s", e->dir ? "[D]" : "   ", e->name);
        printf("\x1b[0m");
    }
    printf("\x1b[41;1H--------------------------------------------------------------------------------");
    printf("\x1b[42;1H A: open   B: up   L/R: page   +: exit");
    printf("\x1b[43;1H Put videos in sdmc:/videos/   %d item(s)", s_count);
    if (status && status[0]) printf("\x1b[44;1H \x1b[31m%.78s\x1b[0m", status);
    consoleUpdate(NULL);
}

bool browser_run(PadState *pad, char *cwd, size_t cwd_len, char *out, size_t out_len, char *status) {
    scan_dir(cwd);
    int sel = 0, top = 0, hold = 0;
    bool dirty = true;

    while (appletMainLoop()) {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        u64 held = padGetButtons(pad);

        // автоповтор при удержании вверх/вниз
        int move = 0;
        if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) { move = 1; hold = 0; }
        else if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) { move = -1; hold = 0; }
        else if (held & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_Up | HidNpadButton_StickLUp)) {
            if (++hold > 20 && (hold % 4) == 0)
                move = (held & (HidNpadButton_Down | HidNpadButton_StickLDown)) ? 1 : -1;
        } else hold = 0;
        if (down & HidNpadButton_R) move = LIST_ROWS;
        if (down & HidNpadButton_L) move = -LIST_ROWS;

        if (move && s_count) {
            sel += move;
            if (sel < 0) sel = 0;
            if (sel >= s_count) sel = s_count - 1;
            dirty = true;
        }

        if (down & HidNpadButton_Plus) return false;

        if (down & HidNpadButton_B) {
            go_parent(cwd);
            scan_dir(cwd);
            sel = top = 0;
            status[0] = 0;
            dirty = true;
        }

        if ((down & HidNpadButton_A) && s_count) {
            Entry *e = &s_entries[sel];
            if (e->dir) {
                size_t len = strlen(cwd);
                snprintf(cwd + len, cwd_len - len, "%s/", e->name);
                scan_dir(cwd);
                sel = top = 0;
                status[0] = 0;
                dirty = true;
            } else {
                snprintf(out, out_len, "%s%s", cwd, e->name);
                return true;
            }
        }

        if (sel < top) top = sel;
        if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;

        if (dirty) { draw(cwd, sel, top, status); dirty = false; }
        else consoleUpdate(NULL);
    }
    return false;
}
