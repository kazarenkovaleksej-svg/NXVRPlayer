// Отслеживание поворота по гироскопу Joy-Con / Pro Controller.
//
// Ориентация хранится как два угла: поворот влево/вправо вокруг вертикали (yaw)
// и наклон вверх/вниз (pitch). Крена нет — горизонт всегда ровный, поэтому
// картинка не «заваливается» даже после многих поворотов.
//
// - yaw: приращение поворота консоли, пересчитанное на вертикальную ось мира;
// - pitch: гироскоп + плавная поправка по акселерометру (направлению силы
//   тяжести), так что наклон вверх/вниз со временем не уплывает.
// Уход по горизонтали (yaw) без компаса полностью убрать нельзя — для этого
// есть кнопка X («смотреть прямо»).
#include <math.h>
#include <string.h>
#include "gyro.h"

// ---- Настройка осей датчика ---------------------------------------------------
#define PITCH_AXIS 0       // кивок вверх/вниз
#define PITCH_SIGN (+1.0f)
#define YAW_AXIS   1       // поворот влево/вправо
#define YAW_SIGN   (+1.0f) // +1: поворот консоли вправо — взгляд вправо
// -----------------------------------------------------------------------------

#define TWO_PI      6.28318530718f
#define PITCH_LIMIT 1.50f   // ~86°
#define ACC_TAU     1.0f    // за сколько секунд акселерометр подтягивает наклон

static HidSixAxisSensorHandle s_h[4]; // 0: handheld, 1: Pro, 2-3: Joy-Con пара (L, R)
static float s_prev[3];
static bool  s_have_prev;

static float s_yaw, s_pitch;          // от гироскопа (рад)
static float s_stick_yaw, s_stick_pitch;
static float s_yaw_user = 1.0f;       // -1: инвертировать поворот (нажатие левого стика)

// акселерометр
static float s_up[3];                 // сглаженное направление «вверх» в осях датчика
static bool  s_have_up;
static float s_ref_ang;               // угол «вверх» в плоскости кивка в момент сброса
static bool  s_have_ref;
static float s_prev_acc_pitch;
static float s_corr;                  // согласование знака гироскопа и акселерометра

// автоподстройка смещения гироскопа, пока консоль неподвижна
static float s_bias[3];               // рад/с
static float s_still;                 // сколько секунд консоль неподвижна

bool gyro_toggle_yaw_invert(void) { s_yaw_user = -s_yaw_user; return s_yaw_user < 0; }

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
    s_yaw = s_pitch = 0;
    s_stick_yaw = s_stick_pitch = 0;
    s_have_prev = false;
    s_have_ref = false;   // текущий наклон консоли станет «прямо»
}

static float wrap_pi(float a) {
    while (a >  (float)M_PI) a -= TWO_PI;
    while (a < -(float)M_PI) a += TWO_PI;
    return a;
}

