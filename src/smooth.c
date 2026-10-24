// smooth.c - LD_PRELOAD shim for Undertale's GameMaker 1.4 Linux runner (32-bit).
// Game logic keeps running at its own 30 ticks/s. Every real frame's GL command stream is
// recorded; each sprite quad is classified as world-static, screen-static (UI) or moving (matched
// to the same sprite in the previous frame), and while the runner busy-waits for its next tick the
// last frame is replayed with everything moved along its motion, up to SMOOTH_FPS presents per
// second, aligned to the display's hardware vblank.
// Env: SMOOTH_FPS (cap, default 144; 0 = off), SMOOTH_MODE (interp | extrap),
//      SMOOTH_VSYNC (1 = align to hw vblank via DRM), SMOOTH_LOG (stats file),
//      SMOOTH_CAPTURE=1 (F9 saves the last 48 presented frames next to the log),
//      SMOOTH_DEBUG=1 (per-present timing in the log), SMOOTH_SELFTEST=1 (replay must match the
//      runner's own frame byte for byte), SMOOTH_DUMP + SMOOTH_DUMP_TRIGGER (window dumps).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/keysym.h>

/* ---------- real functions ---------- */

#define GLFUNCS(X) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform2iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform3iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glClearDepth, (GLclampd)) \
    X(void, glClearStencil, (GLint)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthRange, (GLclampd, GLclampd)) \
    X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
    X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
    X(void, glStencilMask, (GLuint)) \
    X(void, glFrontFace, (GLenum)) \
    X(void, glShadeModel, (GLenum)) \
    X(void, glHint, (GLenum, GLenum)) \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glDrawBuffer, (GLenum)) \
    X(void, glDrawBuffers, (GLsizei, const GLenum *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(GLenum, glGetError, (void)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glXSwapBuffers, (Display *, GLXDrawable)) \
    X(Bool, glXMakeCurrent, (Display *, GLXDrawable, GLXContext)) \
    X(__GLXextFuncPtr, glXGetProcAddressARB, (const GLubyte *)) \
    X(const char *, glXQueryExtensionsString, (Display *, int))

#define DECL(ret, name, args) static ret(*r_##name) args;
GLFUNCS(DECL)
static int (*r_clock_gettime)(clockid_t, struct timespec *);


static volatile int resolved;

// other preloaded libraries (Steam's overlay) may call into our hooks before our constructor runs
__attribute__((constructor)) static void resolve(void)
{
    if (resolved) return;
#define RES(ret, name, args) r_##name = (ret(*) args)dlsym(RTLD_NEXT, #name);
    GLFUNCS(RES)
    r_clock_gettime = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
    resolved = 1;
}
#define RESOLVE() do { if (__builtin_expect(!resolved, 0)) resolve(); } while (0)

/* ---------- config / logging ---------- */

static int cfg_on = 1, cfg_extrap, cfg_vsync = 1, cfg_selftest, cap_on;
static double cfg_min_interval = 1.0 / 144;
static FILE *slog;
static const char *dump_dir, *dump_trigger;
static int dump_left;

static void config(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *s;
    if ((s = getenv("SMOOTH_FPS"))) {
        double fps = atof(s);
        if (fps <= 0) cfg_on = 0;
        else cfg_min_interval = 1.0 / fps;
    }
    if ((s = getenv("SMOOTH_MODE")) && !strcmp(s, "extrap")) cfg_extrap = 1;
    if ((s = getenv("SMOOTH_VSYNC"))) cfg_vsync = atoi(s);
    if ((s = getenv("SMOOTH_SELFTEST"))) cfg_selftest = atoi(s);
    if ((s = getenv("SMOOTH_CAPTURE"))) cap_on = atoi(s);
    if ((s = getenv("SMOOTH_LOG"))) slog = fopen(s, "w");
    dump_dir = getenv("SMOOTH_DUMP");
    dump_trigger = getenv("SMOOTH_DUMP_TRIGGER");
}

#define LOG(...) do { if (slog) { fprintf(slog, __VA_ARGS__); fputc('\n', slog); } } while (0)

static double mono(void)
{
    struct timespec ts;
    r_clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---------- shadow GL state ---------- */

#define MAXUNIT 8
#define MAXATTR 8
#define MAXCAP 32
#define MAXPROG 256
#define MAXTEX 8192
#define MAXFBO 1024

typedef struct {
    unsigned char on, set;
    GLint size;
    GLenum type;
    GLboolean norm;
    GLsizei stride;
    const void *ptr;
    GLuint buf;
} Attr;

typedef struct {
    GLuint tex[MAXUNIT];
    unsigned char texset[MAXUNIT];
    GLuint active, prog, fbo, arraybuf;
    GLint vp[4], sc[4];
    unsigned char vpset, scset;
    GLenum caps[MAXCAP];
    unsigned char capon[MAXCAP];
    int ncap;
    GLenum blend[2];
    unsigned char blendset;
    GLfloat clear[4];
    GLclampd cleardepth;
    GLint clearstencil;
    GLboolean cmask[4], dmask;
    GLenum dfunc;
    GLclampd drange[2];
    GLenum sfunc, sop[3];
    GLint sref;
    GLuint sfmask, smask;
    GLenum frontface, shademodel;
    unsigned char setbits; // which of the "misc" groups above were ever set
    Attr attr[MAXATTR];
} State;

enum { SB_CLEAR = 1, SB_CMASK = 2, SB_DEPTH = 4, SB_STENCIL = 8, SB_FACE = 16, SB_SHADE = 32, SB_CDEPTH = 64, SB_CSTENCIL = 128 };
static unsigned char sb_extra; // SB for drange/dfunc/dmask share SB_DEPTH; stencil func/op/mask share SB_STENCIL

static State st; // current shadow (what the runner believes is bound)

static void cap_set(State *s, GLenum c, int on)
{
    for (int i = 0; i < s->ncap; i++)
        if (s->caps[i] == c) { s->capon[i] = on; return; }
    if (s->ncap < MAXCAP) { s->caps[s->ncap] = c; s->capon[s->ncap++] = on; }
}

/* uniforms: value store per (program, location) */
enum { U1F, U1I, U1FV, U2FV, U3FV, U4FV, U1IV, U2IV, U3IV, U4IV, UM2, UM3, UM4 };
static const int ukind_elems[] = { 1, 1, 1, 2, 3, 4, 1, 2, 3, 4, 4, 9, 16 };

typedef struct {
    GLuint prog;
    GLint loc;
    int kind;
    GLsizei count;
    GLboolean transpose;
    int bytes;
    unsigned char *data;
} Uni;

typedef struct { Uni *u; int n, cap; } UniSet;
static UniSet unis;

static Uni *uni_get(UniSet *set, GLuint prog, GLint loc, int create)
{
    for (int i = 0; i < set->n; i++)
        if (set->u[i].prog == prog && set->u[i].loc == loc) return &set->u[i];
    if (!create) return NULL;
    if (set->n == set->cap) {
        set->cap = set->cap ? set->cap * 2 : 64;
        set->u = realloc(set->u, set->cap * sizeof(Uni));
    }
    Uni *u = &set->u[set->n++];
    memset(u, 0, sizeof *u);
    u->prog = prog;
    u->loc = loc;
    return u;
}

static void uni_store(UniSet *set, GLuint prog, GLint loc, int kind, GLsizei count, GLboolean tr, const void *data)
{
    if (loc < 0) return;
    Uni *u = uni_get(set, prog, loc, 1);
    int bytes = ukind_elems[kind] * 4 * (count > 0 ? count : 1);
    if (u->bytes != bytes) { u->data = realloc(u->data, bytes); u->bytes = bytes; }
    memcpy(u->data, data, bytes);
    u->kind = kind;
    u->count = count;
    u->transpose = tr;
}

static void uniset_copy(UniSet *dst, const UniSet *src)
{
    for (int i = 0; i < src->n; i++) {
        const Uni *s = &src->u[i];
        uni_store(dst, s->prog, s->loc, s->kind, s->count, s->transpose, s->data);
    }
}

static void uni_apply(const Uni *u)
{
    const GLfloat *f = (const GLfloat *)u->data;
    const GLint *iv = (const GLint *)u->data;
    switch (u->kind) {
    case U1F: r_glUniform1f(u->loc, f[0]); break;
    case U1I: r_glUniform1i(u->loc, iv[0]); break;
    case U1FV: r_glUniform1fv(u->loc, u->count, f); break;
    case U2FV: r_glUniform2fv(u->loc, u->count, f); break;
    case U3FV: r_glUniform3fv(u->loc, u->count, f); break;
    case U4FV: r_glUniform4fv(u->loc, u->count, f); break;
    case U1IV: r_glUniform1iv(u->loc, u->count, iv); break;
    case U2IV: r_glUniform2iv(u->loc, u->count, iv); break;
    case U3IV: r_glUniform3iv(u->loc, u->count, iv); break;
    case U4IV: r_glUniform4iv(u->loc, u->count, iv); break;
    case UM2: r_glUniformMatrix2fv(u->loc, u->count, u->transpose, f); break;
    case UM3: r_glUniformMatrix3fv(u->loc, u->count, u->transpose, f); break;
    case UM4: r_glUniformMatrix4fv(u->loc, u->count, u->transpose, f); break;
    }
}

static void uniset_apply(const UniSet *set)
{
    GLuint cur = (GLuint)-1;
    for (int i = 0; i < set->n; i++) {
        if (set->u[i].prog != cur) { cur = set->u[i].prog; r_glUseProgram(cur); }
        uni_apply(&set->u[i]);
    }
}

/* per-program attribute / uniform locations, per-texture and per-fbo info */
typedef struct { GLint pos, uv, mats; unsigned char known_pos, known_uv, known_mats; } ProgInfo;
static ProgInfo proginfo[MAXPROG];
typedef struct { GLsizei w, h; GLint ifmt; GLfloat minf, magf, ws, wt; unsigned char hasparam; } TexInfo;
static TexInfo texinfo[MAXTEX];
static GLuint fbotex[MAXFBO];

/* ---------- frame recording ---------- */

enum {
    C_ACTIVETEX = 1, C_BINDTEX, C_USEPROG, C_UNIFORM, C_ENABLE, C_DISABLE, C_BLEND, C_VIEWPORT, C_SCISSOR,
    C_CLEARCOLOR, C_CLEAR, C_CLEARDEPTH, C_CLEARSTENCIL, C_COLORMASK, C_DEPTHMASK, C_DEPTHFUNC, C_DEPTHRANGE,
    C_STENCILFUNC, C_STENCILOP, C_STENCILMASK, C_FRONTFACE, C_SHADEMODEL, C_HINT, C_TEXPARAMF, C_BINDFBO,
    C_DRAWBUFFER, C_DRAWBUFFERS, C_DRAW
};

typedef struct {
    int op;
    GLint a[4];
    GLfloat f[4];
    GLdouble d[2];
    int off, len;
} Cmd;

typedef struct {
    GLuint idx;
    GLint size;
    GLenum type;
    GLboolean norm;
    int esize, off;
} DAttr;

typedef struct {
    GLenum mode;
    GLsizei count;
    int nattr, pos, uv;
    DAttr at[MAXATTR];
    GLuint tex, fbo, prog;
    GLint vp[4];
    float mvp[16];
    float inv[4]; // inverse of the 2x2 linear part: NDC delta -> vertex delta
    unsigned char has_mvp, has_inv;
} Draw;

enum { Q_WORLD, Q_SCREEN, Q_MOVING };

typedef struct {
    int draw, v0, nv;
    float sx, sy, w, h, vx, vy, mx, my;
    uint32_t key;
    GLuint tex, fbo;
    unsigned char main, group, matched, kind; // kind: Q_WORLD / Q_SCREEN / Q_MOVING
} Quad;

typedef struct {
    Cmd *cmd;
    int ncmd, ccmd;
    unsigned char *data;
    int ndata, cdata;
    Draw *draw;
    int ndraw, cdraw;
    Quad *q;
    int nq, cq;
    double L, Lprev; // tick time of this frame and of the frame its motion was measured against
    State s0, s1;
    UniSet u0, u1;
    int invalid, started, nmatched, nfallback;
    GLuint app_fbo, app_tex;
    float main_mvp[16];
    GLint main_vp[4];
    int has_main;
    float cam_mx, cam_my; // screen motion of world-static content since the previous frame
    int has_cam;
} Frame;

static Frame frames[3];
// cur: being recorded, prv: newest complete frame, old: the one before it
static Frame *cur = &frames[0], *prv = &frames[1], *old = &frames[2];
static int prv_ok, old_ok; // complete and replayable
static GLuint app_fbo, app_tex; // identified application surface
static pthread_t main_thread;
static int have_main;
static int in_replay;
// Steam's overlay draws with its own context on its own thread: only the runner's context is tracked
static GLXContext runner_ctx;
static __thread GLXContext tls_ctx;
#define MINE() (tls_ctx != NULL && tls_ctx == runner_ctx)

#define ENSURE(arr, n, cap, extra) do { if ((n) + (extra) > (cap)) { (cap) = ((n) + (extra)) * 2; (arr) = realloc((arr), (cap) * sizeof(*(arr))); } } while (0)

static int arena_put(Frame *f, const void *src, int len)
{
    ENSURE(f->data, f->ndata, f->cdata, len + 16);
    int off = (f->ndata + 15) & ~15;
    if (off + len > f->cdata) { f->cdata = (off + len) * 2; f->data = realloc(f->data, f->cdata); }
    if (src) memcpy(f->data + off, src, len);
    f->ndata = off + len;
    return off;
}

static int recording(void)
{
    if (!cfg_on || in_replay || !have_main || !MINE()) return 0;
    return 1;
}

static void frame_begin(void);

static Cmd *rec(int op)
{
    if (!cur->started) frame_begin();
    if (cur->ncmd > 200000) { cur->invalid = 1; return NULL; }
    ENSURE(cur->cmd, cur->ncmd, cur->ccmd, 1);
    Cmd *c = &cur->cmd[cur->ncmd++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

/* ---------- timing state ---------- */

static volatile int waiting;      // runner is between its swap and its next frame
static double swap_ret;           // when the runner's swap returned
static double last_wait_seen;     // last time we saw the runner's wait loop
static double frame_start_est;    // F_n: end of the runner's previous wait
static double period_est = 1.0 / 30;
static double work_est = 0.005;   // runner frame work (logic + draw + our present)
static double ghost_cost = 0.002;
static double last_present;       // time of the last present call
static double last_alpha = -1;
static double next_check;
static Display *g_dpy;
static GLXDrawable g_draw;
static long stat_ghosts, stat_real, stat_skipped_deadline, stat_hist[40], stat_npres;
static double stat_t0;

static double last_V;  // vblank the last present was aimed at
static double backoff_until, backoff_len;
// Xwayland hands a pending frame to the compositor once per refresh, at a phase the compositor picks;
// a swap issued before the previous frame was taken blocks until that moment, which reveals the phase
static double cb_phase = 0.0005; // seconds after a vblank; flips complete right at the vblank
static long stat_blocks;
static double block_t[8];        // recent blocked swaps / late runner ticks
static int block_n, late_n;
static double late_t[4];
static int ghost_overran;

static void frame_begin(void)
{
    // the runner's first GL call of a new frame
    cur->started = 1;
    cur->L = mono();
    if (waiting) {
        double F = last_wait_seen > swap_ret ? last_wait_seen : cur->L;
        if (frame_start_est > 0) {
            double p = F - frame_start_est;
            if (ghost_overran && p > period_est + 0.0015) {
                // one of our presents ran past the runner's tick: back off if that keeps happening
                late_t[late_n++ & 3] = F;
                if (late_n >= 3 && F - late_t[(late_n - 3) & 3] < 2.0) { backoff_len = 1; backoff_until = F + 1; late_n = 0; LOG("presents made the game late, plain presents for 1 s"); }
            }
            ghost_overran = 0;
            if (p > 0.02 && p < 0.06) period_est += (p - period_est) * 0.1;
        }
        frame_start_est = F;
        waiting = 0;
    } else if (frame_start_est <= 0) {
        frame_start_est = cur->L;
    }
}

/* ---------- hooks: state ---------- */

#define HOOK_PRE if (recording())

void glActiveTexture(GLenum u)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_ACTIVETEX); if (c) c->a[0] = u; }
    if (!in_replay && MINE()) st.active = u - GL_TEXTURE0;
    r_glActiveTexture(u);
}

void glBindTexture(GLenum t, GLuint id)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_BINDTEX); if (c) { c->a[0] = t; c->a[1] = id; } }
    if (!in_replay && MINE() && t == GL_TEXTURE_2D && st.active < MAXUNIT) { st.tex[st.active] = id; st.texset[st.active] = 1; }
    r_glBindTexture(t, id);
}

