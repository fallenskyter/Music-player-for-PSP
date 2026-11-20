/*
 * SOFT CLUB PLAYER - MP3 player for PSP (custom firmware required)
 * Gen X / soft club vibe, switchable themes.
 *
 * Music folder: ms0:/MUSIC/   (MP3 files, no subfolders)
 *
 * Controls:
 *   UP / DOWN     choose track
 *   CROSS         play selected track (or pause if it is already playing)
 *   CIRCLE        play / pause
 *   L / R         previous / next track
 *   LEFT / RIGHT  volume down / up
 *   TRIANGLE      next theme
 *   SQUARE        previous theme
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspaudio.h>
#include <pspmp3.h>
#include <psputility.h>
#include <psppower.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

PSP_MODULE_INFO("SoftClub", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

#define MUSIC_DIR   "ms0:/MUSIC"
#define CFG_PATH    "ms0:/PSP/GAME/SoftClub/theme.cfg"
#define MAX_TRACKS  512
#define SCR_W       480
#define SCR_H       272
#define STRIDE      512
#define VISIBLE     10
#define ROW_H       13

#define C(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))

/* ------------------------------------------------------------------ */
/* THEMES - add your own here, everything else adapts automatically    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    u32 bg_top, bg_bot;      /* background gradient                  */
    u32 panel; int panel_a;  /* glass panel colour + opacity (0-255) */
    u32 edge;                /* panel border colour                  */
    u32 text, dim;           /* main / secondary text                */
    u32 accent, accent2;     /* progress bar, labels                 */
    u32 sel_bg, sel_text;    /* selected row                         */
    u32 bar_bg;              /* empty bar background                 */
    u32 bubble; int bubbles; /* soft translucent bubbles             */
    int grain;               /* film grain strength (0 = off)        */
    int scan;                /* CRT scanlines (0/1)                  */
    int gloss;               /* glossy highlight on panels (0/1)     */
} Theme;

static const Theme THEMES[] = {
    { "SOFT CLUB",
      C(150,214,226), C(206,186,230),
      C(244,250,255), 170, C(255,255,255),
      C(38,62,96),  C(98,120,150),
      C(40,160,190), C(224,110,160),
      C(70,176,205), C(255,255,255),
      C(190,214,230),
      C(255,255,255), 7, 0, 0, 1 },

    { "GEN X",
      C(66,58,46), C(28,25,22),
      C(92,82,66), 200, C(210,170,90),
      C(232,216,176), C(160,144,112),
      C(222,168,52), C(196,88,52),
      C(176,78,48), C(250,236,200),
      C(48,43,36),
      C(255,255,255), 0, 14, 1, 0 },

    { "MIDNIGHT LOUNGE",
      C(16,18,56), C(70,22,88),
      C(30,32,84), 190, C(110,120,220),
      C(204,222,255), C(124,136,190),
      C(70,226,234), C(236,76,176),
      C(120,40,160), C(255,255,255),
      C(18,20,52),
      C(120,200,255), 5, 4, 0, 1 },

    { "FROSTED SILVER",
      C(248,249,252), C(184,192,206),
      C(255,255,255), 190, C(255,255,255),
      C(46,52,68), C(120,128,142),
      C(64,116,224), C(140,150,168),
      C(64,116,224), C(255,255,255),
      C(200,206,218),
      C(255,255,255), 4, 0, 0, 1 },

    { "PEACH FIZZ",
      C(255,206,176), C(236,148,176),
      C(255,244,238), 175, C(255,255,255),
      C(104,44,60), C(170,110,120),
      C(232,96,120), C(250,160,80),
      C(232,110,130), C(255,255,255),
      C(246,206,200),
      C(255,255,255), 8, 0, 0, 1 },
};
#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

static int g_theme = 0;

