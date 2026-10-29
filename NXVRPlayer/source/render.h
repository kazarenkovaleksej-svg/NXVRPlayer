#pragma once
#include <libavutil/frame.h>
#include "common.h"

typedef struct {
    ViewMode    mode;
    const float *rot;        // 3x3 поворот камеры (column-major)
    float       fov_deg;     // вертикальный угол обзора для 360/180
    float       screen_scale;// размер виртуального экрана в режиме 3D SBS (0..1)
    bool        distort;     // коррекция линз очков
    float       ipd;         // сдвиг изображений глаз (межзрачковое расстояние)
    bool        osd;         // показывать панель
    double      pos, dur;
    bool        paused;
    const char *msg;         // короткое сообщение (или NULL)
} RenderParams;

bool render_init(NWindow *win);
void render_exit(void);
void render_upload(const AVFrame *f);
void render_draw(const RenderParams *rp);
