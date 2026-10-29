// Вывод видео через OpenGL (Mesa/nouveau на Switch).
// YUV -> RGB и все проекции (2D, 3D SBS, VR180, VR360, коррекция линз)
// делаются в одном фрагментном шейдере: для каждого пикселя считаем луч
// взгляда и берём нужную точку из equirectangular-кадра.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include <libavutil/pixfmt.h>
#include "render.h"

static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static GLuint s_prog, s_vao, s_vbo, s_tex[3];
static int    s_tw, s_th;
static bool   s_has_frame, s_full_range, s_bt709;
static float  s_frame_aspect = 16.0f / 9.0f;

static GLint u_mode, u_eye, u_rot, u_tan, u_distort, u_k, u_view_aspect, u_eye_aspect,
             u_scale, u_full, u_bt709, u_shift;

// ---------------------------------------------------------------------------
static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vPos;\n"
    "void main(){ vPos = aPos; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *FS =
    "#version 330 core\n"
    "in vec2 vPos;\n"
    "out vec4 oColor;\n"
    "uniform sampler2D tY, tU, tV;\n"
    "uniform int uMode, uEye, uDistort, uFull, uBt709;\n"
    "uniform mat3 uRot;\n"
    "uniform vec2 uTan, uK;\n"
    "uniform float uViewAspect, uEyeAspect, uScale, uShift;\n"
    "const float PI = 3.14159265;\n"
    "vec3 rgb(vec2 uv){\n"
    "  float y = texture(tY, uv).r;\n"
    "  float u = texture(tU, uv).r - 0.5;\n"
    "  float v = texture(tV, uv).r - 0.5;\n"
    "  if (uFull == 0) { y = (y - 0.0627) * 1.1644; u *= 1.1384; v *= 1.1384; }\n"
    "  vec3 c = (uBt709 == 1)\n"
    "    ? vec3(y + 1.5748*v, y - 0.1873*u - 0.4681*v, y + 1.8556*u)\n"
    "    : vec3(y + 1.4020*v, y - 0.3441*u - 0.7141*v, y + 1.7720*u);\n"
    "  return clamp(c, 0.0, 1.0);\n"
    "}\n"
    "void main(){\n"
    "  vec2 p = vPos;\n"
    "  p.x += uShift;\n"
    "  if (uDistort == 1) {\n"                    // бочкообразная пред-дисторсия под линзы
    "    vec2 q = p * vec2(uViewAspect, 1.0);\n"
    "    float r2 = dot(q, q);\n"
    "    p *= 1.0 + uK.x * r2 + uK.y * r2 * r2;\n"
    "  }\n"
    "  vec2 uv;\n"
    "  if (uMode <= 1) {\n"                      // 2D и 3D SBS: плоский экран
    "    vec2 s = vec2(1.0);\n"
    "    if (uViewAspect > uEyeAspect) s.x = uEyeAspect / uViewAspect; else s.y = uViewAspect / uEyeAspect;\n"
    "    vec2 t = p / (s * uScale);\n"
    "    if (abs(t.x) > 1.0 || abs(t.y) > 1.0) { oColor = vec4(0,0,0,1); return; }\n"
    "    uv = vec2(t.x * 0.5 + 0.5, 0.5 - t.y * 0.5);\n"
    "    if (uMode == 1) uv.x = uv.x * 0.5 + 0.5 * float(uEye);\n"
    "  } else {\n"                               // сферические режимы
    "    vec3 d = normalize(uRot * vec3(p.x * uTan.x, p.y * uTan.y, -1.0));\n"
    "    float lon = atan(d.x, -d.z);\n"
    "    float lat = asin(clamp(d.y, -1.0, 1.0));\n"
    "    if (uMode == 2) {\n"                    // VR180 SBS
    "      if (abs(lon) > PI * 0.5) { oColor = vec4(0,0,0,1); return; }\n"
    "      uv = vec2(lon / PI + 0.5, 0.5 - lat / PI);\n"
    "      uv.x = uv.x * 0.5 + 0.5 * float(uEye);\n"
    "    } else {\n"                             // VR360 (моно / top-bottom / окно)
    "      uv = vec2(fract(lon / (2.0 * PI) + 0.5), 0.5 - lat / PI);\n"
    "      if (uMode == 4) uv.y = uv.y * 0.5 + 0.5 * float(uEye);\n"
    "    }\n"
    "  }\n"
    "  oColor = vec4(rgb(uv), 1.0);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Крошечный шрифт 3x5 для экранной панели (рисуется прямоугольниками glClear)