void glUseProgram(GLuint p)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_USEPROG); if (c) c->a[0] = p; }
    if (!in_replay && MINE()) st.prog = p;
    r_glUseProgram(p);
}

static void uni_hook(int kind, GLint loc, GLsizei count, GLboolean tr, const void *data)
{
    if (in_replay || !MINE()) return;
    int n = count > 0 ? count : 1;
    if (recording()) {
        Cmd *c = rec(C_UNIFORM);
        if (c) {
            c->a[0] = kind; c->a[1] = loc; c->a[2] = count; c->a[3] = tr;
            c->len = ukind_elems[kind] * 4 * n;
            c->off = arena_put(cur, data, c->len);
        }
    }
    uni_store(&unis, st.prog, loc, kind, count, tr, data);
}

void glUniform1f(GLint l, GLfloat v) { RESOLVE(); uni_hook(U1F, l, 1, 0, &v); r_glUniform1f(l, v); }
void glUniform1i(GLint l, GLint v) { RESOLVE(); uni_hook(U1I, l, 1, 0, &v); r_glUniform1i(l, v); }
void glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { RESOLVE(); uni_hook(U1FV, l, n, 0, v); r_glUniform1fv(l, n, v); }
void glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { RESOLVE(); uni_hook(U2FV, l, n, 0, v); r_glUniform2fv(l, n, v); }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { RESOLVE(); uni_hook(U3FV, l, n, 0, v); r_glUniform3fv(l, n, v); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { RESOLVE(); uni_hook(U4FV, l, n, 0, v); r_glUniform4fv(l, n, v); }
void glUniform1iv(GLint l, GLsizei n, const GLint *v) { RESOLVE(); uni_hook(U1IV, l, n, 0, v); r_glUniform1iv(l, n, v); }
void glUniform2iv(GLint l, GLsizei n, const GLint *v) { RESOLVE(); uni_hook(U2IV, l, n, 0, v); r_glUniform2iv(l, n, v); }
void glUniform3iv(GLint l, GLsizei n, const GLint *v) { RESOLVE(); uni_hook(U3IV, l, n, 0, v); r_glUniform3iv(l, n, v); }
void glUniform4iv(GLint l, GLsizei n, const GLint *v) { RESOLVE(); uni_hook(U4IV, l, n, 0, v); r_glUniform4iv(l, n, v); }
void glUniformMatrix2fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { RESOLVE(); uni_hook(UM2, l, n, t, v); r_glUniformMatrix2fv(l, n, t, v); }
void glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { RESOLVE(); uni_hook(UM3, l, n, t, v); r_glUniformMatrix3fv(l, n, t, v); }
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { RESOLVE(); uni_hook(UM4, l, n, t, v); r_glUniformMatrix4fv(l, n, t, v); }

void glEnable(GLenum c)
{
    RESOLVE();
    HOOK_PRE { Cmd *k = rec(C_ENABLE); if (k) k->a[0] = c; }
    if (!in_replay && MINE()) cap_set(&st, c, 1);
    r_glEnable(c);
}

void glDisable(GLenum c)
{
    RESOLVE();
    HOOK_PRE { Cmd *k = rec(C_DISABLE); if (k) k->a[0] = c; }
    if (!in_replay && MINE()) cap_set(&st, c, 0);
    r_glDisable(c);
}

void glBlendFunc(GLenum s, GLenum d)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_BLEND); if (c) { c->a[0] = s; c->a[1] = d; } }
    if (!in_replay && MINE()) { st.blend[0] = s; st.blend[1] = d; st.blendset = 1; }
    r_glBlendFunc(s, d);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_VIEWPORT); if (c) { c->a[0] = x; c->a[1] = y; c->a[2] = w; c->a[3] = h; } }
    if (!in_replay && MINE()) { st.vp[0] = x; st.vp[1] = y; st.vp[2] = w; st.vp[3] = h; st.vpset = 1; }
    r_glViewport(x, y, w, h);
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_SCISSOR); if (c) { c->a[0] = x; c->a[1] = y; c->a[2] = w; c->a[3] = h; } }
    if (!in_replay && MINE()) { st.sc[0] = x; st.sc[1] = y; st.sc[2] = w; st.sc[3] = h; st.scset = 1; }
    r_glScissor(x, y, w, h);
}

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_CLEARCOLOR); if (c) { c->f[0] = r; c->f[1] = g; c->f[2] = b; c->f[3] = a; } }
    if (!in_replay && MINE()) { st.clear[0] = r; st.clear[1] = g; st.clear[2] = b; st.clear[3] = a; st.setbits |= SB_CLEAR; }
    r_glClearColor(r, g, b, a);
}

