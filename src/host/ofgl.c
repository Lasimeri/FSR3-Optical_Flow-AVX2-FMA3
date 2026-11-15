/* ofgl: one monitor's pictures doubled, with the pictures on the GPU and the optical flow on this CPU: AVX2 and FMA3, at most 8 threads on 8 cores, this program's own thread counted among them.
 *
 *   The compositor's cast of the monitor as dmabufs, capped at the rate asked for  ─▶  GPU: is it a new picture (occlusion query), a copy of it, its luma at the field's size  ─▶  the luma's bytes into this process's memory
 *   ─▶  the flow's threads (src/engine): the field at the middle instant  ─▶  GPU: both frames moved half way along the vectors under the triangle windows of bgra_blend_vx  ─▶  overlay: generated, real, generated, real.
 *
 * Rates. --rate N: N pictures a second in, 2 N out, each picture held a whole number of the monitor's refreshes (24 on a 120 Hz monitor: a generated picture 2 refreshes, a real frame 3; 30: 2 and 2). No --rate: half the
 * refresh in, the refresh out, one picture a refresh. A picture always starts on a refresh: that is the vertical sync.
 * Threads. The process is confined to one CPU of each of at most 8 cores before anything else exists; the flow gets at most 7 threads, each pinned to a core of its own, and this thread (PipeWire's loop, Wayland, GL: one
 * thread, so the GL context never changes hands) takes the core that is left.
 *
 * The cast is asked of KWin by this program itself (zkde_screencast_unstable_v1, src/host/wlp.c); --node names one made some other way. scripts/ofgl.sh starts it; scripts/authorize.sh gives it the right, once. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <signal.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "engine.h"
#include "wlp.h"

#define FOURCC_XR24 0x34325258u /* DRM_FORMAT_XRGB8888: SPA's BGRx */
#define FOURCC_AR24 0x34325241u /* DRM_FORMAT_ARGB8888: SPA's BGRA */
#define MOD_INVALID 0x00ffffffffffffffull
#define MODS_MAX 64
#define FR_N OF_SLOTS /* own frame textures: frame i's luma goes in luma slot i */
#define Q_N 16

enum { IT_REAL, IT_MID };

struct item {
    int kind, a, b, v; /* frames (a: the older, -1 for a real frame), the vector texture or -1 */
    uint64_t t_src;
    int hold; /* refreshes it stays on the glass */
};

struct job {
    uint64_t seq;
    int in, a, out; /* this frame, the one before (-1: no mid asked), the vector slot */
    int mid_done, late, real_hold;
    uint64_t pts, t_pub;
};

struct shown {
    uint64_t t_src;
    int generated;
    int hold, waiting;
    uint64_t t_draw;
};

struct stats {
    uint64_t cast, dup, noslot;
    uint64_t sent, made, scene, nomid, late;
    uint64_t com_g, com_r, skip_g;
    uint64_t pres_g, pres_r, disc;
    uint64_t cad[5];
    uint64_t lat_sum, lat_n, lat_max, lat_min;
    uint64_t gap_max;
    uint64_t flow_sum, flow_n, flow_max, moved_sum, wait_sum;
    uint64_t gpu_ns, gpu_n, gpu_max;
};

struct pwbuf {
    EGLImage img;
    GLuint tex;
};

static volatile sig_atomic_t stop;
static void on_signal(int s)
{
    (void)s;
    stop = 1;
}

struct app {
    struct of_engine eng;
    struct wlp *w;
    int W, H, mhz, div, lw, lh, lstride, bw, bh, rate;
    int probe, pass_only, fatal, trace;
    uint8_t *probe_luma;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    struct wl_egl_window *win;
    void (*image_target)(GLenum, GLeglImageOES);
    GLuint prog_copy, prog_cmp, prog_luma, prog_mid, prog_show, query;
    GLuint fr_tex[FR_N], fr_fbo[FR_N], luma_tex, luma_fbo, vec_tex[OF_SLOTS];
    int fr_refs[FR_N], vec_busy[OF_SLOTS], out_turn;
    struct pw_loop *loop;
    struct pw_context *pctx;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook hook;
    struct spa_video_info_raw raw;
    int negotiated, dmabuf, external;
    char format[160];
    uint64_t mods[2][MODS_MAX];
    int mod_ext[2][MODS_MAX], nmods[2];
    int prev;
    uint64_t last_src_t, interval_ns;
    struct job jobs[OF_RING];
    uint64_t pub, done;
    int must_skip_mid;
    struct item q[Q_N];
    unsigned q_head, q_tail;
    int can_draw;
    uint64_t t_swap, t_fc, cur_t, refresh_ns, pair_num, pair_den, pair_acc;
    int cur_hold, deep;
    struct shown ring[256];
    uint64_t commits, last_t;
    struct stats s;
};

/* ---- GL ---- */

static GLuint shader(GLenum type, const char *src)
{
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "ofgl: shader: %s\n%s\n", log, src);
        return 0;
    }
    return s;
}

/* A program over one triangle that covers the target: every pass is a fragment shader. */
static GLuint program(const char *head, const char *body)
{
    static const char *const VS = "#version 310 es\nvoid main() { gl_Position = vec4(float((gl_VertexID << 1) & 2) * 2.0 - 1.0, float(gl_VertexID & 2) * 2.0 - 1.0, 0.0, 1.0); }\n";
    char fs[4096];
    snprintf(fs, sizeof fs, "%s%s", head, body);
    const GLuint v = shader(GL_VERTEX_SHADER, VS), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
        return 0;
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "ofgl: link: %s\n", log);
        return 0;
    }
    glUseProgram(p);
    return p;
}

static const char *const HEAD_2D = "#version 310 es\nprecision highp float;\nprecision highp int;\n#define SAMPLER sampler2D\n";
static const char *const HEAD_EXT = "#version 310 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;\nprecision highp int;\n#define SAMPLER samplerExternalOES\n";

/* The cast's picture into a texture of ours. */
static const char *const FS_COPY = "uniform highp SAMPLER S; uniform vec2 SZ; out vec4 o;\n"
                                   "void main() { o = vec4(texture(S, gl_FragCoord.xy / SZ).rgb, 1.0); }\n";
