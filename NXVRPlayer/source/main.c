// NX VR Player — видеоплеер для Nintendo Switch (homebrew, libnx)
// Обычное видео, стерео-3D (SBS), VR180, VR360 с гироскопом и режимом
// для картонных VR-очков (Nintendo Labo VR и аналоги).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "common.h"
#include "browser.h"
#include "player.h"
#include "render.h"
#include "gyro.h"

typedef struct {
    ViewMode mode;
    float    fov_flat;   // угол обзора в режиме «окно»
    float    fov_vr;     // угол обзора в очках
    float    scale3d;    // размер экрана в режиме 3D SBS
    float    ipd;
    bool     distort;
} Settings;

static Settings s_set = { MODE_FLAT, 75.0f, 95.0f, 0.80f, 0.0f, true };

// Возвращает false, если пользователь нажал «+» (выход из программы)
static bool play_file(PadState *pad, const char *path, char *status, size_t status_len) {
    char err[160] = {0};
    Player *p = player_open(path, err, sizeof err);
    if (!p) {
        snprintf(status, status_len, "Error: %s", err);
        return true;
    }
    if (!render_init(nwindowGetDefault())) {
        snprintf(status, status_len, "Error: OpenGL init failed");
        player_close(p);
        return true;
    }

    appletSetMediaPlaybackState(true); // не гасить экран во время просмотра
    s_set.mode = mode_guess(path);
    gyro_recenter();

    bool keep_running = true;
    double last = now_sec();
    double osd_until = last + 4.0;
    double msg_until = 0;
    char msg[48] = "";
    float rot[9];

    #define SHOW_MSG(...) do { snprintf(msg, sizeof msg, __VA_ARGS__); msg_until = now_sec() + 2.0; } while (0)

    while (appletMainLoop()) {
        double t = now_sec();
        float dt = (float)(t - last);
        last = t;

        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        u64 held = padGetButtons(pad);
        if (down) osd_until = t + 3.0;

        if (down & HidNpadButton_Plus) { keep_running = false; break; }
        if (down & HidNpadButton_B) break;

        if (down & HidNpadButton_A) player_set_pause(p, !player_paused(p));

        double pos = player_clock(p);
        double dur = player_duration(p);
        double seek = 0;
        if (down & HidNpadButton_Left)  seek = -10;
        if (down & HidNpadButton_Right) seek = +10;
        if (down & HidNpadButton_L)     seek = -60;
        if (down & HidNpadButton_R)     seek = +60;
        if (seek != 0 && dur > 0) player_seek(p, pos + seek);

        if (down & HidNpadButton_Y) {
            s_set.mode = (ViewMode)((s_set.mode + 1) % MODE_COUNT);
            gyro_recenter();
        }
        if (down & HidNpadButton_X) { gyro_recenter(); SHOW_MSG("CENTER"); }
        if (down & HidNpadButton_Minus) {
            s_set.distort = !s_set.distort;
            SHOW_MSG("LENS %s", s_set.distort ? "ON" : "OFF");
        }

        bool headset = mode_is_headset(s_set.mode);
        // вверх/вниз: в очках — межзрачковое расстояние, без очков — ничего
        if (headset && (down & HidNpadButton_Up))   { s_set.ipd += 0.01f; SHOW_MSG("IPD %+.2f", s_set.ipd); }
        if (headset && (down & HidNpadButton_Down)) { s_set.ipd -= 0.01f; SHOW_MSG("IPD %+.2f", s_set.ipd); }

        // ZL/ZR (удерживать): угол обзора / размер экрана
        float zoom = 0;
        if (held & HidNpadButton_ZL) zoom = -1;
        if (held & HidNpadButton_ZR) zoom = +1;
        if (zoom != 0) {
            if (s_set.mode == MODE_SBS_3D) {
                s_set.scale3d += zoom * dt * 0.4f;
                if (s_set.scale3d < 0.3f) s_set.scale3d = 0.3f;
                if (s_set.scale3d > 1.2f) s_set.scale3d = 1.2f;
                SHOW_MSG("SIZE %d%%", (int)(s_set.scale3d * 100));
            } else if (mode_is_sphere(s_set.mode)) {
                float *fov = headset ? &s_set.fov_vr : &s_set.fov_flat;
                *fov += zoom * dt * 30.0f;
                if (*fov < 30)  *fov = 30;
                if (*fov > 130) *fov = 130;
                SHOW_MSG("FOV %d", (int)*fov);
            }
            osd_until = t + 2.0;
        }

        gyro_update(pad, dt);
        gyro_matrix(rot);

        AVFrame *f = player_take_frame(p, pos);
        if (f) {
            render_upload(f);
            av_frame_free(&f);
        }

        RenderParams rp = {
            .mode = s_set.mode,
            .rot = rot,
            .fov_deg = headset ? s_set.fov_vr : s_set.fov_flat,
            .screen_scale = s_set.scale3d,
            .distort = s_set.distort,
            .ipd = s_set.ipd,
            .osd = t < osd_until || player_paused(p),
            .pos = pos,
            .dur = dur,
            .paused = player_paused(p),
            .msg = t < msg_until ? msg : NULL,
        };
        render_draw(&rp);

        if (player_finished(p)) break; // конец файла — назад к списку
    }

    appletSetMediaPlaybackState(false);
    render_exit();
    player_close(p);
    return keep_running;
}

int main(int argc, char **argv) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    gyro_init();

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        // без звука всё равно можно смотреть
    }

    char cwd[768] = "sdmc:/videos/";
    struct stat st;
    if (stat("sdmc:/videos", &st) != 0 || !S_ISDIR(st.st_mode)) strcpy(cwd, "sdmc:/");

    char path[1024];
    char status[160] = "";

    while (appletMainLoop()) {
        consoleInit(NULL);
        bool picked = browser_run(&pad, cwd, sizeof cwd, path, sizeof path, status);
        consoleExit(NULL);
        if (!picked) break;
        status[0] = 0;
        if (!play_file(&pad, path, status, sizeof status)) break;
    }

    SDL_Quit();
    gyro_exit();
    return 0;
}
