#pragma once
#include <libavutil/frame.h>
#include "common.h"

typedef struct Player Player;

Player  *player_open(const char *path, char *err, size_t errlen);
void     player_close(Player *p);

double   player_clock(Player *p);                    // текущее время воспроизведения, сек
AVFrame *player_take_frame(Player *p, double clock); // кадр для показа (или NULL); освободить av_frame_free
void     player_set_pause(Player *p, bool pause);
bool     player_paused(Player *p);
void     player_seek(Player *p, double target_sec);  // абсолютная позиция
double   player_duration(Player *p);
bool     player_finished(Player *p);
bool     player_has_audio(Player *p);