/* A fragment survives only where the cast's picture differs from the last one kept: the occlusion query then says whether any pixel changed. */
static const char *const FS_CMP = "uniform highp SAMPLER S; uniform highp sampler2D P; uniform vec2 SZ; out vec4 o;\n"
                                  "void main() { vec3 d = abs(texture(S, gl_FragCoord.xy / SZ).rgb - texelFetch(P, ivec2(gl_FragCoord.xy), 0).rgb); if (d.r + d.g + d.b == 0.0) discard; o = vec4(1.0); }\n";
/* The field's luma, four samples of a row in one RGBA texel, so a plain RGBA read gives its bytes in order. A sample is the box of DIV x DIV frame pixels under it: four bilinear taps a quarter of DIV from its centre. */
static const char *const FS_LUMA = "uniform highp sampler2D S; uniform vec2 SZ; uniform float DIV; out vec4 o;\n"
                                   "float l(vec2 c) { float q = 0.25 * DIV; vec3 s = texture(S, (c + vec2(-q, -q)) / SZ).rgb + texture(S, (c + vec2(q, -q)) / SZ).rgb + texture(S, (c + vec2(-q, q)) / SZ).rgb + texture(S, (c + vec2(q, q)) / SZ).rgb;\n"
                                   "  return dot(s, vec3(0.299, 0.587, 0.114) * 0.25); }\n"
                                   "void main() { vec2 f = floor(gl_FragCoord.xy); float y = (f.y + 0.5) * DIV; float x = f.x * 4.0 + 0.5;\n"
                                   "  o = vec4(l(vec2(x * DIV, y)), l(vec2((x + 1.0) * DIV, y)), l(vec2((x + 2.0) * DIV, y)), l(vec2((x + 3.0) * DIV, y))); }\n";
/* The generated picture. The flow's vectors are per block of 8 x 8 field pixels, TILE frame pixels; a pixel takes from the four blocks whose centres surround it, by the triangle (bilinear) weights bgra_blend_vx has in W_TAB, and
 * each block's vector v gives the mean of the older frame at p + v HALF and the newer at p - v HALF: the same place of the scene at the middle instant. The window's y runs up, the frames' down. */
static const char *const FS_MID = "uniform highp sampler2D A; uniform highp sampler2D B; uniform highp isampler2D V; uniform vec2 SZ; uniform float TILE; uniform float HALF; out vec4 o;\n"
                                  "void main() { vec2 p = vec2(gl_FragCoord.x, SZ.y - gl_FragCoord.y); vec2 u = p / TILE - 0.5; vec2 b = floor(u); vec2 f = u - b; ivec2 vmax = textureSize(V, 0) - 1; vec3 acc = vec3(0.0);\n"
                                  "  for (int k = 0; k < 4; k++) { ivec2 off = ivec2(k & 1, k >> 1); vec2 d = vec2(texelFetch(V, clamp(ivec2(b) + off, ivec2(0), vmax), 0).xy) * HALF;\n"
                                  "    float w = (off.x == 1 ? f.x : 1.0 - f.x) * (off.y == 1 ? f.y : 1.0 - f.y); acc += w * (texture(A, (p + d) / SZ).rgb + texture(B, (p - d) / SZ).rgb); }\n"
                                  "  o = vec4(acc * 0.5, 1.0); }\n";
static const char *const FS_SHOW = "uniform highp sampler2D S; uniform vec2 SZ; out vec4 o;\n"
                                   "void main() { o = vec4(texelFetch(S, ivec2(gl_FragCoord.x, SZ.y - gl_FragCoord.y), 0).rgb, 1.0); }\n";