void glClear(GLbitfield m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_CLEAR); if (c) c->a[0] = m; }
    r_glClear(m);
}

void glClearDepth(GLclampd d)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_CLEARDEPTH); if (c) c->d[0] = d; }
    if (!in_replay && MINE()) { st.cleardepth = d; st.setbits |= SB_CDEPTH; }
    r_glClearDepth(d);
}

void glClearStencil(GLint s)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_CLEARSTENCIL); if (c) c->a[0] = s; }
    if (!in_replay && MINE()) { st.clearstencil = s; st.setbits |= SB_CSTENCIL; }
    r_glClearStencil(s);
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_COLORMASK); if (c) { c->a[0] = r; c->a[1] = g; c->a[2] = b; c->a[3] = a; } }
    if (!in_replay && MINE()) { st.cmask[0] = r; st.cmask[1] = g; st.cmask[2] = b; st.cmask[3] = a; st.setbits |= SB_CMASK; }
    r_glColorMask(r, g, b, a);
}

void glDepthMask(GLboolean f)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_DEPTHMASK); if (c) c->a[0] = f; }
    if (!in_replay && MINE()) { st.dmask = f; sb_extra |= 1; }
    r_glDepthMask(f);
}

void glDepthFunc(GLenum f)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_DEPTHFUNC); if (c) c->a[0] = f; }
    if (!in_replay && MINE()) { st.dfunc = f; sb_extra |= 2; }
    r_glDepthFunc(f);
}

void glDepthRange(GLclampd n, GLclampd f)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_DEPTHRANGE); if (c) { c->d[0] = n; c->d[1] = f; } }
    if (!in_replay && MINE()) { st.drange[0] = n; st.drange[1] = f; sb_extra |= 4; }
    r_glDepthRange(n, f);
}

void glStencilFunc(GLenum f, GLint ref, GLuint m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_STENCILFUNC); if (c) { c->a[0] = f; c->a[1] = ref; c->a[2] = (GLint)m; } }
    if (!in_replay && MINE()) { st.sfunc = f; st.sref = ref; st.sfmask = m; sb_extra |= 8; }
    r_glStencilFunc(f, ref, m);
}

void glStencilOp(GLenum a, GLenum b, GLenum c2)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_STENCILOP); if (c) { c->a[0] = a; c->a[1] = b; c->a[2] = c2; } }
    if (!in_replay && MINE()) { st.sop[0] = a; st.sop[1] = b; st.sop[2] = c2; sb_extra |= 16; }
    r_glStencilOp(a, b, c2);
}

void glStencilMask(GLuint m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_STENCILMASK); if (c) c->a[0] = (GLint)m; }
    if (!in_replay && MINE()) { st.smask = m; sb_extra |= 32; }
    r_glStencilMask(m);
}

void glFrontFace(GLenum m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_FRONTFACE); if (c) c->a[0] = m; }
    if (!in_replay && MINE()) { st.frontface = m; st.setbits |= SB_FACE; }
    r_glFrontFace(m);
}

void glShadeModel(GLenum m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_SHADEMODEL); if (c) c->a[0] = m; }
    if (!in_replay && MINE()) { st.shademodel = m; st.setbits |= SB_SHADE; }
    r_glShadeModel(m);
}

void glHint(GLenum t, GLenum m)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_HINT); if (c) { c->a[0] = t; c->a[1] = m; } }
    r_glHint(t, m);
}

void glTexParameterf(GLenum t, GLenum p, GLfloat v)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_TEXPARAMF); if (c) { c->a[0] = t; c->a[1] = p; c->f[0] = v; } }
    if (!in_replay && MINE() && t == GL_TEXTURE_2D && st.active < MAXUNIT) {
        GLuint id = st.tex[st.active];
        if (id < MAXTEX) {
            TexInfo *ti = &texinfo[id];
            if (p == GL_TEXTURE_MIN_FILTER) ti->minf = v;
            else if (p == GL_TEXTURE_MAG_FILTER) ti->magf = v;
            else if (p == GL_TEXTURE_WRAP_S) ti->ws = v;
            else if (p == GL_TEXTURE_WRAP_T) ti->wt = v;
            ti->hasparam = 1;
        }
    }
    r_glTexParameterf(t, p, v);
}

void glBindFramebuffer(GLenum t, GLuint f)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_BINDFBO); if (c) { c->a[0] = t; c->a[1] = f; } }
    if (!in_replay && MINE()) st.fbo = f;
    r_glBindFramebuffer(t, f);
}

void glDrawBuffer(GLenum b)
{
    RESOLVE();
    HOOK_PRE { Cmd *c = rec(C_DRAWBUFFER); if (c) c->a[0] = b; }
    r_glDrawBuffer(b);
}

void glDrawBuffers(GLsizei n, const GLenum *b)
{
    RESOLVE();
    HOOK_PRE {
        Cmd *c = rec(C_DRAWBUFFERS);
        if (c) { c->a[0] = n; c->len = n * sizeof(GLenum); c->off = arena_put(cur, b, c->len); }
    }
    r_glDrawBuffers(n, b);
}

void glEnableVertexAttribArray(GLuint i)
{
    RESOLVE();
    if (!in_replay && MINE() && i < MAXATTR) st.attr[i].on = 1;
    r_glEnableVertexAttribArray(i);
}

void glDisableVertexAttribArray(GLuint i)
{
    RESOLVE();
    if (!in_replay && MINE() && i < MAXATTR) st.attr[i].on = 0;
    r_glDisableVertexAttribArray(i);
}

void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
    RESOLVE();
    if (!in_replay && MINE() && i < MAXATTR) {
        Attr *a = &st.attr[i];
        a->set = 1; a->size = size; a->type = type; a->norm = norm; a->stride = stride; a->ptr = p; a->buf = st.arraybuf;
    }
    r_glVertexAttribPointer(i, size, type, norm, stride, p);
}

void glBindBuffer(GLenum t, GLuint b)
{
    RESOLVE();
    if (!in_replay && MINE() && t == GL_ARRAY_BUFFER) st.arraybuf = b;
    r_glBindBuffer(t, b);
}

static int type_size(GLenum t)
{
    switch (t) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: return 4;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    }
    return 0;
}