static const char FONT_CHARS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:/.-+%";
static const unsigned char FONT[][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,2,2},
    {7,5,7,5,7},{7,5,7,1,7},
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},
    {7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{6,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},
    {2,5,5,6,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
    {5,5,2,2,2},{7,1,2,4,7},
    {0,2,0,2,0},{1,1,2,4,4},{0,0,0,0,2},{0,0,7,0,0},{0,2,7,2,0},{5,1,2,4,5},
};

static void fill_rect(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static int text_width(const char *s, int px) { return (int)strlen(s) * 4 * px - px; }

static void draw_text(int x, int y, int px, const char *s, float r, float g, float b) {
    for (int i = 0; s[i]; i++) {
        char ch = s[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        const char *f = strchr(FONT_CHARS, ch);
        if (!f || ch == 0) continue;
        const unsigned char *gl = FONT[f - FONT_CHARS];
        for (int row = 0; row < 5; row++) {
            int col = 0;
            while (col < 3) { // объединяем подряд идущие пиксели в один прямоугольник
                if (!(gl[row] & (4 >> col))) { col++; continue; }
                int start = col;
                while (col < 3 && (gl[row] & (4 >> col))) col++;
                fill_rect(x + (i * 4 + start) * px, y + (4 - row) * px, (col - start) * px, px, r, g, b);
            }
        }
    }
}

static void fmt_time(char *out, size_t n, double t) {
    if (t < 0 || isnan(t)) t = 0;
    int s = (int)t, h = s / 3600, m = (s / 60) % 60;
    s %= 60;
    if (h) snprintf(out, n, "%d:%02d:%02d", h, m, s);
    else   snprintf(out, n, "%02d:%02d", m, s);
}

// Панель внизу области (x0..x0+w), центр по горизонтали
static void draw_osd(int x0, int w, int h, bool headset, const RenderParams *rp) {
    glEnable(GL_SCISSOR_TEST);
    int px = headset ? 3 : 4;
    int bar_w = headset ? w * 55 / 100 : w * 80 / 100;
    int bar_x = x0 + (w - bar_w) / 2;
    int bar_y = headset ? h * 30 / 100 : h * 6 / 100;
    int line = 7 * px;

    char t1[32], t2[32], timebuf[80];
    fmt_time(t1, sizeof t1, rp->pos);
    fmt_time(t2, sizeof t2, rp->dur);
    snprintf(timebuf, sizeof timebuf, "%s / %s", t1, t2);

    char title[64];
    snprintf(title, sizeof title, "%s%s", mode_name(rp->mode), rp->paused ? "  PAUSE" : "");

    // подложка
    int box_h = line * 3 + 6 * px;
    fill_rect(bar_x - 3 * px, bar_y - 3 * px, bar_w + 6 * px, box_h, 0.05f, 0.05f, 0.07f);

    // полоса прогресса
    fill_rect(bar_x, bar_y, bar_w, px * 2, 0.25f, 0.25f, 0.3f);
    if (rp->dur > 0) {
        double k = rp->pos / rp->dur;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        fill_rect(bar_x, bar_y, (int)(bar_w * k), px * 2, 0.2f, 0.65f, 1.0f);
    }
    draw_text(bar_x, bar_y + line - px * 2, px, timebuf, 1, 1, 1);
    draw_text(bar_x, bar_y + line * 2, px, title, 1.0f, 0.85f, 0.3f);
    if (rp->msg && rp->msg[0]) {
        int tw = text_width(rp->msg, px);
        draw_text(bar_x + bar_w - tw, bar_y + line * 2, px, rp->msg, 0.5f, 1.0f, 0.6f);
    }
    glDisable(GL_SCISSOR_TEST);
}

// ---------------------------------------------------------------------------
static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        printf("shader error: %s\n", log);
    }
    return sh;
}

static bool init_egl(NWindow *win) {
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display) return false;
    eglInitialize(s_display, NULL, NULL);
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) goto fail;

    static const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(s_display, cfg_attr, &cfg, 1, &n);
    if (n == 0) goto fail;

    s_surface = eglCreateWindowSurface(s_display, cfg, win, NULL);
    if (!s_surface) goto fail;

    static const EGLint ctx_attr[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_NONE
    };
    s_context = eglCreateContext(s_display, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!s_context) goto fail;
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    eglSwapInterval(s_display, 1);
    return true;

fail:
    if (s_surface) eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_display = NULL; s_surface = NULL;
    return false;
}