/* ------------------------------------------------------------------ */
/* Tracks                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[300];
    char disp[64];
} Track;

static Track g_tracks[MAX_TRACKS];
static int g_count = 0;

static const char *RU[32] = {
    "A","B","V","G","D","E","Zh","Z","I","Y","K","L","M","N","O","P",
    "R","S","T","U","F","Kh","Ts","Ch","Sh","Sch","","Y","'","E","Yu","Ya"
};

static void put_str(char *out, int *n, int max, const char *s, int lower)
{
    while (*s && *n < max - 1) {
        out[(*n)++] = lower ? (char)tolower((unsigned char)*s) : *s;
        s++;
    }
}

/* Debug font has no Cyrillic, so transliterate UTF-8 Russian to Latin */
static void make_display(const char *name, char *out, int max)
{
    char tmp[300];
    strncpy(tmp, name, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = 0;

    int n = 0;
    const unsigned char *p = (const unsigned char *)tmp;
    while (*p && n < max - 1) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (c >= 32) out[n++] = (char)c;
            p++;
        } else if ((c == 0xD0 || c == 0xD1) && p[1]) {
            unsigned char c2 = p[1];
            int idx = -1, up = 0;
            if (c == 0xD0) {
                if (c2 >= 0x90 && c2 <= 0xAF)      { idx = c2 - 0x90; up = 1; }
                else if (c2 >= 0xB0 && c2 <= 0xBF) { idx = c2 - 0xB0; }
                else if (c2 == 0x81)               { put_str(out, &n, max, "Yo", 0); p += 2; continue; }
            } else {
                if (c2 >= 0x80 && c2 <= 0x8F)      { idx = c2 - 0x80 + 16; }
                else if (c2 == 0x91)               { put_str(out, &n, max, "yo", 0); p += 2; continue; }
            }
            if (idx >= 0) {
                if (up) {
                    put_str(out, &n, max, RU[idx], 0);
                } else {
                    put_str(out, &n, max, RU[idx], 1);
                }
            } else {
                out[n++] = '?';
            }
            p += 2;
        } else {
            int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
            out[n++] = '?';
            for (int i = 0; i < len && *p; i++) p++;
        }
    }
    out[n] = 0;
}

static int cmp_tracks(const void *a, const void *b)
{
    return strcasecmp(((const Track *)a)->path, ((const Track *)b)->path);
}

static void scan_tracks(void)
{
    SceUID d = sceIoDopen(MUSIC_DIR);
    if (d < 0) return;

    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && g_count < MAX_TRACKS) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
            const char *dot = strrchr(e.d_name, '.');
            if (dot && strcasecmp(dot, ".mp3") == 0) {
                Track *t = &g_tracks[g_count++];
                snprintf(t->path, sizeof(t->path), "%s/%s", MUSIC_DIR, e.d_name);
                make_display(e.d_name, t->disp, sizeof(t->disp));
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    qsort(g_tracks, g_count, sizeof(Track), cmp_tracks);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static u32 *vram;
static u32 g_bg[STRIDE * SCR_H];   /* pre-rendered background + panels */
static u32 g_seed = 12345;

static u32 rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static inline u32 blend(u32 d, u32 s, int a)
{
    int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255;
    int sr = s & 255, sg = (s >> 8) & 255, sb = (s >> 16) & 255;
    dr += (sr - dr) * a / 255;
    dg += (sg - dg) * a / 255;
    db += (sb - db) * a / 255;
    return 0xFF000000u | ((u32)db << 16) | ((u32)dg << 8) | (u32)dr;
}

static void rrect(u32 *b, int x, int y, int w, int h, int r, u32 c, int a)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= SCR_H) continue;
        int dy = 0;
        if (row < r)            dy = r - row;
        else if (row >= h - r)  dy = row - (h - r) + 1;
        int inset = 0;
        if (dy > 0) {
            float d = (float)dy - 0.5f;
            inset = (int)((float)r - sqrtf((float)(r * r) - d * d) + 0.5f);
        }
        int x0 = x + inset, x1 = x + w - inset;
        if (x0 < 0) x0 = 0;
        if (x1 > SCR_W) x1 = SCR_W;
        u32 *line = b + py * STRIDE;
        if (a >= 255) {
            for (int px = x0; px < x1; px++) line[px] = c;
        } else {
            for (int px = x0; px < x1; px++) line[px] = blend(line[px], c, a);
        }
    }
}

static void bubble(u32 *b, int cx, int cy, int r, u32 c, int a)
{
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= SCR_H) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= SCR_W) continue;
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            int rim = (d2 >= (r - 2) * (r - 2));
            /* soft shine in upper-left */
            int shine = (dx < 0 && dy < 0 && d2 < (r * r) / 3);
            int aa = a + (rim ? a : 0) + (shine ? a / 2 : 0);
            if (aa > 255) aa = 255;
            b[y * STRIDE + x] = blend(b[y * STRIDE + x], c, aa);
        }
    }
}

