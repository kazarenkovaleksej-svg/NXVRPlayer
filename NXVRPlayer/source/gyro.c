// Отслеживание поворота головы по гироскопу Joy-Con / Pro Controller.
// Используем поле `angle` датчика (накопленный угол, 1.0 = один оборот):
// берём приращения за кадр и поворачиваем кватернион камеры в её локальной системе.
#include <math.h>
#include <string.h>
#include "gyro.h"

// ---- Настройка осей ----------------------------------------------------------
// Если в очках картинка поворачивается «не туда» (например, наклоняется вместо
// поворота или крутится в обратную сторону) — поменяйте индекс оси (0=x,1=y,2=z)
// или знак (+1/-1) здесь и пересоберите.
#define PITCH_AXIS 0   // кивок вверх/вниз
#define PITCH_SIGN (+1.0f)
#define YAW_AXIS   2   // поворот влево/вправо
#define YAW_SIGN   (+1.0f)
#define ROLL_AXIS  1   // наклон головы к плечу
#define ROLL_SIGN  (-1.0f)
// -----------------------------------------------------------------------------

#define TWO_PI 6.28318530718f

static HidSixAxisSensorHandle s_h[4]; // 0: handheld, 1: Pro, 2-3: Joy-Con пара (L, R)
static float s_q[4];                   // ориентация камеры (w, x, y, z)
static float s_prev[3];
static bool  s_have_prev;
static float s_stick_yaw, s_stick_pitch;

static void q_identity(float *q) { q[0] = 1; q[1] = q[2] = q[3] = 0; }

static void q_mul(float *r, const float *a, const float *b) {
    float w = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
    float x = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
    float y = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
    float z = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
    r[0] = w; r[1] = x; r[2] = y; r[3] = z;
}

static void q_normalize(float *q) {
    float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 0) for (int i = 0; i < 4; i++) q[i] /= n;
}

void gyro_init(void) {
    hidGetSixAxisSensorHandles(&s_h[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&s_h[1], 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&s_h[2], 2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
    for (int i = 0; i < 4; i++) hidStartSixAxisSensor(s_h[i]);
    gyro_recenter();
}

void gyro_exit(void) {
    for (int i = 0; i < 4; i++) hidStopSixAxisSensor(s_h[i]);
}

void gyro_recenter(void) {
    q_identity(s_q);
    s_have_prev = false;
    s_stick_yaw = s_stick_pitch = 0;
}

void gyro_update(PadState *pad, float dt) {
    HidSixAxisSensorState st = {0};
    u32 style = padGetStyleSet(pad);
    size_t n = 0;
    if (style & HidNpadStyleTag_NpadHandheld)      n = hidGetSixAxisSensorStates(s_h[0], &st, 1);
    else if (style & HidNpadStyleTag_NpadFullKey)  n = hidGetSixAxisSensorStates(s_h[1], &st, 1);
    else if (style & HidNpadStyleTag_NpadJoyDual)  n = hidGetSixAxisSensorStates(s_h[3], &st, 1); // правый Joy-Con

    if (n > 0 && (st.attributes & HidSixAxisSensorAttribute_IsConnected)) {
        float cur[3] = { st.angle.x, st.angle.y, st.angle.z };
        if (s_have_prev) {
            float d[3];
            for (int i = 0; i < 3; i++) {
                d[i] = cur[i] - s_prev[i];
                d[i] -= roundf(d[i]);          // защита от перехода через целый оборот
                d[i] *= TWO_PI;                // в радианы
            }
            // вектор малого поворота в системе камеры (x вправо, y вверх, z назад)
            float v[3] = { PITCH_SIGN * d[PITCH_AXIS], YAW_SIGN * d[YAW_AXIS], ROLL_SIGN * d[ROLL_AXIS] };
            float ang = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (ang > 1e-6f && ang < 1.0f) {
                float s = sinf(ang * 0.5f) / ang;
                float dq[4] = { cosf(ang * 0.5f), v[0]*s, v[1]*s, v[2]*s };
                float r[4];
                q_mul(r, s_q, dq);
                memcpy(s_q, r, sizeof(r));
                q_normalize(s_q);
            }
        }
        memcpy(s_prev, cur, sizeof(cur));
        s_have_prev = true;
    }

    // правый стик — ручной обзор (удобно без очков или если гироскоп недоступен)
    HidAnalogStickState rs = padGetStickPos(pad, 1);
    float sx = rs.x / 32767.0f, sy = rs.y / 32767.0f;
    if (fabsf(sx) < 0.15f) sx = 0;
    if (fabsf(sy) < 0.15f) sy = 0;
    s_stick_yaw   -= sx * dt * 1.8f;
    s_stick_pitch += sy * dt * 1.4f;
    if (s_stick_pitch >  1.5f) s_stick_pitch =  1.5f;
    if (s_stick_pitch < -1.5f) s_stick_pitch = -1.5f;
}

// m — матрица 3x3 (по столбцам, как в GLSL): камера -> мир.
void gyro_matrix(float *m) {
    float w = s_q[0], x = s_q[1], y = s_q[2], z = s_q[3];
    float g[9] = { // столбцы
        1 - 2*(y*y + z*z), 2*(x*y + w*z),     2*(x*z - w*y),
        2*(x*y - w*z),     1 - 2*(x*x + z*z), 2*(y*z + w*x),
        2*(x*z + w*y),     2*(y*z - w*x),     1 - 2*(x*x + y*y),
    };
    // стик: сначала поворот по горизонту (мировая ось Y), затем кивок
    float cy = cosf(s_stick_yaw),   sy = sinf(s_stick_yaw);
    float cp = cosf(s_stick_pitch), sp = sinf(s_stick_pitch);
    float s[9] = { // Ry * Rx, по столбцам
        cy,      0,  -sy,
        sy*sp,   cp,  cy*sp,
        sy*cp,  -sp,  cy*cp,
    };
    // m = s * g
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            m[c*3 + r] = s[0*3 + r] * g[c*3 + 0] + s[1*3 + r] * g[c*3 + 1] + s[2*3 + r] * g[c*3 + 2];
}