static int find_mats(GLuint prog, float out[16])
{
    if (prog >= MAXPROG || !proginfo[prog].known_mats) return 0;
    GLint loc = proginfo[prog].mats;
    Uni *u = uni_get(&unis, prog, loc, 0);
    if (u && u->kind == UM4 && u->count >= 5) { memcpy(out, u->data + 4 * 64, 64); return !u->transpose; }
    u = uni_get(&unis, prog, loc + 4, 0); // separately set gm_Matrices[4]
    if (u && u->kind == UM4 && u->count >= 1) { memcpy(out, u->data, 64); return !u->transpose; }
    return 0;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    RESOLVE();
    if (recording() && count > 0) {
        Cmd *c = rec(C_DRAW);
        if (c) {
            ENSURE(cur->draw, cur->ndraw, cur->cdraw, 1);
            Draw *d = &cur->draw[cur->ndraw];
            memset(d, 0, sizeof *d);
            d->mode = mode; d->count = count; d->pos = d->uv = -1;
            d->tex = st.tex[0]; d->fbo = st.fbo; d->prog = st.prog;
            memcpy(d->vp, st.vp, sizeof d->vp);
            ProgInfo *pi = st.prog < MAXPROG ? &proginfo[st.prog] : NULL;
            for (GLuint i = 0; i < MAXATTR; i++) {
                Attr *a = &st.attr[i];
                if (!a->on) continue;
                if (!a->set || a->buf != 0 || !a->ptr || d->nattr == MAXATTR) { cur->invalid = 1; break; }
                int ts = type_size(a->type);
                if (!ts) { cur->invalid = 1; break; }
                int es = ts * a->size, stride = a->stride ? a->stride : es;
                DAttr *da = &d->at[d->nattr];
                da->idx = i; da->size = a->size; da->type = a->type; da->norm = a->norm; da->esize = es;
                da->off = arena_put(cur, NULL, es * count);
                const unsigned char *src = (const unsigned char *)a->ptr + (size_t)first * stride;
                unsigned char *dst = cur->data + da->off;
                if (stride == es) memcpy(dst, src, (size_t)es * count);
                else for (GLsizei k = 0; k < count; k++) memcpy(dst + (size_t)k * es, src + (size_t)k * stride, es);
                if (pi && pi->known_pos && (GLint)i == pi->pos && a->type == GL_FLOAT && a->size >= 2) d->pos = d->nattr;
                if (pi && pi->known_uv && (GLint)i == pi->uv && a->type == GL_FLOAT && a->size >= 2) d->uv = d->nattr;
                d->nattr++;
            }
            d->has_mvp = find_mats(st.prog, d->mvp);
            if (d->has_mvp) {
                float a00 = d->mvp[0], a01 = d->mvp[4], a10 = d->mvp[1], a11 = d->mvp[5];
                float det = a00 * a11 - a01 * a10;
                if (fabsf(det) > 1e-12f) {
                    d->inv[0] = a11 / det; d->inv[1] = -a01 / det; d->inv[2] = -a10 / det; d->inv[3] = a00 / det;
                    d->has_inv = 1;
                }
            }
            // a draw into the default framebuffer sampling an fbo's colour texture is the app-surface blit
            if (st.fbo == 0) {
                for (GLuint f = 1; f < MAXFBO; f++)
                    if (fbotex[f] && fbotex[f] == d->tex) { app_fbo = f; app_tex = d->tex; break; }
            }
            c->a[0] = cur->ndraw++;
        }
    }
    r_glDrawArrays(mode, first, count);
}

/* ---------- hooks: resources (tracked, never replayed) ---------- */

void glTexImage2D(GLenum t, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void *p)
{
    RESOLVE();
    if (!in_replay && MINE() && t == GL_TEXTURE_2D && lv == 0 && st.active < MAXUNIT && st.tex[st.active] < MAXTEX) {
        TexInfo *ti = &texinfo[st.tex[st.active]];
        ti->w = w; ti->h = h; ti->ifmt = ifmt;
    }
    r_glTexImage2D(t, lv, ifmt, w, h, b, fmt, ty, p);
}

void glFramebufferTexture2D(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lv)
{
    RESOLVE();
    if (!in_replay && MINE() && att == GL_COLOR_ATTACHMENT0 && st.fbo < MAXFBO) fbotex[st.fbo] = tex;
    r_glFramebufferTexture2D(t, att, tt, tex, lv);
}

GLint glGetUniformLocation(GLuint p, const GLchar *name)
{
    RESOLVE();
    GLint r = r_glGetUniformLocation(p, name);
    if (MINE() && p < MAXPROG && (!strcmp(name, "gm_Matrices") || !strcmp(name, "gm_Matrices[0]")) && r >= 0) {
        proginfo[p].mats = r;
        proginfo[p].known_mats = 1;
    }
    return r;
}

GLint glGetAttribLocation(GLuint p, const GLchar *name)
{
    RESOLVE();
    GLint r = r_glGetAttribLocation(p, name);
    if (MINE() && p < MAXPROG && r >= 0) {
        if (!strcmp(name, "in_Position")) { proginfo[p].pos = r; proginfo[p].known_pos = 1; }
        else if (!strcmp(name, "in_TextureCoord")) { proginfo[p].uv = r; proginfo[p].known_uv = 1; }
    }
    return r;
}

void glDeleteTextures(GLsizei n, const GLuint *ids)
{
    RESOLVE();
    for (GLsizei i = 0; MINE() && i < n; i++) {
        if (ids[i] < MAXTEX) memset(&texinfo[ids[i]], 0, sizeof(TexInfo));
        if (ids[i] == app_tex) { app_tex = 0; app_fbo = 0; }
    }
    r_glDeleteTextures(n, ids);
}

void glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
    RESOLVE();
    for (GLsizei i = 0; MINE() && i < n; i++) {
        if (ids[i] < MAXFBO) fbotex[ids[i]] = 0;
        if (ids[i] == app_fbo) { app_fbo = 0; app_tex = 0; }
    }
    r_glDeleteFramebuffers(n, ids);
}

/* ---------- state restore ---------- */

static void apply_state(const State *s)
{
    r_glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    if (s->vpset) r_glViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    if (s->scset) r_glScissor(s->sc[0], s->sc[1], s->sc[2], s->sc[3]);
    for (int i = 0; i < s->ncap; i++) (s->capon[i] ? r_glEnable : r_glDisable)(s->caps[i]);
    if (s->blendset) r_glBlendFunc(s->blend[0], s->blend[1]);
    if (s->setbits & SB_CLEAR) r_glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
    if (s->setbits & SB_CDEPTH) r_glClearDepth(s->cleardepth);
    if (s->setbits & SB_CSTENCIL) r_glClearStencil(s->clearstencil);
    if (s->setbits & SB_CMASK) r_glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
    if (sb_extra & 1) r_glDepthMask(s->dmask);
    if (sb_extra & 2) r_glDepthFunc(s->dfunc);
    if (sb_extra & 4) r_glDepthRange(s->drange[0], s->drange[1]);
    if (sb_extra & 8) r_glStencilFunc(s->sfunc, s->sref, s->sfmask);
    if (sb_extra & 16) r_glStencilOp(s->sop[0], s->sop[1], s->sop[2]);
    if (sb_extra & 32) r_glStencilMask(s->smask);
    if (s->setbits & SB_FACE) r_glFrontFace(s->frontface);
    if (s->setbits & SB_SHADE) r_glShadeModel(s->shademodel);
    for (int u = 0; u < MAXUNIT; u++)
        if (s->texset[u]) { r_glActiveTexture(GL_TEXTURE0 + u); r_glBindTexture(GL_TEXTURE_2D, s->tex[u]); }
    r_glActiveTexture(GL_TEXTURE0 + s->active);
    for (GLuint i = 0; i < MAXATTR; i++) {
        const Attr *a = &s->attr[i];
        if (a->set) {
            r_glBindBuffer(GL_ARRAY_BUFFER, a->buf);
            r_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, a->ptr);
        }
        (a->on ? r_glEnableVertexAttribArray : r_glDisableVertexAttribArray)(i);
    }
    r_glBindBuffer(GL_ARRAY_BUFFER, s->arraybuf);
    r_glUseProgram(s->prog);
}

/* ---------- quads & matching ---------- */

