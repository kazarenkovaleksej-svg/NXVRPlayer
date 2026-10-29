#include <string.h>
#include <ctype.h>
#include "common.h"

const char *mode_name(ViewMode m) {
    switch (m) {
        case MODE_FLAT:       return "2D";
        case MODE_SBS_3D:     return "3D SBS";
        case MODE_VR180_SBS:  return "VR180 SBS";
        case MODE_VR360_MONO: return "VR360 MONO";
        case MODE_VR360_TB:   return "VR360 TB";
        case MODE_360_WINDOW: return "360 WINDOW";
        default:              return "?";
    }
}

bool mode_is_headset(ViewMode m) {
    return m == MODE_SBS_3D || m == MODE_VR180_SBS || m == MODE_VR360_MONO || m == MODE_VR360_TB;
}

bool mode_is_sphere(ViewMode m) {
    return m == MODE_VR180_SBS || m == MODE_VR360_MONO || m == MODE_VR360_TB || m == MODE_360_WINDOW;
}

// Ищем подсказки в имени файла: video_180_sbs.mp4, clip_360_tb.mkv, movie.3d.sbs.mkv ...
ViewMode mode_guess(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char low[256];
    size_t i = 0;
    for (; name[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;

    bool has180 = strstr(low, "180") != NULL;
    bool has360 = strstr(low, "360") != NULL;
    bool tb     = strstr(low, "_tb") || strstr(low, ".tb") || strstr(low, "-tb") ||
                  strstr(low, "_ou") || strstr(low, ".ou") || strstr(low, "-ou") ||
strstr(low, "topbottom") || strstr(low, "top_bottom");
    bool sbs    = strstr(low, "sbs") || strstr(low, "_lr") || strstr(low, "3dh") || strstr(low, "side");

    if (has180) return MODE_VR180_SBS;
    if (has360) return tb ? MODE_VR360_TB : MODE_VR360_MONO;
    if (sbs || strstr(low, "3d")) return MODE_SBS_3D;
    return MODE_FLAT;
}