static GLuint texture(GLenum internal, int w, int h, GLint filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static GLuint fbo_of(GLuint tex)
{
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? f : 0;
}

static void set2(GLuint p, const char *name, float x, float y) { glUniform2f(glGetUniformLocation(p, name), x, y); }
static void set1(GLuint p, const char *name, float x) { glUniform1f(glGetUniformLocation(p, name), x); }
static void seti(GLuint p, const char *name, int x) { glUniform1i(glGetUniformLocation(p, name), x); }

static int gl_open(struct app *a)
{
    a->dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wlp_display(a->w), NULL);
    EGLint major = 0, minor = 0, n = 0;
    if (a->dpy == EGL_NO_DISPLAY || !eglInitialize(a->dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "ofgl: no EGL on the Wayland display\n");
        return -1;
    }
    const char *ext = eglQueryString(a->dpy, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers")) {
        fprintf(stderr, "ofgl: EGL has no EGL_EXT_image_dma_buf_import_modifiers\n");
        return -1;
    }
    static const EGLint want[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_NONE};
    static const EGLint version[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLConfig cfg;
    if (!eglChooseConfig(a->dpy, want, &cfg, 1, &n) || n < 1 || (a->ctx = eglCreateContext(a->dpy, cfg, EGL_NO_CONTEXT, version)) == EGL_NO_CONTEXT) {
        fprintf(stderr, "ofgl: no GLES 3.1 context: EGL error %#x\n", eglGetError());
        return -1;
    }
    a->win = wl_egl_window_create(wlp_surface(a->w), a->W, a->H);
    a->surf = a->win ? eglCreatePlatformWindowSurface(a->dpy, cfg, a->win, NULL) : EGL_NO_SURFACE;
    if (a->surf == EGL_NO_SURFACE || !eglMakeCurrent(a->dpy, a->surf, a->surf, a->ctx)) {
        fprintf(stderr, "ofgl: no EGL surface on the overlay: EGL error %#x\n", eglGetError());
        return -1;
    }
    eglSwapInterval(a->dpy, 0); /* the pace is the compositor's frame callback, not a wait inside the swap */
    a->image_target = (void (*)(GLenum, GLeglImageOES))eglGetProcAddress("glEGLImageTargetTexture2DOES");
    EGLBoolean (*query_mods)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *) = (EGLBoolean(*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *))eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    if (!a->image_target || !query_mods) {
        fprintf(stderr, "ofgl: no glEGLImageTargetTexture2DOES or eglQueryDmaBufModifiersEXT\n");
        return -1;
    }
    static const EGLint fourcc[2] = {(EGLint)FOURCC_XR24, (EGLint)FOURCC_AR24};
    for (int k = 0; k < 2; k++) {
        EGLuint64KHR m[MODS_MAX];
        EGLBoolean e[MODS_MAX];
        EGLint got = 0;
        if (!query_mods(a->dpy, fourcc[k], MODS_MAX, m, e, &got))
            got = 0;
        for (int i = 0; i < got; i++) {
            a->mods[k][i] = m[i];
            a->mod_ext[k][i] = e[i] != EGL_FALSE;
        }
        a->nmods[k] = got;
    }
    printf("gl: EGL %d.%d, %s, %s; dmabuf modifiers: %d for XRGB8888, %d for ARGB8888\n", major, minor, (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), a->nmods[0], a->nmods[1]);
    const float W = (float)a->W, H = (float)a->H;
    for (int i = 0; i < FR_N; i++) {
        a->fr_tex[i] = texture(GL_RGBA8, a->W, a->H, GL_LINEAR);
        a->fr_fbo[i] = fbo_of(a->fr_tex[i]);
        if (!a->fr_fbo[i])
            return -1;
    }
    a->luma_tex = texture(GL_RGBA8, a->lw / 4, a->lh, GL_NEAREST);
    a->luma_fbo = fbo_of(a->luma_tex);
    for (int i = 0; i < OF_SLOTS; i++)
        a->vec_tex[i] = texture(GL_RG16I, a->bw, a->bh, GL_NEAREST);
    glGenQueries(1, &a->query);
    a->prog_luma = program(HEAD_2D, FS_LUMA);
    if (!a->prog_luma || !a->luma_fbo)
        return -1;
    seti(a->prog_luma, "S", 0);
    set2(a->prog_luma, "SZ", W, H);
    set1(a->prog_luma, "DIV", (float)a->div);
    a->prog_mid = program(HEAD_2D, FS_MID);
    if (!a->prog_mid)
        return -1;
    seti(a->prog_mid, "A", 0);
    seti(a->prog_mid, "B", 1);
    seti(a->prog_mid, "V", 2);
    set2(a->prog_mid, "SZ", W, H);
    set1(a->prog_mid, "TILE", 8.0f * (float)a->div);
    set1(a->prog_mid, "HALF", 0.5f * (float)a->div);
    a->prog_show = program(HEAD_2D, FS_SHOW);
    if (!a->prog_show)
        return -1;
    seti(a->prog_show, "S", 0);
    set2(a->prog_show, "SZ", W, H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    return 0;
}

/* The two passes that read the cast's own texture, once its kind is known: a plain 2D texture, or an external one. */
static int gl_cast_programs(struct app *a)
{
    const char *head = a->external ? HEAD_EXT : HEAD_2D;
    a->prog_copy = program(head, FS_COPY);
    if (!a->prog_copy)
        return -1;
    seti(a->prog_copy, "S", 0);
    set2(a->prog_copy, "SZ", (float)a->W, (float)a->H);
    a->prog_cmp = program(head, FS_CMP);
    if (!a->prog_cmp)
        return -1;
    seti(a->prog_cmp, "S", 0);
    seti(a->prog_cmp, "P", 1);
    set2(a->prog_cmp, "SZ", (float)a->W, (float)a->H);
    return 0;
}

/* A cast buffer as a texture. Every plane of the dmabuf is named to EGL: a modifier with metadata planes (AMD's DCC) has up to four. */
static struct pwbuf *import(struct app *a, const struct spa_buffer *buf)
{
    static const EGLint P_FD[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint P_OFF[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint P_PITCH[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint P_LO[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint P_HI[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    const struct spa_data *d = &buf->datas[0];
    EGLAttrib at[64];
    int n = 0;
#define AT(k, v) (at[n++] = (EGLAttrib)(k), at[n++] = (EGLAttrib)(v))
    AT(EGL_WIDTH, a->W);
    AT(EGL_HEIGHT, a->H);
    AT(EGL_LINUX_DRM_FOURCC_EXT, a->raw.format == SPA_VIDEO_FORMAT_BGRA ? FOURCC_AR24 : FOURCC_XR24);
    for (uint32_t pl = 0; pl < buf->n_datas && pl < 4; pl++) {
        const struct spa_data *e = &buf->datas[pl];
        if (e->type != SPA_DATA_DmaBuf || !e->chunk)
            break;
        AT(P_FD[pl], e->fd);
        AT(P_OFF[pl], e->chunk->offset);
        AT(P_PITCH[pl], e->chunk->stride ? e->chunk->stride : a->W * 4);
        if (a->raw.modifier != MOD_INVALID) {
            AT(P_LO[pl], a->raw.modifier & 0xffffffffu);
            AT(P_HI[pl], a->raw.modifier >> 32);
        }
    }
#undef AT
    at[n] = EGL_NONE;
    const EGLImage img = eglCreateImage(a->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
    if (img == EGL_NO_IMAGE) {
        fprintf(stderr, "ofgl: the cast's dmabuf (%u planes; the first: fd %d, pitch %d, offset %u, modifier %#llx) could not be imported: EGL error %#x\n", buf->n_datas, (int)d->fd, d->chunk->stride, d->chunk->offset, (unsigned long long)a->raw.modifier, eglGetError());
        return NULL;
    }
    struct pwbuf *b = calloc(1, sizeof *b);
    b->img = img;
    const GLenum target = a->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    glGenTextures(1, &b->tex);
    glBindTexture(target, b->tex);
    a->image_target(target, (GLeglImageOES)img);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "ofgl: the dmabuf's image did not become a texture: GL error %#x\n", err);
        return NULL;
    }
    return b;
}

/* ---- the order of pictures ---- */

static inline void fr_unref(struct app *a, int i)
{
    if (i >= 0)
        a->fr_refs[i]--;
}

static void item_release(struct app *a, const struct item *it)
{
    fr_unref(a, it->a);
    fr_unref(a, it->b);
    if (it->v >= 0)
        a->vec_busy[it->v] = 0;
}

static inline unsigned q_len(const struct app *a) { return a->q_tail - a->q_head; }

static void enqueue_real(struct app *a, int frame, uint64_t t_src, int hold)
{
    /* Far behind (the output asleep, no frame callbacks): what waits is dropped, the newest stands. */
    if (q_len(a) >= Q_N - 2)
        while (a->q_head != a->q_tail) {
            item_release(a, &a->q[a->q_head++ % Q_N]);
            a->s.skip_g++;
        }
    a->fr_refs[frame]++;
    a->q[a->q_tail++ % Q_N] = (struct item){IT_REAL, -1, frame, -1, t_src, hold};
}

static int free_vec(struct app *a)
{
    for (int i = 0; i < OF_SLOTS; i++) {
        const int k = (a->out_turn + i) % OF_SLOTS;
        if (!a->vec_busy[k]) {
            a->out_turn = (k + 1) % OF_SLOTS;
            return k;
        }
    }
    return -1;
}

/* A new picture, in frame texture s with its luma in luma slot s: to the flow, which must see every frame. older: the frame before, still referenced by the caller. */
static void submit(struct app *a, int s, int older, int gap_ok, uint64_t pts)
{
    if (a->pass_only) {
        enqueue_real(a, s, pts, 1);
        return;
    }
    if (a->pub - a->done >= OF_RING / 2) {
        enqueue_real(a, s, pts, 1);
        a->must_skip_mid = 1;
        return;
    }
    const int o = gap_ok && !a->must_skip_mid && older >= 0 ? free_vec(a) : -1;
    const int want_mid = o >= 0;
    a->must_skip_mid = 0;
    if (want_mid) {
        a->vec_busy[o] = 1;
        a->fr_refs[older]++;
    } else {
        a->s.nomid++;
    }
    a->fr_refs[s]++;
    const uint64_t seq = ++a->pub;
    a->eng.msg[seq % OF_RING] = (struct of_msg){.seq = seq, .in_slot = (uint32_t)s, .out_slot = (uint32_t)(want_mid ? o : 0), .flags = want_mid ? 0u : (uint32_t)OF_NO_MID};
    of_engine_submit(&a->eng, seq);
    /* A frame with no mid waits for nothing when nothing is ahead of it. */
    const int now_shown = !want_mid && seq == a->done + 1;
    if (now_shown)
        enqueue_real(a, s, pts, 1);
    a->jobs[seq % OF_RING] = (struct job){.seq = seq, .in = s, .a = want_mid ? older : -1, .out = want_mid ? o : -1, .mid_done = !want_mid, .late = now_shown, .pts = pts, .t_pub = of_now_ns()};
    a->s.sent++;
}

static void take_mid(struct app *a)
{
    const uint64_t have = __atomic_load_n(&a->eng.gen_pub, __ATOMIC_SEQ_CST);
    for (uint64_t s = a->done + 1; s <= a->pub && s <= have; s++) {
        struct job *j = &a->jobs[s % OF_RING];
        /* A frame whose answer is no field (a scene change) is take_done's. */
        if (j->mid_done || a->eng.gen[s % OF_RING].made != 1)
            continue;
        j->mid_done = 1;
        a->s.made++;
        if (!j->late && q_len(a) < 3) {
            glBindTexture(GL_TEXTURE_2D, a->vec_tex[j->out]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, a->bw, a->bh, GL_RG_INTEGER, GL_SHORT, a->eng.vec[j->out]);
            a->fr_refs[j->in]++;
            /* Half way between the two frames' instants. The job's hold on the older frame becomes the item's. */
            /* What this source frame is worth in refreshes, the fraction carried: the generated picture takes the smaller half, the real frame after it the rest. */
            a->pair_acc += a->pair_num;
            const int pair = (int)(a->pair_acc / a->pair_den);
            a->pair_acc %= a->pair_den;
            const int g_hold = pair / 2 > 0 ? pair / 2 : 1;
            j->real_hold = pair - g_hold > 0 ? pair - g_hold : 1;
            a->q[a->q_tail++ % Q_N] = (struct item){IT_MID, j->a, j->in, j->out, j->pts - a->interval_ns / 2, g_hold};
        } else {
            a->s.skip_g++;
            fr_unref(a, j->a);
            a->vec_busy[j->out] = 0;
        }
        j->a = -1;
    }
}

static void take_done(struct app *a)
{
    take_mid(a);
    const uint64_t have = __atomic_load_n(&a->eng.gen_pub, __ATOMIC_SEQ_CST);
    while (a->done < have && a->done < a->pub) {
        const uint64_t seq = ++a->done;
        struct job *j = &a->jobs[seq % OF_RING];
        const struct of_gen g = a->eng.gen[seq % OF_RING];
        a->s.flow_sum += g.us[OF_T_TOTAL];
        a->s.flow_n++;
        a->s.wait_sum += g.us[OF_T_WAIT];
        a->s.flow_max = g.us[OF_T_TOTAL] > a->s.flow_max ? g.us[OF_T_TOTAL] : a->s.flow_max;
        a->s.moved_sum += g.moved;
        if (g.made == 0)
            a->s.scene++;
        /* Asked for a mid that did not come: a scene change. */
        if (!j->mid_done) {
            j->mid_done = 1;
            fr_unref(a, j->a);
            a->vec_busy[j->out] = 0;
            j->a = -1;
        }
        if (!j->late)
            enqueue_real(a, j->in, j->pts, j->real_hold ? j->real_hold : 1);
        fr_unref(a, j->in);
    }
}

/* A frame the flow has held too long reaches the glass, and its generated picture, should it come, is dropped. */
static void take_late(struct app *a)
{
    const uint64_t now = of_now_ns();
    for (uint64_t s = a->done + 1; s <= a->pub; s++) {
        struct job *j = &a->jobs[s % OF_RING];
        if (!j->late && now - j->t_pub > 2 * a->interval_ns) {
            j->late = 1;
            a->s.late++;
            enqueue_real(a, j->in, j->pts, 1);
        }
    }
}

/* When the next picture may be drawn so that the one on the glass gets its refreshes, and is not replaced before it was ever shown. 0: at once. KWin has two depths and moves between them, and each needs its own clock:
 *   deep (a commit reaches the glass 1 to 2 refreshes later: KWin composes right after a vblank): the frame callback marks the start of KWin's cycle; that instant plus whole refreshes is the same point of a later
 *     cycle. A picture held h refreshes is followed h cycles on; one held 1 is followed at the frame callback itself.
 *   shallow (under 1 refresh: KWin composes just before the vblank): the frame callback can come while the picture still waits for its cycle, and the next one sent then replaces it unseen (measured: generated
 *     pictures discarded). There the clock is the picture's own scanout: the next one goes just after the vblank that ends the held refreshes, far from where KWin latches.
 * Measured and rejected: the scanout clock in the deep state (1 and 3 refreshes for 2 and 2), and wp_fifo barriers in either (a barrier falls at a refresh its commit missed). docs/measurements.md. */
static uint64_t hold_due(const struct app *a)
{
    if (a->deep)
        return a->cur_hold > 1 && a->t_fc ? a->t_fc + (uint64_t)(a->cur_hold - 1) * a->refresh_ns : 0;
    if (a->cur_t)
        return a->cur_t + (uint64_t)(a->cur_hold - 1) * a->refresh_ns + 300000ull;
    /* Not shown yet, or its feedback never came: a refresh past the time it must have been taken up. */
    return a->t_swap ? a->t_swap + (uint64_t)a->cur_hold * a->refresh_ns + 1000000ull : 0;
}

/* One picture for a refresh the compositor has asked for, once the picture before it has had its refreshes. */
static void draw_next(struct app *a)
{
    const uint64_t now = of_now_ns();
    if (!a->can_draw && a->t_swap && now - a->t_swap > 100000000ull)
        a->can_draw = 1; /* no frame callback for 100 ms: the output sleeps, or the surface is not shown */
    if (!a->can_draw || a->q_head == a->q_tail)
        return;
    const uint64_t due = hold_due(a);
    if (due && now < due)
        return;
    const struct item it = a->q[a->q_head++ % Q_N];
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, a->W, a->H);
    if (it.kind == IT_MID) {
        glUseProgram(a->prog_mid);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, a->vec_tex[it.v]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, a->fr_tex[it.b]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a->fr_tex[it.a]);
        a->s.com_g++;
    } else {
        glUseProgram(a->prog_show);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a->fr_tex[it.b]);
        a->s.com_r++;
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint64_t c = a->commits++;
    a->ring[c & 255] = (struct shown){.t_src = it.t_src, .generated = it.kind == IT_MID};
    wlp_before_swap(a->w, c);
    eglSwapBuffers(a->dpy, a->surf);
    a->can_draw = 0;
    a->t_swap = of_now_ns();
    /* Behind by more than a source frame (a generated picture and its real frame may always wait): one refresh less, until the queue is short again. */
    a->cur_hold = q_len(a) >= 3 && it.hold > 1 ? it.hold - 1 : it.hold;
    a->cur_t = 0;
    struct shown *const sh = &a->ring[c & 255];
    sh->hold = a->cur_hold;
    sh->waiting = (int)q_len(a);
    sh->t_draw = a->t_swap;
    item_release(a, &it);
}

/* The compositor is ready for the next picture: the start of a cycle of its own, the phase every later picture is timed from. */
static void on_frame_callback(void *u)
{
    struct app *a = u;
    a->can_draw = 1;
    a->t_fc = of_now_ns();
}

static void on_presented(void *u, uint64_t c, uint64_t t, uint32_t refresh, uint64_t vblank, uint32_t flags)
{
    (void)flags, (void)vblank;
    struct app *a = u;
    const struct shown *e = &a->ring[c & 255];
    if (refresh) {
        a->refresh_ns = refresh;
        /* How deep the compositor is just now: from this picture's own way to the glass. */
        if (e->t_draw && t > e->t_draw)
            a->deep = t - e->t_draw > (uint64_t)refresh + refresh / 6;
    }
    if (c + 1 == a->commits)
        a->cur_t = t;
    if (a->trace)
        printf("  shown %llu %s, to hold %d, %d waiting behind it; drawn %.2f ms before its scanout (%s), %.2f refreshes after the one before\n", (unsigned long long)c, e->generated ? "generated" : "real", e->hold, e->waiting, (double)(t - e->t_draw) * 1e-6, a->deep ? "deep" : "shallow",
               a->last_t && refresh ? (double)(t - a->last_t) / (double)refresh : 0.0);
    if (e->generated)
        a->s.pres_g++;
    else
        a->s.pres_r++;
    if (a->last_t) {
        const uint64_t gap = t - a->last_t, slots = (gap + refresh / 2) / (refresh ? refresh : 1);
        a->s.cad[slots > 4 ? 4 : slots]++;
        a->s.gap_max = gap > a->s.gap_max ? gap : a->s.gap_max;
    }
    a->last_t = t;
    if (e->t_src && t > e->t_src) {
        const uint64_t lat = t - e->t_src;
        a->s.lat_sum += lat;
        a->s.lat_n++;
        a->s.lat_max = lat > a->s.lat_max ? lat : a->s.lat_max;
        a->s.lat_min = !a->s.lat_min || lat < a->s.lat_min ? lat : a->s.lat_min;
    }
}

static void on_discarded(void *u, uint64_t c)
{
    (void)c;
    ((struct app *)u)->s.disc++;
}

/* ---- the cast ---- */

/* The newest buffer of the cast, on this thread: is it a new picture; if so a copy, its luma, the flow. The buffer goes back to the compositor before this returns, the GPU having finished with it. */
static void ingest(struct app *a, struct pw_buffer *b)
{
    a->s.cast++;
    const struct spa_data *d = &b->buffer->datas[0];
    if (d->type != SPA_DATA_DmaBuf || !d->chunk) {
        fprintf(stderr, "ofgl: the cast gave plain memory (data type %u), not a dmabuf: this program keeps the pictures on the GPU and has no path for that\n", d->type);
        a->fatal = 1;
        return;
    }
    if (d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
        return;
    if (!a->prog_copy && gl_cast_programs(a)) {
        a->fatal = 1;
        return;
    }
    struct pwbuf *pb = b->user_data;
    if (!pb) {
        pb = import(a, b->buffer);
        if (!pb) {
            a->fatal = 1;
            return;
        }
        b->user_data = pb;
    }
    const uint64_t t = of_now_ns();
    const struct spa_meta_header *hd = spa_buffer_find_meta_data(b->buffer, SPA_META_Header, sizeof *hd);
    const uint64_t src_t = hd && hd->pts > 0 ? (uint64_t)hd->pts : t;
    int s = -1;
    for (int i = 0; i < FR_N && s < 0; i++)
        if (!a->fr_refs[i])
            s = i;
    if (s < 0) {
        a->s.noslot++;
        return;
    }
    const GLenum target = a->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    glBindFramebuffer(GL_FRAMEBUFFER, a->fr_fbo[s]);
    glViewport(0, 0, a->W, a->H);
    if (a->prev >= 0) {
        glUseProgram(a->prog_cmp);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, a->fr_tex[a->prev]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(target, pb->tex);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, a->query);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        GLuint any = 0;
        glGetQueryObjectuiv(a->query, GL_QUERY_RESULT, &any);
        if (!any) {
            /* Nothing changed on the monitor: nothing to make, nothing to show, nothing for the compositor to do. */
            a->s.dup++;
            return;
        }
    }
    glUseProgram(a->prog_copy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, pb->tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, a->luma_fbo);
    glViewport(0, 0, a->lw / 4, a->lh);
    glUseProgram(a->prog_luma);
    glBindTexture(GL_TEXTURE_2D, a->fr_tex[s]);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glPixelStorei(GL_PACK_ROW_LENGTH, a->lstride / 4);
    glReadPixels(0, 0, a->lw / 4, a->lh, GL_RGBA, GL_UNSIGNED_BYTE, a->probe ? a->probe_luma : a->eng.luma[s]);
    const uint64_t took = of_now_ns() - t;
    a->s.gpu_ns += took;
    a->s.gpu_n++;
    a->s.gpu_max = took > a->s.gpu_max ? took : a->s.gpu_max;
    /* A mid is worth making across a captured frame that did not change, not across a pause. */
    const int gap_ok = a->last_src_t && src_t - a->last_src_t <= 2 * a->interval_ns + a->interval_ns / 2;
    a->last_src_t = src_t;
    const int older = a->prev;
    a->prev = s;
    a->fr_refs[s]++;
    if (!a->probe)
        submit(a, s, older, gap_ok, src_t);
    fr_unref(a, older);
}

static void on_process(void *u)
{
    struct app *a = u;
    struct pw_buffer *b = NULL, *next;
    while ((next = pw_stream_dequeue_buffer(a->stream))) {
        if (b)
            pw_stream_queue_buffer(a->stream, b);
        b = next;
    }
    if (!b)
        return;
    if (!a->fatal && a->negotiated)
        ingest(a, b);
    pw_stream_queue_buffer(a->stream, b);
}

static void on_remove_buffer(void *u, struct pw_buffer *b)
{
    struct app *a = u;
    struct pwbuf *pb = b->user_data;
    if (!pb)
        return;
    glDeleteTextures(1, &pb->tex);
    eglDestroyImage(a->dpy, pb->img);
    free(pb);
    b->user_data = NULL;
}

static void on_param(void *u, uint32_t id, const struct spa_pod *param)
{
    struct app *a = u;
    if (!param || id != SPA_PARAM_Format)
        return;
    uint32_t mt, mst;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_format_video_raw_parse(param, &a->raw);
    a->dmabuf = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
    const int k = a->raw.format == SPA_VIDEO_FORMAT_BGRA;
    a->external = 0;
    for (int i = 0; i < a->nmods[k]; i++)
        if (a->mods[k][i] == a->raw.modifier)
            a->external = a->mod_ext[k][i];
    snprintf(a->format, sizeof a->format, "%s %ux%u, max %u/%u, %s, modifier %#llx%s", spa_debug_type_find_short_name(spa_type_video_format, a->raw.format), a->raw.size.width, a->raw.size.height, a->raw.max_framerate.num, a->raw.max_framerate.denom,
             a->dmabuf ? "dmabuf" : "plain memory", (unsigned long long)a->raw.modifier, a->external ? ", external texture" : "");
    a->negotiated = (int)a->raw.size.width == a->W && (int)a->raw.size.height == a->H;
    uint8_t mem[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(mem, sizeof mem);
    const struct spa_pod *out[2];
    out[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(6, 2, 16), SPA_PARAM_BUFFERS_dataType,
                                        SPA_POD_CHOICE_FLAGS_Int(a->dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    out[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(a->stream, out, 2);
}

static void on_state(void *u, enum pw_stream_state old, enum pw_stream_state state, const char *err)
{
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "ofgl: cast: %s\n", err ? err : "error");
        ((struct app *)u)->fatal = 1;
    }
}

static const struct pw_stream_events stream_ev = {.version = PW_VERSION_STREAM_EVENTS, .param_changed = on_param, .process = on_process, .remove_buffer = on_remove_buffer, .state_changed = on_state};

/* One format offer: with the modifiers EGL can import (the cast then comes as dmabufs), the compositor choosing among them. */
static const struct spa_pod *offer(struct spa_pod_builder *b, uint32_t format, const uint64_t *mods, int n, int w, int h, uint32_t num, uint32_t den)
{
    struct spa_pod_frame f[2];
    const struct spa_rectangle size = SPA_RECTANGLE((uint32_t)w, (uint32_t)h);
    const struct spa_fraction zero = SPA_FRACTION(0, 1), want = SPA_FRACTION(num, den), lo = SPA_FRACTION(1, 1);
    spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(b, (int64_t)mods[0]);
    for (int i = 0; i < n; i++)
        spa_pod_builder_long(b, (int64_t)mods[i]);
    spa_pod_builder_pop(b, &f[1]);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&zero), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&want, &lo, &want), 0);
    return spa_pod_builder_pop(b, &f[0]);
}

static int cast_open(struct app *a, uint32_t node, uint32_t num, uint32_t den)
{
    pw_init(NULL, NULL);
    a->loop = pw_loop_new(NULL);
    a->pctx = a->loop ? pw_context_new(a->loop, NULL, 0) : NULL;
    a->core = a->pctx ? pw_context_connect(a->pctx, NULL, 0) : NULL;
    if (!a->core) {
        fprintf(stderr, "ofgl: no PipeWire session\n");
        return -1;
    }
    a->stream = pw_stream_new(a->core, "ofgl", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL));
    pw_stream_add_listener(a->stream, &a->hook, &stream_ev, a);
    static uint8_t mem[8192];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(mem, sizeof mem);
    const struct spa_pod *params[2];
    uint32_t n = 0;
    static const uint32_t formats[2] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA};
    for (int k = 0; k < 2; k++)
        if (a->nmods[k])
            params[n++] = offer(&b, formats[k], a->mods[k], a->nmods[k], a->W, a->H, num, den);
    if (!n) {
        fprintf(stderr, "ofgl: EGL names no dmabuf modifier for XRGB8888 or ARGB8888\n");
        return -1;
    }
    const int rc = pw_stream_connect(a->stream, PW_DIRECTION_INPUT, node, PW_STREAM_FLAG_AUTOCONNECT, params, n);
    if (rc < 0) {
        fprintf(stderr, "ofgl: connect to node %u: %s\n", node, spa_strerror(rc));
        return -1;
    }
    pw_loop_enter(a->loop);
    return 0;
}