static uint32_t hash_bytes(const void *p, int n, uint32_t h)
{
    const unsigned char *b = p;
    for (int i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void project(const float *m, const GLint *vp, float x, float y, float *sx, float *sy)
{
    float nx = m[0] * x + m[4] * y + m[12];
    float ny = m[1] * x + m[5] * y + m[13];
    float w = m[3] * x + m[7] * y + m[15];
    if (fabsf(w) < 1e-6f) w = 1;
    *sx = (nx / w + 1) * 0.5f * vp[2] + vp[0];
    *sy = (ny / w + 1) * 0.5f * vp[3] + vp[1];
}

static void build_quads(Frame *f)
{
    f->nq = 0;
    f->has_cam = 0;
    f->cam_mx = f->cam_my = 0;
    // main view matrix: the one used by the most vertices drawn into the app surface (or screen)
    GLuint target = app_fbo;
    int best = -1, bestn = 0;
    for (int i = 0; i < f->ndraw; i++) {
        Draw *d = &f->draw[i];
        if (!d->has_mvp || d->fbo != target) continue;
        int n = 0;
        for (int j = 0; j < f->ndraw; j++)
            if (f->draw[j].has_mvp && f->draw[j].fbo == target && !memcmp(f->draw[j].mvp, d->mvp, 64)) n += f->draw[j].count;
        if (n > bestn) { bestn = n; best = i; }
        if (bestn * 2 > 1) break; // first big group is good enough (draws usually share one view)
    }
    f->has_main = best >= 0;
    if (best >= 0) { memcpy(f->main_mvp, f->draw[best].mvp, 64); memcpy(f->main_vp, f->draw[best].vp, sizeof f->main_vp); }

    for (int i = 0; i < f->ndraw; i++) {
        Draw *d = &f->draw[i];
        if (d->pos < 0 || !d->has_mvp || !d->has_inv) continue;
        if (d->fbo != app_fbo && d->fbo != 0) continue;
        const float *pos = (const float *)(f->data + d->at[d->pos].off);
        int ps = d->at[d->pos].size;
        const float *uv = d->uv >= 0 ? (const float *)(f->data + d->at[d->uv].off) : NULL;
        int us = d->uv >= 0 ? d->at[d->uv].size : 0;
        int is_main = f->has_main && !memcmp(d->mvp, f->main_mvp, 64);
        int quads = d->mode == GL_TRIANGLES && d->count % 6 == 0;
        int nunits = quads ? d->count / 6 : 1;
        int per = quads ? 6 : d->count;
        ENSURE(f->q, f->nq, f->cq, nunits);
        for (int k = 0; k < nunits; k++) {
            Quad *q = &f->q[f->nq++];
            memset(q, 0, sizeof *q);
            q->draw = i; q->v0 = k * per; q->nv = per; q->tex = d->tex; q->fbo = d->fbo;
            q->main = is_main; q->group = !quads; q->kind = Q_SCREEN;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, sxm = 0, sym = 0;
            float uvb[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
            for (int v = q->v0; v < q->v0 + per; v++) {
                float x = pos[v * ps], y = pos[v * ps + 1];
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                sxm += x; sym += y;
                if (uv) {
                    float u = uv[v * us], w = uv[v * us + 1];
                    if (u < uvb[0]) uvb[0] = u;
                    if (w < uvb[1]) uvb[1] = w;
                    if (u > uvb[2]) uvb[2] = u;
                    if (w > uvb[3]) uvb[3] = w;
                }
            }
            if (quads) { q->vx = (x0 + x1) * 0.5f; q->vy = (y0 + y1) * 0.5f; }
            else { q->vx = sxm / per; q->vy = sym / per; }
            project(d->mvp, d->vp, q->vx, q->vy, &q->sx, &q->sy);
            float ax, ay, bx, by;
            project(d->mvp, d->vp, x0, y0, &ax, &ay);
            project(d->mvp, d->vp, x1, y1, &bx, &by);
            q->w = fabsf(bx - ax); q->h = fabsf(by - ay);
            uint32_t h = 2166136261u;
            h = hash_bytes(&d->tex, sizeof d->tex, h);
            h = hash_bytes(&d->fbo, sizeof d->fbo, h);
            if (quads) {
                if (uv) h = hash_bytes(uvb, sizeof uvb, h);
                else { float sz[2] = { q->w, q->h }; h = hash_bytes(sz, sizeof sz, h); }
            } else {
                h = hash_bytes(&d->mode, sizeof d->mode, h);
                h = hash_bytes(&d->count, sizeof d->count, h) ^ 0x9e3779b9u;
            }
            q->key = h;
        }
    }
}

#define EXACT_R 1.5f    // px of the render target (app surface = 2x game pixels)
#define MOVE_R 32.0f    // own motion per tick that still counts as the same object
#define FALLBACK_R 16.0f
#define CAM_MAX 48.0f   // bigger camera jumps are cuts: no interpolation

static int *hhead, *hnext, hsize;
static unsigned char *used;

static float d2(float ax, float ay, float bx, float by) { return (ax - bx) * (ax - bx) + (ay - by) * (ay - by); }

// Every quad is either world-static (moves exactly with the camera), screen-static (UI) or an
// object with its own motion. Only the last needs a match, and it must be close and unambiguous:
// a wrong match throws a piece of the picture across the screen, a missed one costs nothing.
static void match_frames(Frame *f, Frame *p)
{
    f->nmatched = f->nfallback = 0;
    f->has_cam = 0;
    f->cam_mx = f->cam_my = 0;
    if (f->has_main && p->has_main) {
        float ax, ay, bx, by;
        project(f->main_mvp, f->main_vp, 0, 0, &ax, &ay);
        project(p->main_mvp, f->main_vp, 0, 0, &bx, &by);
        float cx = roundf((ax - bx) * 16) / 16, cy = roundf((ay - by) * 16) / 16; // drop float noise
        if (cx * cx + cy * cy <= CAM_MAX * CAM_MAX) { f->cam_mx = cx; f->cam_my = cy; f->has_cam = 1; }
    }
    for (int i = 0; i < f->nq; i++) {
        Quad *q = &f->q[i];
        q->kind = q->main && f->has_cam ? Q_WORLD : Q_SCREEN;
        q->mx = q->my = 0;
        q->matched = 0;
    }
    if (!p->nq) return;
    int need = 1;
    while (need < p->nq * 2) need <<= 1;
    if (need > hsize) { hsize = need; hhead = realloc(hhead, hsize * sizeof(int)); }
    hnext = realloc(hnext, p->nq * sizeof(int));
    used = realloc(used, p->nq);
    memset(used, 0, p->nq);
    for (int i = 0; i < need; i++) hhead[i] = -1;
    for (int i = 0; i < p->nq; i++) {
        int b = p->q[i].key & (need - 1);
        hnext[i] = hhead[b];
        hhead[b] = i;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < f->nq; i++) {
            Quad *q = &f->q[i];
            if (q->matched) continue;
            int world = q->main && f->has_cam;
            float pwx = q->sx - (world ? f->cam_mx : 0), pwy = q->sy - (world ? f->cam_my : 0);
            int bw = -1, bs = -1, b1 = -1;
            float dw = 1e30f, ds = 1e30f, m1 = 1e30f, m2 = 1e30f;
            // pass 0: same sub-image (exact key); pass 1: same texture page and size (animation frame changed)
            int c = pass == 0 ? hhead[q->key & (need - 1)] : 0;
            for (; pass == 0 ? c >= 0 : c < p->nq; c = pass == 0 ? hnext[c] : c + 1) {
                Quad *o = &p->q[c];
                if (used[c] || o->fbo != q->fbo) continue;
                if (pass == 0) {
                    if (o->key != q->key) continue;
                } else {
                    if (o->group || q->group || o->tex != q->tex) continue;
                    float tw = q->w * 0.35f > 3 ? q->w * 0.35f : 3, th = q->h * 0.35f > 3 ? q->h * 0.35f : 3;
                    if (fabsf(o->w - q->w) > tw || fabsf(o->h - q->h) > th) continue;
                }
                float a = d2(o->sx, o->sy, pwx, pwy), b = d2(o->sx, o->sy, q->sx, q->sy);
                if (a < dw) { dw = a; bw = c; }
                if (b < ds) { ds = b; bs = c; }
                if (a < m1) { m2 = m1; m1 = a; b1 = c; }
                else if (a < m2) m2 = a;
            }
            float R = pass == 0 ? MOVE_R : FALLBACK_R;
            if (bw >= 0 && dw <= EXACT_R * EXACT_R) {
                used[bw] = 1; q->matched = 1; q->kind = world ? Q_WORLD : Q_SCREEN;
            } else if (bs >= 0 && ds <= EXACT_R * EXACT_R) {
                used[bs] = 1; q->matched = 1; q->kind = Q_SCREEN;
            } else if (b1 >= 0 && m1 <= R * R && sqrtf(m2) - sqrtf(m1) > 6.0f) {
                used[b1] = 1; q->matched = 1; q->kind = Q_MOVING;
                q->mx = q->sx - p->q[b1].sx;
                q->my = q->sy - p->q[b1].sy;
                if (pass) f->nfallback++;
            }
            if (q->matched && pass == 0) f->nmatched++;
        }
    }
}

/* ---------- replay ---------- */

static GLuint g_fbo, g_tex, g_for;
static GLsizei g_w, g_h;
static unsigned char *scratch;
static int cscratch;

static int ensure_ghost_target(Frame *f)
{
    if (!f->app_fbo) return 1; // the game renders straight to the window
    if (f->app_tex >= MAXTEX) return 0;
    TexInfo *ti = &texinfo[f->app_tex];
    if (!ti->w || !ti->h) return 0;
    if (g_fbo && g_for == f->app_tex && g_w == ti->w && g_h == ti->h) return 1;
    if (!g_tex) r_glGenTextures(1, &g_tex);
    if (!g_fbo) r_glGenFramebuffers(1, &g_fbo);
    r_glActiveTexture(GL_TEXTURE0);
    r_glBindTexture(GL_TEXTURE_2D, g_tex);
    r_glTexImage2D(GL_TEXTURE_2D, 0, ti->ifmt ? ti->ifmt : GL_RGBA, ti->w, ti->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    r_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ti->hasparam && ti->minf ? ti->minf : GL_NEAREST);
    r_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ti->hasparam && ti->magf ? ti->magf : GL_NEAREST);
    r_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ti->hasparam && ti->ws ? ti->ws : GL_CLAMP_TO_EDGE);
    r_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ti->hasparam && ti->wt ? ti->wt : GL_CLAMP_TO_EDGE);
    r_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    r_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tex, 0);
    if (r_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("ghost fbo incomplete, disabling");
        cfg_on = 0;
        return 0;
    }
    g_for = f->app_tex; g_w = ti->w; g_h = ti->h;
    LOG("ghost target %ux%u for app surface fbo %u tex %u", g_w, g_h, f->app_fbo, f->app_tex);
    return 1;
}

static void move_quad(Frame *f, Quad *q, unsigned char *arena, float px, float py)
{
    Draw *d = &f->draw[q->draw];
    float dnx = px * 2.0f / (d->vp[2] ? d->vp[2] : 1);
    float dny = py * 2.0f / (d->vp[3] ? d->vp[3] : 1);
    float dvx = d->inv[0] * dnx + d->inv[1] * dny;
    float dvy = d->inv[2] * dnx + d->inv[3] * dny;
    float *pos = (float *)(arena + d->at[d->pos].off);
    int ps = d->at[d->pos].size;
    for (int v = q->v0; v < q->v0 + q->nv; v++) { pos[v * ps] += dvx; pos[v * ps + 1] += dvy; }
}