static void render_theme(void)
{
    const Theme *t = &THEMES[g_theme];

    /* vertical gradient */
    int tr = t->bg_top & 255, tg = (t->bg_top >> 8) & 255, tb = (t->bg_top >> 16) & 255;
    int br = t->bg_bot & 255, bg = (t->bg_bot >> 8) & 255, bb = (t->bg_bot >> 16) & 255;
    for (int y = 0; y < SCR_H; y++) {
        int r = tr + (br - tr) * y / (SCR_H - 1);
        int g = tg + (bg - tg) * y / (SCR_H - 1);
        int b = tb + (bb - tb) * y / (SCR_H - 1);
        u32 col = C(r, g, b);
        for (int x = 0; x < STRIDE; x++) g_bg[y * STRIDE + x] = col;
    }

    /* bubbles */
    g_seed = 777;
    for (int i = 0; i < t->bubbles; i++) {
        int cx = rnd() % SCR_W;
        int cy = rnd() % SCR_H;
        int r  = 14 + rnd() % 34;
        bubble(g_bg, cx, cy, r, t->bubble, 28);
    }

    /* film grain */
    if (t->grain > 0) {
        int gr = t->grain;
        for (int y = 0; y < SCR_H; y++) {
            for (int x = 0; x < SCR_W; x++) {
                u32 p = g_bg[y * STRIDE + x];
                int n = (int)(rnd() % (2 * gr + 1)) - gr;
                int r = (p & 255) + n, g = ((p >> 8) & 255) + n, b = ((p >> 16) & 255) + n;
                if (r < 0) r = 0; if (r > 255) r = 255;
                if (g < 0) g = 0; if (g > 255) g = 255;
                if (b < 0) b = 0; if (b > 255) b = 255;
                g_bg[y * STRIDE + x] = C(r, g, b);
            }
        }
    }

    /* CRT scanlines */
    if (t->scan) {
        for (int y = 0; y < SCR_H; y += 2)
            for (int x = 0; x < SCR_W; x++)
                g_bg[y * STRIDE + x] = blend(g_bg[y * STRIDE + x], C(0,0,0), 36);
    }

    /* glass panels: now playing + list */
    rrect(g_bg, 10, 20, 460, 76, 10, t->edge, 120);
    rrect(g_bg, 11, 21, 458, 74, 9, t->panel, t->panel_a);
    rrect(g_bg, 10, 102, 460, 144, 10, t->edge, 120);
    rrect(g_bg, 11, 103, 458, 142, 9, t->panel, t->panel_a);

    if (t->gloss) {
        rrect(g_bg, 16, 24, 448, 9, 4, C(255,255,255), 45);
        rrect(g_bg, 16, 106, 448, 9, 4, C(255,255,255), 30);
    }
}

static void text(int x, int y, u32 col, const char *s)
{
    while (*s) {
        pspDebugScreenPutChar(x, y, col, (unsigned char)*s++);
        x += 8;
    }
}

static void text_bold(int x, int y, u32 col, const char *s)
{
    text(x, y, col, s);
    text(x + 1, y, col, s);
}

static void text_clip(int x, int y, u32 col, const char *s, int maxc, int bold)
{
    char buf[80];
    if (maxc > 78) maxc = 78;
    strncpy(buf, s, maxc);
    buf[maxc] = 0;
    if (bold) text_bold(x, y, col, buf); else text(x, y, col, buf);
}

static void fmt_time(char *out, int ms)
{
    int s = ms / 1000;
    sprintf(out, "%02d:%02d", s / 60, s % 60);
}

/* ------------------------------------------------------------------ */
/* Audio (hardware MP3 decoder, runs in its own thread)                */
/* ------------------------------------------------------------------ */

static volatile int g_load_idx = -1;   /* main -> audio: load this track  */
static volatile int g_playing  = 0;
static volatile int g_ended    = 0;    /* audio -> main: track finished   */
static volatile int g_err      = 0;
static volatile int g_pos_ms   = 0;
static volatile int g_dur_ms   = 0;
static volatile int g_volume   = 14;   /* 0..20 */

typedef struct {
    int fd, h, src;
    int chans, rate, maxs;
    int eof;
    unsigned int frames;
    int dur_ms;
} Dec;