static void print_second(struct app *a, double secs)
{
    struct stats *s = &a->s;
    printf("%6.1f s cast %6.2f dup %6.2f noslot %llu | flow sent %6.2f made %6.2f scene %llu nomid %llu late %llu | commit g %llu r %llu skip %llu | glass %6.2f/s (g %6.2f r %6.2f) disc %llu cadence 1:%llu 2:%llu 3:%llu 4+:%llu gapmax %.1f ms | latency ms %.1f mean %.1f..%.1f | flow ms %.2f max %.2f wait %.2f moved %.1f | gpu ms %.2f max %.2f\n",
           secs, (double)s->cast, (double)s->dup, (unsigned long long)s->noslot, (double)s->sent, (double)s->made, (unsigned long long)s->scene, (unsigned long long)s->nomid, (unsigned long long)s->late, (unsigned long long)s->com_g, (unsigned long long)s->com_r, (unsigned long long)s->skip_g,
           (double)(s->pres_g + s->pres_r), (double)s->pres_g, (double)s->pres_r, (unsigned long long)s->disc, (unsigned long long)s->cad[1], (unsigned long long)s->cad[2], (unsigned long long)s->cad[3], (unsigned long long)s->cad[4], (double)s->gap_max * 1e-6,
           s->lat_n ? (double)s->lat_sum / (double)s->lat_n * 1e-6 : 0.0, (double)s->lat_min * 1e-6, (double)s->lat_max * 1e-6, s->flow_n ? (double)s->flow_sum / (double)s->flow_n * 1e-3 : 0.0, (double)s->flow_max * 1e-3, s->flow_n ? (double)s->wait_sum / (double)s->flow_n * 1e-3 : 0.0,
           s->flow_n ? (double)s->moved_sum / (double)s->flow_n : 0.0, s->gpu_n ? (double)s->gpu_ns / (double)s->gpu_n * 1e-6 : 0.0, (double)s->gpu_max * 1e-6);
    fflush(stdout);
    memset(s, 0, sizeof *s);
}