static unsigned char *arena_copy(Frame *f)
{
    if (f->ndata > cscratch) { cscratch = f->ndata * 2; scratch = realloc(scratch, cscratch); }
    memcpy(scratch, f->data, f->ndata);
    return scratch;
}

// Vertex data for a present: positions moved by fac * motion (fac = -(1-alpha) interp, alpha extrap),
// in whole target pixels (pixel art drawn with nearest sampling shimmers at fractional offsets).
// All world-static quads share one rounded offset so tiles never open seams between them.
static const unsigned char *prepare_arena(Frame *f, float fac)
{
    float wx = f->has_cam ? roundf(fac * f->cam_mx) : 0, wy = f->has_cam ? roundf(fac * f->cam_my) : 0;
    unsigned char *arena = NULL;
    for (int i = 0; i < f->nq; i++) {
        Quad *q = &f->q[i];
        float px, py;
        if (q->kind == Q_WORLD) { px = wx; py = wy; }
        else if (q->kind == Q_MOVING) { px = roundf(fac * q->mx); py = roundf(fac * q->my); }
        else continue;
        if (px == 0 && py == 0) continue;
        if (!arena) arena = arena_copy(f);
        move_quad(f, q, arena, px, py);
    }
    return arena ? arena : f->data;
}

// The previous frame's world-static content moved to where this present shows the world, drawn
// under it: fills the strip at the screen edge that this frame never drew (it was off camera).
static const unsigned char *prepare_underlay(Frame *f, float px, float py)
{
    unsigned char *arena = arena_copy(f);
    for (int i = 0; i < f->nq; i++) {
        Quad *q = &f->q[i];
        Draw *d = &f->draw[q->draw];
        if (q->fbo != f->app_fbo || !f->app_fbo) continue;
        if (q->kind == Q_WORLD) { move_quad(f, q, arena, px, py); continue; }
        // movers and UI would show up twice: collapse them
        float *pos = (float *)(arena + d->at[d->pos].off);
        int ps = d->at[d->pos].size;
        for (int v = q->v0 + 1; v < q->v0 + q->nv; v++) { pos[v * ps] = pos[q->v0 * ps]; pos[v * ps + 1] = pos[q->v0 * ps + 1]; }
    }
    return arena;
}

