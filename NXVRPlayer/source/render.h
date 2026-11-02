#pragma once
#include <libavutil/frame.h>
#include "common.h"

typedef struct {
    ViewMode    mode;
    const float *rot;        // 3x3 поворот камеры (column-major)
    float       fov_deg;     // вертикальный угол обзора для 360/180
    float       screen_scale;// размер виртуального экрана в режиме 3D SBS (0..1)
    bool        distort;     // коррекция линз очков
    bool        single;      // один экран без разделения (без очков)
    float       ipd;         // сдвиг изображений глаз (межзрачковое расстояние)
    bool        osd;         // показывать панель
    double      pos, dur;
    bool        paused;
    const char *msg;         // короткое сообщение (или NULL)
    const char *info;        // служебная строка (или NULL)
} RenderParams;

bool render_init(NWindow *win);
void render_exit(void);
void render_upload(const AVFrame *f);
void render_draw(const RenderParams *rp);
const char *render_status(void); // состояние шейдера и ошибки GL для панели
void render_reset_video(void);   // забыть кадр предыдущего видео

// Экран интерфейса 1280x720 (координаты от левого верхнего угла, цвет 0xRRGGBB)
void ui_clear(uint32_t rgb);
void ui_rect(int x, int y, int w, int h, uint32_t rgb);
void ui_text(int x, int y, int px, const char *s, int max_chars, uint32_t rgb); // max_chars<=0 — без ограничения
int  ui_text_width(const char *s, int px);
void ui_present(void);           // вывести на экран