int main(int argc, char **argv)
{
    static struct app a;
    uint32_t node = 0;
    int threads = 0, enough = -1, seconds = 0, no_excl = 0, plan = 0;
    const char *output = NULL;
    a.div = 2;
    a.prev = -1;
    a.can_draw = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--node") && i + 1 < argc)
            node = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)
            a.rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--div") && i + 1 < argc)
            a.div = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--enough") && i + 1 < argc)
            enough = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pass"))
            a.pass_only = 1;
        else if (!strcmp(argv[i], "--probe"))
            a.probe = 1;
        else if (!strcmp(argv[i], "--trace"))
            a.trace = 1;
        else if (!strcmp(argv[i], "--no-excl"))
            no_excl = 1;
        else if (!strcmp(argv[i], "--plan"))
            plan = 1;
        else {
            fprintf(stderr, "usage: ofgl --output NAME [--node N: a cast made elsewhere; without it the cast is asked of KWin] [--rate N: pictures a second in, twice that out (24, 30, ...); without it half the refresh in, the refresh out] [--div D: the field is the picture / D, 2 to 4]\n"
                            "            [--threads T: the flow's threads, 1 to %d, one a core; this program's own thread is one more] [--enough Q6] [--pass: no flow, real frames only] [--probe: the cast and the GPU passes alone, nothing shown] [--seconds S]\n"
                            "       ofgl --output NAME [--div D] --plan: the output's pixels and mHz, the luma's size, and no run\n", OF_MAX_THREADS - 1);
            return 2;
        }
    }
    /* The law of the 8 cores, before any library has made a thread: every thread this process will ever own inherits these CPUs. */
    int cpus[OF_MAX_THREADS];
    const int cores = of_cpus_pick(cpus, OF_MAX_THREADS);
    if (of_cpus_confine(cpus, cores))
        perror("ofgl: affinity");
    if (!output || a.div < 2 || a.div > 4 || a.rate < 0 || wlp_output_mode(output, &a.W, &a.H, &a.mhz)) {
        fprintf(stderr, "ofgl: --output must name a monitor, --div is 2 to 4, --rate a number of pictures a second\n");
        return 2;
    }
    a.lw = ((a.W + a.div - 1) / a.div + 15) & ~15;
    a.lh = (a.H + a.div - 1) / a.div;
    a.lstride = of_luma_stride(a.lw);
    a.bw = of_field_w(a.lw);
    a.bh = of_field_w(a.lh);
    if (plan) {
        printf("%d %d %d %d %d\n", a.W, a.H, a.mhz, a.lw, a.lh);
        return 0;
    }
    if (a.rate && (uint64_t)a.rate * 2000u > (uint64_t)a.mhz) {
        fprintf(stderr, "ofgl: %d in is %d out, more than the monitor's %d.%03d Hz shows\n", a.rate, 2 * a.rate, a.mhz / 1000, a.mhz % 1000);
        return 2;
    }
    if (!of_cpu_ok()) {
        fprintf(stderr, "ofgl: this CPU has no AVX2 or no FMA3\n");
        return 1;
    }
    /* This thread is one of the 8: the flow gets what is left, at most 7, each a core of its own; with one core they share it. */
    const int most = cores > 1 ? cores - 1 : 1;
    if (threads < 1 || threads > most)
        threads = most;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    const int with_flow = !a.probe && !a.pass_only;
    if (a.probe)
        a.probe_luma = malloc((size_t)a.lstride * (size_t)a.lh);
    const struct wlp_cb cb = {&a, on_presented, on_discarded, NULL};
    a.w = wlp_open(output, a.W, a.H, 0, &cb);
    if (!a.w)
        return 1;
    wlp_on_frame(a.w, on_frame_callback, &a);
    if (gl_open(&a))
        return 1;
    if (with_flow && of_engine_open(&a.eng, a.lw, a.lh, cpus, threads, enough))
        return 1;
    /* The cast's cap and what a source frame is worth in refreshes, as a fraction: num / den pictures a second in. */
    const uint32_t cast_num = a.rate ? (uint32_t)a.rate * 1000u : (uint32_t)a.mhz, cast_den = a.rate ? 1000u : 2000u;
    a.interval_ns = 1000000000ull * cast_den / cast_num;
    a.refresh_ns = 1000000000000ull / (uint64_t)a.mhz;
    a.pair_num = (uint64_t)a.mhz * cast_den;
    a.pair_den = (uint64_t)cast_num * 1000u;
    int lw, lh;
    wlp_size(a.w, &lw, &lh);
    printf("overlay on %s: %d x %d logical, %d x %d buffer, refresh %d mHz; cast capped at %u/%u a second, twice that on the glass, a source frame worth %.3f refreshes (the generated picture the smaller half); the field is the picture / %d: luma %d x %d, %d x %d vectors\n",
           output, lw, lh, a.W, a.H, a.mhz, cast_num, cast_den, (double)a.pair_num / (double)a.pair_den, a.div, a.lw, a.lh, a.bw, a.bh);
    printf("holds: %s\n", a.rate ? "a picture's refreshes are counted from the frame callback while KWin is deep, from its own scanout while shallow" : "none: one picture a refresh, by the frame callback");
    int rc = 0, excluded = 0;
    if (!no_excl && !a.probe) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "scripts/excl.sh on %d >/dev/null", (int)getpid());
        if (system(cmd)) {
            fprintf(stderr, "ofgl: the capture exclusion failed: refusing to run a feedback loop\n");
            rc = 1;
        }
        excluded = !rc;
    }
    /* No node given: the cast is asked of the compositor on the overlay's own connection. */
    if (!rc && !node && wlp_cast_open(a.w, &node))
        rc = 1;
    if (!rc)
        printf("cast: PipeWire node %u\n", node);
    if (!rc && cast_open(&a, node, cast_num, cast_den))
        rc = 1;
    wlp_flush(a.w);
    /* Everything that makes threads has been opened: this thread to its own core, the one the flow does not use. */
    of_cpus_pin(cpus[cores - 1]);
    printf("threads: the flow %d on cpus", with_flow ? threads : 0);
    for (int i = 0; with_flow && i < threads; i++)
        printf(" %d", cpus[i]);
    printf(", this thread on cpu %d; %d in the process, the law is %d on %d cores; flow levels %d\n", cpus[cores - 1], of_threads_now(), OF_MAX_THREADS, cores, with_flow ? a.eng.levels : 0);
    const uint64_t t_start = of_now_ns();
    uint64_t t_print = t_start + 1000000000ull;
    int printed_format = 0;
    while (!rc && !stop && !a.fatal) {
        struct pollfd q[3] = {{wlp_fd(a.w), POLLIN, 0}, {pw_loop_get_fd(a.loop), POLLIN, 0}, {with_flow ? a.eng.event_fd : -1, POLLIN, 0}};
        /* Asleep until the compositor, the cast or the flow has something, or the picture on the glass has been held long enough. */
        uint64_t nap = 5000000ull;
        const uint64_t due = hold_due(&a), now0 = of_now_ns();
        if (a.q_head != a.q_tail && due)
            nap = due > now0 ? (due - now0 < nap ? due - now0 : nap) : 0;
        const struct timespec ts = {0, (long)nap};
        ppoll(q, 3, &ts, NULL);
        if (q[2].revents & POLLIN) {
            uint64_t n;
            if (read(a.eng.event_fd, &n, sizeof n) < 0)
                n = 0;
        }
        if (wlp_cast_closed(a.w)) {
            fprintf(stderr, "ofgl: the compositor ended the cast\n");
            break;
        }
        if (wlp_read(a.w) < 0) {
            fprintf(stderr, "ofgl: the compositor closed the overlay\n");
            break;
        }
        pw_loop_iterate(a.loop, 0);
        if (with_flow) {
            take_done(&a);
            take_late(&a);
        }
        draw_next(&a);
        if (wlp_flush(a.w) < 0)
            break;
        const uint64_t now = of_now_ns();
        if (now >= t_print) {
            if (!printed_format && a.format[0]) {
                printf("cast: %s\n", a.format);
                printed_format = 1;
            }
            print_second(&a, (double)(now - t_start) * 1e-9);
            t_print += 1000000000ull;
            if (seconds && now - t_start >= (uint64_t)seconds * 1000000000ull)
                break;
        }
    }
    if (a.stream) {
        pw_stream_disconnect(a.stream);
        pw_stream_destroy(a.stream);
    }
    if (a.loop && a.core) {
        pw_loop_leave(a.loop);
        pw_core_disconnect(a.core);
        pw_context_destroy(a.pctx);
        pw_loop_destroy(a.loop);
    }
    if (excluded && system("scripts/excl.sh off >/dev/null"))
        fprintf(stderr, "ofgl: excl.sh off failed\n");
    if (with_flow)
        of_engine_close(&a.eng);
    eglMakeCurrent(a.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(a.dpy, a.surf);
    wl_egl_window_destroy(a.win);
    eglDestroyContext(a.dpy, a.ctx);
    wlp_close(a.w);
    printf("ofgl: done\n");
    return rc || a.fatal;
}