void gyro_update(PadState *pad, float dt) {
    HidSixAxisSensorState st = {0};
    u32 style = padGetStyleSet(pad);
    size_t n = 0;
    if (style & HidNpadStyleTag_NpadHandheld)      n = hidGetSixAxisSensorStates(s_h[0], &st, 1);
    else if (style & HidNpadStyleTag_NpadFullKey)  n = hidGetSixAxisSensorStates(s_h[1], &st, 1);
    else if (style & HidNpadStyleTag_NpadJoyDual)  n = hidGetSixAxisSensorStates(s_h[3], &st, 1); // правый Joy-Con

    if (n > 0 && (st.attributes & HidSixAxisSensorAttribute_IsConnected)) {
        // --- гироскоп ---
        float cur[3] = { st.angle.x, st.angle.y, st.angle.z };
        float dpitch = 0;
        if (s_have_prev) {
            float d[3];
            for (int i = 0; i < 3; i++) {
                d[i] = cur[i] - s_prev[i];
                d[i] -= roundf(d[i]);      // переход через целый оборот
                d[i] *= TWO_PI;            // в радианы
            }
            if (fabsf(d[0]) < 1.0f && fabsf(d[1]) < 1.0f && fabsf(d[2]) < 1.0f && dt > 0) {
                // неподвижна, если все скорости (за вычетом смещения) меньше ~3°/с
                bool still = true;
                for (int i = 0; i < 3; i++)
                    if (fabsf(d[i] / dt - s_bias[i]) > 0.05f) still = false;
                s_still = still ? s_still + dt : 0;
                if (s_still > 0.5f) {
                    float k = 1.0f - expf(-dt / 3.0f);
                    for (int i = 0; i < 3; i++) s_bias[i] += (d[i] / dt - s_bias[i]) * k;
                }
                for (int i = 0; i < 3; i++) d[i] -= s_bias[i] * dt;

                dpitch = PITCH_SIGN * d[PITCH_AXIS];
                // поворот вокруг вертикали мира: при наклонённой консоли датчик
                // видит только cos(pitch) от него — компенсируем
                float c = cosf(s_pitch);
                if (c < 0.3f) c = 0.3f;
                s_yaw   += s_yaw_user * YAW_SIGN * d[YAW_AXIS] / c;
                s_pitch += dpitch;
            }
        }
        memcpy(s_prev, cur, sizeof(cur));
        s_have_prev = true;

        // --- акселерометр: поправка наклона ---
        float ax = st.acceleration.x, ay = st.acceleration.y, az = st.acceleration.z;
        float g = sqrtf(ax*ax + ay*ay + az*az);
        if (g > 0.85f && g < 1.15f) {             // консоль не трясут
            float a[3] = { ax / g, ay / g, az / g };
            if (!s_have_up) { memcpy(s_up, a, sizeof a); s_have_up = true; }
            for (int i = 0; i < 3; i++) s_up[i] += (a[i] - s_up[i]) * 0.2f;

            // угол «вверх» в плоскости, перпендикулярной оси кивка (оси Y и Z датчика)
            float ang = atan2f(s_up[2], s_up[1]);
            if (!s_have_ref) {
                s_ref_ang = ang;
                s_prev_acc_pitch = 0;
                s_have_ref = true;
            } else {
                float acc_pitch = wrap_pi(ang - s_ref_ang);
                // определяем, в какую сторону акселерометр считает наклон
                // относительно гироскопа (по совпадению движений)
                float dacc = wrap_pi(acc_pitch - s_prev_acc_pitch);
                if (fabsf(dacc) < 0.3f) s_corr += dpitch * dacc;
                s_prev_acc_pitch = acc_pitch;

                if (fabsf(s_corr) > 0.01f) {      // знак уже понятен
                    float target = (s_corr > 0 ? 1.0f : -1.0f) * acc_pitch;
                    float k = 1.0f - expf(-dt / ACC_TAU);
                    s_pitch += (target - s_pitch) * k;
                }
            }
        }
    }

    if (s_pitch >  PITCH_LIMIT) s_pitch =  PITCH_LIMIT;
    if (s_pitch < -PITCH_LIMIT) s_pitch = -PITCH_LIMIT;
    s_yaw = wrap_pi(s_yaw);

    // правый стик — ручной обзор
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
// Положительный yaw — взгляд влево, положительный pitch — вверх.
void gyro_matrix(float *m) {
    float yaw   = s_yaw + s_stick_yaw;
    float pitch = s_pitch + s_stick_pitch;
    if (pitch >  PITCH_LIMIT) pitch =  PITCH_LIMIT;
    if (pitch < -PITCH_LIMIT) pitch = -PITCH_LIMIT;
    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float r[9] = { // Ry(yaw) * Rx(pitch), по столбцам
        cy,      0,  -sy,
        sy*sp,   cp,  cy*sp,
        sy*cp,  -sp,  cy*cp,
    };
    memcpy(m, r, sizeof r);
}