static void replay_ex(Frame *f, const unsigned char *arena, int keep_app)
{
    in_replay = 1;
    int direct = !f->app_fbo;
    if (!ensure_ghost_target(f)) { in_replay = 0; return; }
    apply_state(&f->s0);
    uniset_apply(&f->u0);
    r_glUseProgram(f->s0.prog);
    r_glBindBuffer(GL_ARRAY_BUFFER, 0);
    GLuint fbo = f->s0.fbo;
    int skip = !(fbo == 0 || (!direct && fbo == f->app_fbo));
    if (!direct && fbo == f->app_fbo) r_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    GLuint prog = f->s0.prog;
    for (int i = 0; i < f->ncmd; i++) {
        Cmd *c = &f->cmd[i];
        switch (c->op) {
        case C_ACTIVETEX: r_glActiveTexture(c->a[0]); break;
        case C_BINDTEX: r_glBindTexture(c->a[0], (!direct && (GLuint)c->a[1] == f->app_tex) ? g_tex : (GLuint)c->a[1]); break;
        case C_USEPROG: prog = c->a[0]; r_glUseProgram(prog); break;
        case C_UNIFORM: {
            Uni u = { prog, c->a[1], c->a[0], c->a[2], (GLboolean)c->a[3], c->len, f->data + c->off };
            uni_apply(&u);
            break;
        }
        case C_ENABLE: r_glEnable(c->a[0]); break;
        case C_DISABLE: r_glDisable(c->a[0]); break;
        case C_BLEND: r_glBlendFunc(c->a[0], c->a[1]); break;
        case C_VIEWPORT: r_glViewport(c->a[0], c->a[1], c->a[2], c->a[3]); break;
        case C_SCISSOR: r_glScissor(c->a[0], c->a[1], c->a[2], c->a[3]); break;
        case C_CLEARCOLOR: r_glClearColor(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case C_CLEAR: if (!skip && !(keep_app && fbo != 0)) r_glClear(c->a[0]); break;
        case C_CLEARDEPTH: r_glClearDepth(c->d[0]); break;
        case C_CLEARSTENCIL: r_glClearStencil(c->a[0]); break;
        case C_COLORMASK: r_glColorMask(c->a[0], c->a[1], c->a[2], c->a[3]); break;
        case C_DEPTHMASK: r_glDepthMask(c->a[0]); break;
        case C_DEPTHFUNC: r_glDepthFunc(c->a[0]); break;
        case C_DEPTHRANGE: r_glDepthRange(c->d[0], c->d[1]); break;
        case C_STENCILFUNC: r_glStencilFunc(c->a[0], c->a[1], (GLuint)c->a[2]); break;
        case C_STENCILOP: r_glStencilOp(c->a[0], c->a[1], c->a[2]); break;
        case C_STENCILMASK: r_glStencilMask((GLuint)c->a[0]); break;
        case C_FRONTFACE: r_glFrontFace(c->a[0]); break;
        case C_SHADEMODEL: r_glShadeModel(c->a[0]); break;
        case C_HINT: r_glHint(c->a[0], c->a[1]); break;
        case C_TEXPARAMF: r_glTexParameterf(c->a[0], c->a[1], c->f[0]); break;
        case C_BINDFBO:
            fbo = c->a[1];
            if (fbo == 0) { skip = 0; r_glBindFramebuffer(c->a[0], 0); }
            else if (!direct && fbo == f->app_fbo) { skip = 0; r_glBindFramebuffer(c->a[0], g_fbo); }
            else skip = 1; // user surfaces keep the content the real frame drew
            break;
        case C_DRAWBUFFER: if (!skip) r_glDrawBuffer(c->a[0]); break;
        case C_DRAWBUFFERS: if (!skip) r_glDrawBuffers(c->a[0], (const GLenum *)(f->data + c->off)); break;
        case C_DRAW: {
            if (skip) break;
            Draw *d = &f->draw[c->a[0]];
            for (int k = 0; k < d->nattr; k++) {
                DAttr *a = &d->at[k];
                r_glEnableVertexAttribArray(a->idx);
                r_glVertexAttribPointer(a->idx, a->size, a->type, a->norm, 0, arena + a->off);
            }
            r_glDrawArrays(d->mode, 0, d->count);
            for (int k = 0; k < d->nattr; k++) r_glDisableVertexAttribArray(d->at[k].idx);
            break;
        }
        }
    }
    uniset_apply(&unis);
    apply_state(&st);
    while (r_glGetError() != GL_NO_ERROR) {}
    in_replay = 0;
}

static void replay(Frame *f, const unsigned char *arena) { replay_ex(f, arena, 0); }

/* ---------- frame dumps & self-test (testing only) ---------- */

static void window_size(Frame *f, GLint *w, GLint *h)
{
    *w = *h = 0;
    // the last viewport set for the default framebuffer is the window size
    GLuint fbo = f->s0.fbo;
    for (int i = 0; i < f->ncmd; i++) {
        if (f->cmd[i].op == C_BINDFBO) fbo = f->cmd[i].a[1];
        if (f->cmd[i].op == C_VIEWPORT && fbo == 0) { *w = f->cmd[i].a[2]; *h = f->cmd[i].a[3]; }
    }
}

static unsigned char *read_back(GLint w, GLint h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    in_replay = 1;
    r_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    r_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    r_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    r_glBindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    in_replay = 0;
    return px;
}

static void dump_backbuffer(Frame *f, const char *tag, double alpha)
{
    if (!dump_dir || dump_left <= 0) return;
    static int seq;
    GLint w, h;
    window_size(f, &w, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    unsigned char *px = read_back(w, h);
    char fn[512];
    snprintf(fn, sizeof fn, "%s/%05d_%s_a%.3f.pam", dump_dir, seq++, tag, alpha);
    FILE *o = fopen(fn, "wb");
    if (o) {
        // raw bottom-up RGBA (flipped by the viewer script)
        fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
        fwrite(px, 1, (size_t)w * h * 4, o);
        fclose(o);
    }
    free(px);
    dump_left--;
}

static void selftest_compare(Frame *f)
{
    // a replay without motion must reproduce the runner's own frame exactly
    static int runs, calls;
    if (runs >= 40 || calls++ % 45) return;
    GLint w, h;
    window_size(f, &w, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    size_t n = (size_t)w * h * 4, diff = 0;
    unsigned char *a = read_back(w, h);
    replay(f, f->data);
    unsigned char *b = read_back(w, h);
    for (size_t i = 0; i < n; i++) diff += a[i] != b[i];
    LOG("SELFTEST cmds=%d draws=%d quads=%d differing bytes=%zu of %zu", f->ncmd, f->ndraw, f->nq, diff, n);
    free(a);
    free(b);
    runs++;
}

/* ---------- presenting ---------- */

// Each present shows the game state at time s = V - delay, V being the vblank it is shown at.
// With a fixed delay every vblank advances the picture by exactly one refresh period of game
// time, which is what makes motion smooth; "need" tracks how far behind the newest tick a
// present can land so the delay always covers it (interp) or one tick less (extrap).
static double need_max = 0.045;
static double delay_margin = 0.0005;
static double need_ring[32], tick_need;
static int need_pos;

// at every tick: need_max = worst need over the last 32 ticks (a hitch longer than a tick is ignored)
static void need_tick(void)
{
    double cap = period_est + 0.03;
    need_ring[need_pos++ & 31] = tick_need > cap ? cap : tick_need;
    tick_need = 0;
    double m = 0;
    for (int i = 0; i < 32; i++) if (need_ring[i] > m) m = need_ring[i];
    if (need_pos >= 4) need_max = m;
}

// Hardware vblank times straight from the kernel (DRM_IOCTL_WAIT_VBLANK with a relative-0 query
// returns at once with the last vblank's sequence and CLOCK_MONOTONIC timestamp).
struct vbl_req { uint32_t type, sequence; unsigned long signal; };
struct vbl_rep { uint32_t type, sequence; long tval_sec, tval_usec; };
union vbl { struct vbl_req request; struct vbl_rep reply; };
#define VBL_IOCTL _IOWR('d', 0x3a, union vbl)
static int vbl_fd = -1, vbl_crtc, vbl_ok;
static double vbl_t, vbl_R, vbl_last_query, vbl_ref_t;
static uint32_t vbl_seq, vbl_ref_seq;

static int vbl_read(int fd, int crtc, uint32_t *seq, double *t)
{
    union vbl v;
    memset(&v, 0, sizeof v);
    v.request.type = 0x1 /* relative */ | (crtc << 1 /* high crtc shift */);
    if (ioctl(fd, VBL_IOCTL, &v)) return 0;
    *seq = v.reply.sequence;
    *t = v.reply.tval_sec + v.reply.tval_usec * 1e-6;
    return *t > 0;
}

static void vbl_init(void)
{
    char path[32];
    for (int c = 0; c < 4 && !vbl_ok; c++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", c);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        for (int crtc = 0; crtc < 4 && !vbl_ok; crtc++) {
            uint32_t s0, s1;
            double t0, t1;
            if (!vbl_read(fd, crtc, &s0, &t0)) continue;
            struct timespec d = { 0, 60000000 };
            nanosleep(&d, NULL);
            if (!vbl_read(fd, crtc, &s1, &t1) || s1 <= s0) continue;
            double R = (t1 - t0) / (s1 - s0);
            if (R < 0.002 || R > 0.05 || fabs(mono() - t1) > 0.1) continue;
            vbl_fd = fd; vbl_crtc = crtc; vbl_ok = 1;
            vbl_R = R; vbl_seq = vbl_ref_seq = s1; vbl_t = vbl_ref_t = t1;
            LOG("vblank source %s crtc %d, refresh %.3f ms", path, crtc, R * 1000);
        }
        if (!vbl_ok) close(fd);
    }
    if (!vbl_ok) LOG("no hardware vblank source, pacing by timer");
}

static void vbl_update(double now)
{
    uint32_t seq;
    double t;
    vbl_last_query = now;
    if (!vbl_read(vbl_fd, vbl_crtc, &seq, &t) || fabs(now - t) > 1.0) return; // display off: keep the old phase
    vbl_seq = seq;
    vbl_t = t;
    if (seq - vbl_ref_seq >= 60) { // refine the period over at least ~0.5 s
        double R = (t - vbl_ref_t) / (seq - vbl_ref_seq);
        if (R > 0.002 && R < 0.05) vbl_R = R;
        if (seq - vbl_ref_seq > 2000) { vbl_ref_seq = seq; vbl_ref_t = t; }
    }
}

// first vblank strictly after time x
static double vblank_after(double x) { return vbl_t + (floor((x - vbl_t) / vbl_R) + 1) * vbl_R; }

static void cap_grab(GLuint fbo, int w, int h, const char *tag, double a);
static int in_present; // a present is in progress (Mesa calls clock_gettime inside the swap)


static void present_state(double V, int64_t target, int ghost)
{
    in_present = 1;
    double need = V - prv->L;
    if (need > tick_need) tick_need = need;
    if (need > need_max && need < period_est + 0.03) need_max = need; // react at once to a new worst case
    double P = period_est;
    double delay = need_max + delay_margin - (cfg_extrap ? P : 0);
    double s = V - delay;
    Frame *f = prv;
    double a;
    if (cfg_extrap) {
        a = (s - prv->L) / P;
    } else {
        if (s < prv->Lprev && old_ok) f = old;
        double T = f->L - f->Lprev;
        a = T > 0.015 && T < 0.08 ? (s - f->Lprev) / T : 1;
    }
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    float fac = cfg_extrap ? (float)a : (float)-(1 - a);
    double t0 = mono();
    float wx = f->has_cam ? roundf(fac * f->cam_mx) : 0, wy = f->has_cam ? roundf(fac * f->cam_my) : 0;
    if (!cfg_extrap && f == prv && old_ok && f->app_fbo && (wx != 0 || wy != 0)) {
        replay(old, prepare_underlay(old, wx + roundf(f->cam_mx), wy + roundf(f->cam_my)));
        replay_ex(f, prepare_arena(f, fac), 1);
    } else {
        replay(f, prepare_arena(f, fac));
    }
    if (cap_on && g_fbo) cap_grab(g_fbo, g_w, g_h, ghost ? "ghost" : "real", a);
    double tr = mono();
    dump_backbuffer(f, ghost ? "ghost" : "real", a);
    in_replay = 1; // the next swap in the chain (Steam overlay) draws with GL calls we must not record
    r_glXSwapBuffers(g_dpy, g_draw);
    in_replay = 0;
    double t1 = mono();
    double took = t1 - t0;
    if (took < 0.004) ghost_cost += (took - ghost_cost) * 0.2;
    double blocked = t1 - tr;
    int trouble = 0;
    if (blocked > 0.0015 && vbl_ok) {
        // the swap waited for the previous frame to be taken: that moment is the pickup phase
        stat_blocks++;
        double ph = fmod(t1 - vbl_t, vbl_R);
        if (ph < 0) ph += vbl_R;
        double d = remainder(ph - cb_phase, vbl_R);
        cb_phase = fmod(cb_phase + d * (stat_blocks < 3 ? 1.0 : 0.5) + vbl_R, vbl_R);
        block_t[block_n++ & 7] = t1;
        if (block_n >= 6 && t1 - block_t[(block_n - 6) & 7] < 2.0) trouble = 1; // keeps blocking
    }
    if (ghost && t1 > frame_start_est + period_est + 0.0005) ghost_overran = 1; // checked when the tick starts
    if (blocked > 0.1) trouble = 1; // window hidden / throttled
    if (trouble) {
        // present like the stock game for a while, then probe again with a growing pause
        backoff_len = backoff_len ? (backoff_len * 2 > 4 ? 4 : backoff_len * 2) : 0.5;
        backoff_until = t1 + backoff_len;
        block_n = late_n = 0;
        LOG("presents keep stalling (last %.1f ms), plain presents for %.1f s", blocked * 1000, backoff_len);
    } else if (t1 > backoff_until + 4) {
        backoff_len = 0;
    }
    if (slog) {
        int b = (int)((t1 - last_present) * 1000);
        stat_hist[b < 0 ? 0 : b > 39 ? 39 : b]++;
        if (!stat_npres++) stat_t0 = t1;
        static int dbg;
        if (dbg < 600 && getenv("SMOOTH_DEBUG")) {
            dbg++;
            LOG("PRESENT %s V-now=%.2fms s-L=%.2fms frame=%s a=%.3f delay=%.2fms replay=%.2fms swap=%.2fms vb=%.2f",
                ghost ? "ghost" : "real ", (V - t0) * 1000, (s - prv->L) * 1000, f == prv ? "new" : "old", a, delay * 1000,
                (tr - t0) * 1000, (t1 - tr) * 1000, vbl_ok ? fmod(V - vbl_t, vbl_R) * 1000 : 0.0);
        }
    }
    last_present = t1;
    last_alpha = a;
    if (target) last_V = V;
    stat_ghosts += ghost;
    in_present = 0;
}

static void reset_gl_objects(void)
{
    g_fbo = g_tex = g_for = 0;
    g_w = g_h = 0;
    prv_ok = old_ok = 0;
    app_fbo = app_tex = 0;
    memset(fbotex, 0, sizeof fbotex);
}

Bool glXMakeCurrent(Display *d, GLXDrawable w, GLXContext c)
{
    RESOLVE();
    Bool r = r_glXMakeCurrent(d, w, c);
    if (!r) return r;
    tls_ctx = c;
    if (c && !runner_ctx) runner_ctx = c; // the runner sets up its context before anything else draws
    else if (c && c != runner_ctx && have_main && pthread_equal(pthread_self(), main_thread)) {
        LOG("runner switched to a new context, resetting");
        runner_ctx = c;
        reset_gl_objects();
        have_main = 0;
    }
    return r;
}

// Debug capture (SMOOTH_CAPTURE=1): keep the last CAPN presented frames (app surface size) in memory,
// F9 writes them out next to the log
#define CAPN 48
static int cap_request, cap_pos, cap_w, cap_h;
static struct { unsigned char *px; char tag[8]; double a, t; int frame; } cap_ring[CAPN];

static void cap_grab(GLuint fbo, int w, int h, const char *tag, double a)
{
    if (!cap_on || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
    if (w != cap_w || h != cap_h) {
        for (int i = 0; i < CAPN; i++) { free(cap_ring[i].px); cap_ring[i].px = NULL; }
        cap_w = w; cap_h = h;
    }
    int i = cap_pos++ % CAPN;
    if (!cap_ring[i].px) cap_ring[i].px = malloc((size_t)w * h * 4);
    in_replay = 1;
    r_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    r_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    r_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, cap_ring[i].px);
    r_glBindFramebuffer(GL_FRAMEBUFFER, st.fbo);
    in_replay = 0;
    snprintf(cap_ring[i].tag, sizeof cap_ring[i].tag, "%s", tag);
    cap_ring[i].a = a;
    cap_ring[i].t = mono();
    cap_ring[i].frame = (int)stat_real;
}

static void cap_write(void)
{
    cap_request = 0;
    const char *lp = getenv("SMOOTH_LOG");
    char dir[512];
    snprintf(dir, sizeof dir, "%.*s/capture-%ld", lp && strrchr(lp, '/') ? (int)(strrchr(lp, '/') - lp) : 1, lp ? lp : ".", (long)time(NULL));
    if (mkdir(dir, 0755)) return;
    int n = cap_pos < CAPN ? cap_pos : CAPN;
    for (int k = 0; k < n; k++) {
        int i = (cap_pos - n + k) % CAPN;
        char fn[640];
        snprintf(fn, sizeof fn, "%s/%03d_f%06d_%s_a%.3f_t%.4f.pam", dir, k, cap_ring[i].frame, cap_ring[i].tag, cap_ring[i].a, cap_ring[i].t);
        FILE *o = fopen(fn, "wb");
        if (!o) continue;
        fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", cap_w, cap_h);
        fwrite(cap_ring[i].px, 1, (size_t)cap_w * cap_h * 4, o);
        fclose(o);
    }
    LOG("capture: %d frames written to %s", n, dir);
}

// The runner reads its events with XNextEvent; focus changes tell us when the window gets hidden
// (alt-tab out of fullscreen: presents would stall) and when it is back.
static volatile int unfocused;

int XNextEvent(Display *d, XEvent *ev)
{
    static int (*real)(Display *, XEvent *);
    if (!real) real = (int (*)(Display *, XEvent *))dlsym(RTLD_NEXT, "XNextEvent");
    int r = real(d, ev);
    if (have_main && d == g_dpy) {
        int t = ev->type;
        if (t == FocusIn || t == FocusOut)
            LOG("event %s window 0x%lx mode %d detail %d", t == FocusIn ? "FocusIn" : "FocusOut", ev->xfocus.window, ev->xfocus.mode,
                ev->xfocus.detail);
        int in_hier = (t == FocusIn || t == FocusOut) && ev->xfocus.detail == NotifyInferior;
        if ((t == FocusOut && !in_hier && ev->xfocus.mode != NotifyGrab) || (t == UnmapNotify && ev->xany.window == g_draw)) {
            if (!unfocused) LOG("window lost focus: plain presents");
            unfocused = 1;
        } else if ((t == FocusIn && !in_hier) || t == KeyPress || (t == MapNotify && ev->xany.window == g_draw)) {
            if (unfocused) {
                LOG("window focused again: smoothing back on");
                backoff_until = backoff_len = 0;
                block_n = late_n = 0;
            }
            unfocused = 0;
        }
        if (t == KeyPress && cap_on && XLookupKeysym(&ev->xkey, 0) == XK_F9) cap_request = 1;
    }
    return r;
}

static void start_recording(void)
{
    cur->ncmd = cur->ndata = cur->ndraw = cur->nq = 0;
    cur->invalid = cur->started = 0;
    cur->s0 = st;
    uniset_copy(&cur->u0, &unis);
    swap_ret = mono();
    next_check = swap_ret + 0.001;
    waiting = 1;
}

void glXSwapBuffers(Display *dpy, GLXDrawable w)
{
    RESOLVE();
    config();
    if (!cfg_on || in_replay || !MINE()) { r_glXSwapBuffers(dpy, w); return; }
    if (!have_main) {
        main_thread = pthread_self();
        have_main = 1;
        g_dpy = dpy;
        g_draw = w;
        if (cfg_vsync) vbl_init();
        LOG("smooth: cap %.1f fps, mode %s", 1 / cfg_min_interval, cfg_extrap ? "extrap" : "interp");
        r_glXSwapBuffers(dpy, w);
        last_present = mono();
        start_recording();
        return;
    }
    if (dpy != g_dpy || w != g_draw || !pthread_equal(pthread_self(), main_thread)) { r_glXSwapBuffers(dpy, w); return; }

    // the runner finished frame "cur"
    if (!cur->started) frame_begin();
    cur->s1 = st;
    uniset_copy(&cur->u1, &unis);
    cur->app_fbo = app_fbo;
    cur->app_tex = app_tex;
    cur->Lprev = prv_ok ? prv->L : cur->L - period_est;
    int ok = !cur->invalid && cur->ndraw > 0;
    if (ok) {
        build_quads(cur);
        if (prv_ok) match_frames(cur, prv);
        else for (int i = 0; i < cur->nq; i++) { cur->q[i].mx = cur->q[i].my = 0; cur->q[i].kind = Q_SCREEN; }
    }
    if (dump_trigger && access(dump_trigger, F_OK) == 0) { unlink(dump_trigger); dump_left = 60; }
    if (ok && cfg_selftest) selftest_compare(cur);
    if (ok && dump_left > 0) dump_backbuffer(cur, "orig", 0);
    if (ok && cap_on && cur->app_fbo && cur->app_tex < MAXTEX) cap_grab(cur->app_fbo, texinfo[cur->app_tex].w, texinfo[cur->app_tex].h, "orig", 0);
    if (cap_request) cap_write();

    // rotate: cur becomes the newest complete frame
    Frame *t = old;
    old = prv;
    old_ok = prv_ok;
    prv = cur;
    prv_ok = ok;
    cur = t;
    stat_real++;

    double now = mono();
    if (!ok || now < backoff_until || unfocused) {
        in_replay = 1;
        r_glXSwapBuffers(dpy, w); // show the runner's own frame as the stock game would
        in_replay = 0;
        last_present = mono();
        if (!ok) old_ok = 0;
    } else if (vbl_ok) {
        // presents follow the vblank schedule from the wait loop; only cover a gap here
        if (now - last_present > 0.1) {
            in_replay = 1;
            r_glXSwapBuffers(dpy, w); // scheduler starved (long runner frames): keep the screen alive
            in_replay = 0;
            last_present = mono();
        }
    } else {
        present_state(now, 0, 0);
    }
    need_tick();
    if (slog && stat_real % 300 == 0) {
        LOG("frame: cmds=%d draws=%d quads=%d matched=%d fallback=%d ok=%d period=%.2fms need_max=%.2fms work=%.2fms ghosts=%ld late=%ld cost=%.2fms",
            prv->ncmd, prv->ndraw, prv->nq, prv->nmatched, prv->nfallback, ok, period_est * 1000, need_max * 1000,
            work_est * 1000, stat_ghosts, stat_skipped_deadline, ghost_cost * 1000);
        if (stat_real % 1800 == 0 && stat_npres > 1) {
            char buf[512];
            int o = 0;
            for (int i = 0; i < 40; i++) if (stat_hist[i]) o += snprintf(buf + o, sizeof buf - o, " %dms:%ld", i, stat_hist[i]);
            LOG("presents/s=%.1f vbl=%d R=%.3fms pickup=%.2fms blocks=%ld intervals:%s", (stat_npres - 1) / (last_present - stat_t0), vbl_ok, vbl_R * 1000, cb_phase * 1000, stat_blocks, buf);
        }
        fflush(slog);
    }
    double fin = mono();
    if (frame_start_est > 0 && fin - frame_start_est < 0.1) work_est += ((fin - frame_start_est) - work_est) * 0.1;
    start_recording();
}

// Presents are issued while the runner spins on clock_gettime for its next tick: time spent
// there is inside its wait, so the game never slows down as long as we finish before the deadline.
static void maybe_ghost(double now)
{
    if (!prv_ok || !prv->nq || now - swap_ret < 0.001 || now < backoff_until || unfocused) return;
    double deadline = frame_start_est + period_est;
    if (now + ghost_cost + 0.0008 > deadline) return;
    if (vbl_ok) {
        if (now - vbl_last_query > 0.1) vbl_update(now);
        // One present per compositor pickup ("slot"); pickups happen once per refresh at cb_phase.
        // Submit right after the previous frame was taken: a swap issued before that blocks, and
        // early submission leaves almost a whole refresh for the frame to reach the screen.
        double R = vbl_R;
        double cb_last = vbl_t + cb_phase + floor((now - vbl_t - cb_phase) / R) * R;
        double cb_next = cb_last + R;
        double V = cb_next; // time axis for content: one present per pickup, R apart
        if (V <= last_V + R * 0.5) return;                    // this slot already has a frame
        int step = (int)ceil(cfg_min_interval / R - 0.05);    // fps cap on screens faster than it
        if (step > 1 && last_V > 0 && V - last_V < step * R - R * 0.5) return;
        if (now < cb_last + 0.0004) return;                   // previous frame not taken yet
        if (now > cb_next - 0.0025) { last_V = V; return; }   // too late in the slot, skip it
        present_state(V, 1, 1);
    } else {
        if (now - last_present < cfg_min_interval - 0.0005) return;
        present_state(now, 0, 1);
    }
}

int clock_gettime(clockid_t c, struct timespec *ts)
{
    RESOLVE();
    int r = r_clock_gettime(c, ts);
    if (waiting && !in_replay && !in_present && have_main && pthread_equal(pthread_self(), main_thread)) {
        double now = c == CLOCK_MONOTONIC ? ts->tv_sec + ts->tv_nsec * 1e-9 : mono();
        last_wait_seen = now;
        if (now >= next_check) {
            next_check = now + 0.0003;
            maybe_ghost(now);
        }
    }
    return r;
}
