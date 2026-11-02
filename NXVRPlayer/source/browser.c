// Файловый браузер. Рисуется через видеокарту (тем же способом, что и видео),
// поэтому переход «список -> видео -> список» не переключает режимы экрана.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "browser.h"
#include "render.h"

#define MAX_ENTRIES 2048
#define PX          3                     // размер «пикселя» шрифта
#define CHAR_W      (4 * PX)
#define LINE_H      (8 * PX)
#define LIST_TOP    (LINE_H * 3 + 8)
#define LIST_ROWS   24
#define MAX_CHARS   100

#define C_BG     0x101218
#define C_TITLE  0x4FC3F7
#define C_PATH   0xFFD54F
#define C_TEXT   0xE0E0E0
#define C_DIR    0x90CAF9
#define C_SEL_BG 0x2E5C8A
#define C_DIM    0x888888
#define C_ERR    0xFF6E6E

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
    ui_clear(C_BG);
    ui_text(20, 12, PX, "NX VR PLAYER  -  VIDEO / 3D / VR180 / VR360", 0, C_TITLE);
    ui_text(20, 12 + LINE_H, PX, cwd, MAX_CHARS, C_PATH);
    ui_rect(20, LIST_TOP - 6, 1240, 2, 0x333844);

    if (s_count == 0)
        ui_text(40, LIST_TOP + 10, PX, "(NO VIDEO FILES HERE - PRESS B TO GO UP)", 0, C_DIM);

    for (int i = top; i < s_count && i < top + LIST_ROWS; i++) {
        const Entry *e = &s_entries[i];
        int y = LIST_TOP + (i - top) * LINE_H;
        if (i == sel) ui_rect(16, y - 5, 1248, LINE_H, C_SEL_BG);
        if (e->dir) ui_text(24, y, PX, "[D]", 0, C_DIR);
        ui_text(24 + CHAR_W * 4, y, PX, e->name, MAX_CHARS - 6, e->dir ? C_DIR : C_TEXT);
    }

    // полоса прокрутки
    if (s_count > LIST_ROWS) {
        int track = LIST_ROWS * LINE_H;
        int bar = track * LIST_ROWS / s_count;
        if (bar < 12) bar = 12;
        int pos = (track - bar) * top / (s_count - LIST_ROWS);
        ui_rect(1268, LIST_TOP - 5, 4, track, 0x2A2E38);
        ui_rect(1268, LIST_TOP - 5 + pos, 4, bar, 0x6A7080);
    }

    int by = 720 - LINE_H * 2 - 8;
    ui_rect(20, by - 8, 1240, 2, 0x333844);
    char line[96];
    snprintf(line, sizeof line, "A: OPEN   B: UP   L/R: PAGE   +: EXIT        %d ITEM(S)", s_count);
    ui_text(20, by, PX, line, 0, C_DIM);
    if (status && status[0]) ui_text(20, by + LINE_H, PX, status, MAX_CHARS, C_ERR);
    ui_present();
}

bool browser_run(PadState *pad, char *cwd, size_t cwd_len, char *out, size_t out_len, char *status) {
    scan_dir(cwd);
    static int sel = 0, top = 0;     // запоминаем позицию в списке между просмотрами
    static char last_dir[768];
    if (strcmp(last_dir, cwd) != 0) { sel = top = 0; snprintf(last_dir, sizeof last_dir, "%s", cwd); }
    if (sel >= s_count) sel = s_count ? s_count - 1 : 0;
    int hold = 0;
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
            snprintf(last_dir, sizeof last_dir, "%s", cwd);
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
                snprintf(last_dir, sizeof last_dir, "%s", cwd);
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
        else ui_present();   // перерисовка той же картинки (держит ритм 60 кадров/с)
    }
    return false;
}