static unsigned char mp3Buf[16 * 1024] __attribute__((aligned(64)));
static unsigned char pcmBuf[16 * 1024] __attribute__((aligned(64)));
static short outBuf[4608] __attribute__((aligned(64)));

static void dec_close(Dec *d)
{
    if (d->src >= 0) { sceAudioSRCChRelease(); d->src = -1; }
    if (d->h >= 0)   { sceMp3ReleaseMp3Handle(d->h); d->h = -1; }
    if (d->fd >= 0)  { sceIoClose(d->fd); d->fd = -1; }
}

static int fill_stream(Dec *d)
{
    unsigned char *dst;
  SceInt32 towrite, srcpos;
    if (sceMp3GetInfoToAddStreamData(d->h, &dst, &towrite, &srcpos) < 0) return -1;
    sceIoLseek32(d->fd, srcpos, PSP_SEEK_SET);
    int r = sceIoRead(d->fd, dst, towrite);
    if (r <= 0) { d->eof = 1; return 0; }
    sceMp3NotifyAddStreamData(d->h, r);
    return r;
}

static int dec_open(Dec *d, const char *path)
{
    d->fd = d->h = d->src = -1;
    d->eof = 0;
    d->frames = 0;

    d->fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (d->fd < 0) return -1;

    int size = (int)sceIoLseek32(d->fd, 0, PSP_SEEK_END);
    sceIoLseek32(d->fd, 0, PSP_SEEK_SET);

    /* skip ID3v2 tag */
    int start = 0;
    unsigned char hdr[10];
    if (sceIoRead(d->fd, hdr, 10) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        int tag = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                  ((hdr[8] & 0x7F) << 7)  |  (hdr[9] & 0x7F);
        start = 10 + tag + ((hdr[5] & 0x10) ? 10 : 0);
    }
    sceIoLseek32(d->fd, 0, PSP_SEEK_SET);

    SceMp3InitArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.mp3StreamStart = start;
    arg.mp3StreamEnd   = size;
    arg.mp3Buf         = mp3Buf;
    arg.mp3BufSize     = sizeof(mp3Buf);
    arg.pcmBuf         = pcmBuf;
    arg.pcmBufSize     = sizeof(pcmBuf);

    d->h = sceMp3ReserveMp3Handle(&arg);
    if (d->h < 0) { dec_close(d); return -1; }

    fill_stream(d);
    if (sceMp3Init(d->h) < 0) { dec_close(d); return -1; }

    d->rate  = sceMp3GetSamplingRate(d->h);
    d->chans = sceMp3GetMp3ChannelNum(d->h);
    d->maxs  = sceMp3GetMaxOutputSample(d->h);
    if (d->maxs <= 0 || d->maxs > 1152) d->maxs = 1152;
    int kbps = sceMp3GetBitRate(d->h);
    d->dur_ms = (kbps > 0) ? (int)(((long long)(size - start) * 8) / kbps) : 0;

    d->src = sceAudioSRCChReserve(d->maxs, d->rate, 2);
    if (d->src < 0) { dec_close(d); return -1; }
    return 0;
}