bool render_init(NWindow *win) {
    setenv("MESA_NO_ERROR", "1", 1);
    if (!init_egl(win)) return false;
    gladLoadGL();

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(s_prog);

    glUniform1i(glGetUniformLocation(s_prog, "tY"), 0);
    glUniform1i(glGetUniformLocation(s_prog, "tU"), 1);
    glUniform1i(glGetUniformLocation(s_prog, "tV"), 2);
    u_mode        = glGetUniformLocation(s_prog, "uMode");
    u_eye         = glGetUniformLocation(s_prog, "uEye");
    u_rot         = glGetUniformLocation(s_prog, "uRot");
    u_tan         = glGetUniformLocation(s_prog, "uTan");
    u_distort     = glGetUniformLocation(s_prog, "uDistort");
    u_k           = glGetUniformLocation(s_prog, "uK");
    u_view_aspect = glGetUniformLocation(s_prog, "uViewAspect");
    u_eye_aspect  = glGetUniformLocation(s_prog, "uEyeAspect");
    u_scale       = glGetUniformLocation(s_prog, "uScale");
    u_full        = glGetUniformLocation(s_prog, "uFull");
    u_bt709       = glGetUniformLocation(s_prog, "uBt709");
    u_shift       = glGetUniformLocation(s_prog, "uShift");

    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);

    glGenTextures(3, s_tex);
    s_tw = s_th = 0;
    s_has_frame = false;
    return true;
}

void render_exit(void) {
    if (!s_display) return;
    glDeleteTextures(3, s_tex);
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_prog);
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_display, s_context);
    eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_display = NULL; s_context = NULL; s_surface = NULL;
}

void render_upload(const AVFrame *f) {
    if (f->width != s_tw || f->height != s_th) {
        s_tw = f->width;
        s_th = f->height;
        for (int i = 0; i < 3; i++) {
            int w = i ? (s_tw + 1) / 2 : s_tw;
            int h = i ? (s_th + 1) / 2 : s_th;
            glBindTexture(GL_TEXTURE_2D, s_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; i++) {
        int w = i ? (s_tw + 1) / 2 : s_tw;
        int h = i ? (s_th + 1) / 2 : s_th;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, f->data[i]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    s_full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    s_bt709 = f->colorspace == AVCOL_SPC_BT709 ||
              (f->colorspace == AVCOL_SPC_UNSPECIFIED && s_th >= 720);
    float sar = (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
                    ? (float)f->sample_aspect_ratio.num / f->sample_aspect_ratio.den : 1.0f;
    s_frame_aspect = (float)s_tw * sar / (float)s_th;
    s_has_frame = true;
}

void render_draw(const RenderParams *rp) {
    EGLint W = 1280, H = 720;
    eglQuerySurface(s_display, s_surface, EGL_WIDTH, &W);
    eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &H);

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    bool headset = mode_is_headset(rp->mode);
    int eyes = headset ? 2 : 1;
    int ew = W / eyes;
    float view_aspect = (float)ew / (float)H;

    // соотношение сторон картинки одного глаза
    float eye_aspect = s_frame_aspect;
    if (rp->mode == MODE_SBS_3D && s_frame_aspect >= 2.5f)
        eye_aspect = s_frame_aspect * 0.5f; // full-SBS (например 3840x1080)

    float tan_y = tanf(rp->fov_deg * 0.5f * (float)M_PI / 180.0f);

    for (int eye = 0; eye < eyes; eye++) {
        int x0 = eye * ew;
        if (s_has_frame) {
            glViewport(x0, 0, ew, H);
            glUseProgram(s_prog);
            glBindVertexArray(s_vao);
            for (int i = 0; i < 3; i++) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, s_tex[i]);
            }
            glUniform1i(u_mode, (int)rp->mode);
            glUniform1i(u_eye, eye);
            glUniformMatrix3fv(u_rot, 1, GL_FALSE, rp->rot);
            glUniform2f(u_tan, tan_y * view_aspect, tan_y);
            glUniform1i(u_distort, headset && rp->distort ? 1 : 0);
            glUniform2f(u_k, 0.22f, 0.24f);
            glUniform1f(u_view_aspect, view_aspect);
            glUniform1f(u_eye_aspect, eye_aspect);
            glUniform1f(u_scale, rp->mode == MODE_SBS_3D ? rp->screen_scale : 1.0f);
            glUniform1i(u_full, s_full_range ? 1 : 0);
            glUniform1i(u_bt709, s_bt709 ? 1 : 0);
            glUniform1f(u_shift, headset ? (eye == 0 ? rp->ipd : -rp->ipd) : 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        if (rp->osd || !s_has_frame) {
            glViewport(0, 0, W, H);
            draw_osd(x0, ew, H, headset, rp);
        }
    }
    if (headset) { // тонкая разделительная линия между глазами
        glEnable(GL_SCISSOR_TEST);
        fill_rect(ew - 1, 0, 2, H, 0, 0, 0);
        glDisable(GL_SCISSOR_TEST);
    }
    eglSwapBuffers(s_display, s_surface);
}
