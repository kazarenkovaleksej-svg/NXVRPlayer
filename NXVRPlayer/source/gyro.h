#pragma once
#include "common.h"

void gyro_init(void);
void gyro_exit(void);
void gyro_recenter(void);                 // «смотреть прямо» = текущее положение
void gyro_update(PadState *pad, float dt); // вызывать каждый кадр
void gyro_matrix(float *m);               // 3x3, column-major: камера -> мир