static int audio_thread(SceSize args, void *argp)
{
    Dec d;
    d.fd = d.h = d.src = -1;

    for (;;) {
        int idx = g_load_idx;
        if (idx >= 0) {
            g_load_idx = -1;
            dec_close(&d);
            if (dec_open(&d, g_tracks[idx].path) == 0) {
                g_pos_ms = 0;
                g_dur_ms = d.dur_ms;
                g_err = 0;
            } else {
                g_err = 1;
                g_playing = 0;
            }
            continue;
        }

        if (d.h < 0 || !g_playing) {
            sceKernelDelayThread(20000);
            continue;
        }

        if (sceMp3CheckStreamDataNeeded(d.h) > 0) fill_stream(&d);

        short *pcm = NULL;
        int bytes = sceMp3Decode(d.h, &pcm);
        if (bytes <= 0 || !pcm) {
            dec_close(&d);
            g_ended = 1;
            continue;
        }

        int want = d.maxs * 2;               /* shorts for a full stereo frame */
        int n = 0;
        if (d.chans == 1 && bytes <= d.maxs * 2) {
            int fr = bytes / 2;              /* mono -> stereo */
            for (int i = 0; i < fr && n + 1 < want; i++) {
                outBuf[n++] = pcm[i];
                outBuf[n++] = pcm[i];
            }
            d.frames += fr;
        } else {
            int sh = bytes / 2;
            if (sh > want) sh = want;
            memcpy(outBuf, pcm, sh * 2);
            n = sh;
            d.frames += sh / 2;
        }
        while (n < want) outBuf[n++] = 0;

        int v = g_volume;
        sceAudioSRCOutputBlocking(v * v * PSP_AUDIO_VOLUME_MAX / 400, outBuf);
        g_pos_ms = (int)((unsigned long long)d.frames * 1000ULL / (unsigned)d.rate);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Config + system                                                     */
/* ------------------------------------------------------------------ */

static void load_cfg(void)
{
    SceUID f = sceIoOpen(CFG_PATH, PSP_O_RDONLY, 0777);
    if (f < 0) return;
    char c = '0';
    if (sceIoRead(f, &c, 1) == 1 && c >= '0' && c < '0' + THEME_COUNT) g_theme = c - '0';
    sceIoClose(f);
}

static void save_cfg(void)
{
    SceUID f = sceIoOpen(CFG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0) return;
    char c = (char)('0' + g_theme);
    sceIoWrite(f, &c, 1);
    sceIoClose(f);
}

static int exit_callback(int a1, int a2, void *c)
{
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cb = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int th = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static int g_sel = 0, g_top = 0, g_cur = -1;

typedef struct { int sel, top, cur, playing, sec, vol, theme, err, count; } UiState;

static void draw_ui(void)
{
    const Theme *t = &THEMES[g_theme];
    char buf[96];

    memcpy(vram, g_bg, sizeof(g_bg));

    /* header */
    text_bold(14, 6, t->text, "SOFT CLUB PLAYER");
    sprintf(buf, "%s  %d/%d", t->name, g_theme + 1, THEME_COUNT);
    text(SCR_W - 14 - (int)strlen(buf) * 8, 6, t->dim, buf);

    /* now playing panel */
    const char *state = g_err ? "ERROR - CAN'T PLAY FILE"
                      : (g_cur < 0) ? "STOPPED"
                      : g_playing ? "NOW PLAYING" : "PAUSED";
    text_bold(22, 38, t->accent2, state);
    if (g_count > 0) {
        sprintf(buf, "%d/%d", (g_cur >= 0 ? g_cur : g_sel) + 1, g_count);
        text(SCR_W - 22 - (int)strlen(buf) * 8, 38, t->dim, buf);
    }

    if (g_cur >= 0)
        text_clip(22, 51, t->text, g_tracks[g_cur].disp, 52, 1);
    else
        text(22, 51, t->dim, "Pick a track and press X");

    int pos = g_pos_ms, dur = g_dur_ms;
    char a[16], b[16];
    fmt_time(a, pos);
    fmt_time(b, dur);
    sprintf(buf, "%s / %s", a, b);
    text(22, 64, t->text, buf);

    /* volume */
    text(318, 64, t->dim, "VOL");
    rrect(vram, 350, 66, 108, 6, 3, t->bar_bg, 255);
    int vw = g_volume * 108 / 20;
    if (vw > 5) rrect(vram, 350, 66, vw, 6, 3, t->accent2, 255);

    /* progress */
    rrect(vram, 22, 78, 436, 8, 4, t->bar_bg, 255);
    if (dur > 0) {
        int pw = (int)((long long)pos * 436 / dur);
        if (pw > 436) pw = 436;
        if (pw > 8) rrect(vram, 22, 78, pw, 8, 4, t->accent, 255);
    }

    /* list */
    if (g_count == 0) {
        text(24, 150, t->text, "No MP3 files found in ms0:/MUSIC/");
    } else {
        for (int i = 0; i < VISIBLE; i++) {
            int idx = g_top + i;
            if (idx >= g_count) break;
            int y = 112 + i * ROW_H;
            u32 col = t->text;
            if (idx == g_sel) {
                rrect(vram, 16, y - 3, 436, 13, 5, t->sel_bg, 255);
                col = t->sel_text;
            }
            text(24, y, (idx == g_cur && idx != g_sel) ? t->accent2 : col,
                 (idx == g_cur) ? ">" : " ");
            text_clip(40, y, col, g_tracks[idx].disp, 50, 0);
        }
        /* scrollbar */
        if (g_count > VISIBLE) {
            rrect(vram, 459, 110, 4, 128, 2, t->bar_bg, 255);
            int th = 128 * VISIBLE / g_count;
            if (th < 10) th = 10;
            int ty = 110 + (128 - th) * g_top / (g_count - VISIBLE);
            rrect(vram, 459, ty, 4, th, 2, t->accent, 255);
        }
    }

    /* footer */
    const char *hint = "X:Play O:Pause L/R:Skip <>:Vol TRI/SQ:Theme";
    text((SCR_W - (int)strlen(hint) * 8) / 2, 254, t->dim, hint);
}

static void play_track(int idx)
{
    if (idx < 0 || idx >= g_count) return;
    g_cur = idx;
    g_ended = 0;
    g_err = 0;
    g_pos_ms = 0;
    g_dur_ms = 0;
    g_playing = 1;
    g_load_idx = idx;
}

static void select_track(int idx)
{
    if (g_count == 0) return;
    if (idx < 0) idx = g_count - 1;
    if (idx >= g_count) idx = 0;
    g_sel = idx;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + VISIBLE) g_top = g_sel - VISIBLE + 1;
}

static int rep(u32 btn, u32 now, u32 old, int *cnt)
{
    if (now & btn) {
        (*cnt)++;
        if (!(old & btn)) return 1;
        if (*cnt > 18 && (*cnt % 4) == 0) return 1;
    } else {
        *cnt = 0;
    }
    return 0;
}

static void set_theme(int t)
{
    g_theme = (t + THEME_COUNT) % THEME_COUNT;
    render_theme();
    save_cfg();
}

int main(int argc, char *argv[])
{
    setup_callbacks();
    scePowerSetClockFrequency(222, 222, 111);

    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);
    vram = (u32 *)(0x40000000 | (u32)sceGeEdramGetAddr());

    sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    sceMp3InitResource();

    load_cfg();
    render_theme();
    scan_tracks();

    int th = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    SceCtrlData pad;
    u32 old = 0;
    int c_up = 0, c_dn = 0, c_lf = 0, c_rt = 0;
    UiState last;
    memset(&last, 0xFF, sizeof(last));

    for (;;) {
        sceCtrlPeekBufferPositive(&pad, 1);
        u32 now = pad.Buttons;
        u32 pressed = now & ~old;

        if (rep(PSP_CTRL_UP, now, old, &c_up))   select_track(g_sel - 1);
        if (rep(PSP_CTRL_DOWN, now, old, &c_dn)) select_track(g_sel + 1);
        if (rep(PSP_CTRL_LEFT, now, old, &c_lf)  && g_volume > 0)  g_volume--;
        if (rep(PSP_CTRL_RIGHT, now, old, &c_rt) && g_volume < 20) g_volume++;

        if (pressed & PSP_CTRL_CROSS) {
            if (g_sel == g_cur && !g_err) g_playing = !g_playing;
            else play_track(g_sel);
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            if (g_cur >= 0) g_playing = !g_playing;
            else play_track(g_sel);
        }
        if ((pressed & PSP_CTRL_RTRIGGER) && g_count > 0) {
            int n = (g_cur < 0 ? g_sel : g_cur) + 1;
            if (n >= g_count) n = 0;
            play_track(n); select_track(n);
        }
        if ((pressed & PSP_CTRL_LTRIGGER) && g_count > 0) {
            int n = (g_cur < 0 ? g_sel : g_cur) - 1;
            if (n < 0) n = g_count - 1;
            play_track(n); select_track(n);
        }
        if (pressed & PSP_CTRL_TRIANGLE) set_theme(g_theme + 1);
        if (pressed & PSP_CTRL_SQUARE)   set_theme(g_theme - 1);

        if (g_ended) {
            g_ended = 0;
            if (g_count > 0) {
                int n = (g_cur + 1) % g_count;
                play_track(n); select_track(n);
            }
        }

        old = now;

        UiState s = { g_sel, g_top, g_cur, g_playing, g_pos_ms / 1000,
                      g_volume, g_theme, g_err, g_count };
        sceDisplayWaitVblankStart();
        if (memcmp(&s, &last, sizeof(s)) != 0) {
            draw_ui();
            last = s;
        }
    }
    return 0;
}
