#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <switch.h>

// Режимы отображения
typedef enum {
    MODE_FLAT = 0,     // обычное 2D-видео на весь экран
    MODE_SBS_3D,       // стерео-3D фильм side-by-side (виртуальный экран в очках)
    MODE_VR180_SBS,    // VR180, стерео side-by-side
    MODE_VR360_MONO,   // VR360, моно (в очках)
    MODE_VR360_TB,     // VR360, стерео top-bottom (в очках)
    MODE_360_WINDOW,   // 360° без очков: «окно» на весь экран, крутить консолью/стиком
    MODE_COUNT
} ViewMode;

const char *mode_name(ViewMode m);
bool        mode_is_headset(ViewMode m);   // режим для очков (экран делится пополам)
bool        mode_is_sphere(ViewMode m);    // сферическая проекция (нужен гироскоп)
ViewMode    mode_guess(const char *path);  // угадать режим по имени файла

static inline double now_sec(void) {
    return (double)armTicksToNs(armGetSystemTick()) / 1e9;
}
