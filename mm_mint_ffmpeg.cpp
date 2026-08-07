/*
 * mm_mint_ffmpeg.cpp
 */

#include <limits.h>
#if ULONG_MAX == 4294967295UL
#  define UINT64_C(n)  n ## ULL
#else
#  define UINT64_C(n)  n ## UL
#endif
#ifndef UINT64_MAX
#  define UINT64_MAX (UINT64_C(18446744073709551615))
#endif
#ifndef INT64_MAX
#  define INT64_MAX   0x7fffffffffffffffLL
#endif
#ifndef INT64_MIN
#  define INT64_MIN  (-INT64_MAX - 1LL)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <mint/cookie.h>
#include <mint/falcon.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gem.h>
#include <gemx.h>
#include <pthread.h>
#include <png.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}

#define TRUE  1
#define FALSE 0
#ifndef UCHAR_MAX
#  define UCHAR_MAX 255
#endif

#define MFDB_STRIDE(w)   (((w) + 15) & -16)
#define MIN(a,b)         ((a) <= (b) ? (a) : (b))
#define MAX(a,b)         ((a) >= (b) ? (a) : (b))

#define FPS_MAX_DELTA_DIV 4
#define AUDIO_LATENCY_COMPENSATION 1500 

#define VOL_SHIFT_MIN 0
#define VOL_SHIFT_MAX 8
#define VOL_SHIFT_STEP 1

#define BYTES_TO_CHECK          8
#define ICO_PATH_PLAY           "./ico/play.png"
#define ICO_PATH_PAUSE          "./ico/pause.png"
#define ICO_PATH_STOP           "./ico/go_start.png"
#define ICO_PATH_VOLUME         "./ico/volume.png"
#define ICO_PATH_MUTE           "./ico/mute.png"
#define ICO_PATH_FFMPEG_LOGO    "./ico/ffmpeg_logo.png"

#define SPLASH_GREY_R           0xD0
#define SPLASH_GREY_G           0xD0
#define SPLASH_GREY_B           0xD0

#define AP_MSG_TIME_UPDATE      0x4700

static bool g_disable_resample = false;
static bool g_direct_play = false;
static volatile bool g_restart_requested = false;

#define SHADOW_BUF_SIZE 128   /* CHANGED: was 32 */
static AVPacket* g_audio_shadow[SHADOW_BUF_SIZE];
static volatile int g_shadow_head = 0;
static volatile int g_shadow_tail = 0;
static pthread_mutex_t g_shadow_mutex = PTHREAD_MUTEX_INITIALIZER;

#define VID_BUF_SIZE 24
static uint8_t *g_vid_ring_buf = NULL;
static int64_t  g_vid_pts_buf[VID_BUF_SIZE];
static int      g_vid_buf_size = 0;
static volatile int g_vid_head = 0;
static volatile int g_vid_tail = 0;

static int16_t g_vid_off_x = 0;
static int16_t g_vid_off_y = 0;

static volatile bool g_fullscreen = false;
static volatile bool g_fs_requested = false;
static int16_t g_saved_x, g_saved_y, g_saved_w, g_saved_h;
static int16_t fullscreen_lock_count = 0;

#ifndef __GEMLIB_AES
extern int16_t gl_apid;
#else
int16_t gl_apid;
#endif

int16_t gl_hchar, gl_wchar, gl_wbox, gl_hbox;
int16_t phys_handle, handle, wi_handle;
int16_t xdesk, ydesk, hdesk, wdesk;
int16_t xwork, ywork, hwork, wwork;
int16_t xext, yext, hext, wext;
int16_t msgbuff[8];
int16_t keycode;
int16_t mx, my, mb, mc;
int16_t button_is_down;
int16_t dummy_rv;
int16_t cursor_is_hidden;
int16_t work_in[11];
int16_t work_out[57];
int16_t work_out_extended[57];

const char *win_title = NULL;

MFDB mm_wi_mfdb = { 0 };
MFDB screen_mfdb = { 0 };

volatile bool app_end = false;

static int32_t get200hz(void) { return *((volatile int32_t *)0x000004BA); }
static int32_t st_Supexec(int32_t (*fn)(void)) { return (int32_t)Supexec(fn); }
static void enableTimerASei(void) { *((volatile unsigned char*)0xFFFFFA17L) |= (1<<3); }
static void st_enableTimerASei(void) { Supexec(enableTimerASei); }

static volatile int16_t  loadNewSample = 0;
static int16_t           attenuation_left, attenuation_right;
static volatile int g_vol_shift = 0;
static volatile bool g_demux_finished = false;

typedef struct {
    int      src_samplerate;
    int      src_channels;
    AVSampleFormat src_fmt;
    int16_t  clk_prescale;
    int32_t  atari_effective_sr;
    int32_t  wanted_sr;
    int32_t  bufferSize;
    int8_t  *pBuffer;
    int8_t  *pPhysical;
    int8_t  *pLogical;
    volatile uint32_t feed_count;
    SwrContext *swr_ctx;
    uint8_t    *swr_tmp_buf;
    AVCodecContext *codec_ctx;
    AVFormatContext *fmt_ctx;
    AVFrame  *frame;
    int       stream_index;
    double   duration_s;
    bool     is_playing;
    bool     is_paused;
    bool     is_muted;
    bool     dma_started; 
    int16_t  clk_source;   /* NEW: CLK25M, CLKEXT, CLKOLD */
    int16_t  setpre_val;   /* NEW: -1 or PRE160/PRE320/… */      
} snd_state_t;

static snd_state_t g_snd;

typedef struct {
    AVCodecContext  *codec_ctx;
    AVFormatContext *fmt_ctx;
    AVFrame         *frame_yuv;
    AVFrame         *frame_rgb;
    AVPacket        *packet;
    SwsContext      *sws_ctx;
    int              stream_index;
    uint32_t  frame_counter;
    uint32_t  frames_dropped;
    uint32_t  total_frames;
    double    fps;
    double    time_per_frame;
    double    time_per_frame_wall;
    uint32_t  drop_thresh_samples;
    uint16_t  width;
    uint16_t  height;
    AVPixelFormat screen_pix_fmt;
    int       screen_bpp;
    bool      is_playing;
    bool      is_paused;
} vid_state_t;

static vid_state_t g_vid;

static MFDB g_splash_mfdb = { 0 };
static volatile bool g_splash_active = false;
static volatile bool g_playback_started = false;
static volatile bool g_audio_only = false;

static char g_bin_dir[512] = {0};

typedef struct {
    MFDB mfdb;
    int16_t x, y, x2, y2;
} ico_png_t;

typedef struct {
    int16_t  id;
    const char *main_path;
    const char *alt_path;
    ico_png_t  *main_ico;
    ico_png_t  *alt_ico;
    void       (*handler)(void);
    int16_t    pos_x, pos_y;
    bool       alt_state;
} ico_entry_t;

static ico_png_t ico_play     = {0};
static ico_png_t ico_pause    = {0};
static ico_png_t ico_stop     = {0};
static ico_png_t ico_volume   = {0};
static ico_png_t ico_mute     = {0};
static ico_png_t ico_ffmpeg   = {0};

static ico_entry_t control_bar[] = {
    { 1, ICO_PATH_PAUSE, ICO_PATH_PLAY, &ico_pause, &ico_play, NULL, 32,  4, false },
    { 2, ICO_PATH_STOP,  NULL,          &ico_stop,  NULL,      NULL, 70,  4, false },
    { 3, ICO_PATH_VOLUME,ICO_PATH_MUTE, &ico_volume,&ico_mute, NULL, 106, 4, false },
    { -1, NULL, NULL, NULL, NULL, NULL, 0, 0, false }
};

static int16_t mm_ico_win_delta_y = 0;
static int16_t mm_ico_pxy_control_bar[4];
static MFDB mm_control_bar_mfdb = { 0 };

static int16_t g_machine_type = 3;   /* default Falcon */

/* ------------------------------------------------------------------ */
/*  Tables de fréquences exactes par machine                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t  freq;
    int16_t  prescale;
    int16_t  clk;
    int16_t  setpre;        /* -1 = pas de SETPRESCALE, sinon PRE160.. */
} freq_entry_t;

/* ST/E : 6258, 12517, 25033, 50066 */
static const freq_entry_t freq_ste[] = {
    { 50066, CLKOLD, CLK25M, PRE160  },
    { 25033, CLKOLD, CLK25M, PRE320  },
    { 12517, CLKOLD, CLK25M, PRE640  },
    {  6258, CLKOLD, CLK25M, PRE1280 },
};

/* TT : 6292, 12584, 25169, 50352 */
static const freq_entry_t freq_tt[] = {
    { 50352, CLKOLD, CLK25M, PRE160  },
    { 25169, CLKOLD, CLK25M, PRE320  },
    { 12584, CLKOLD, CLK25M, PRE640  },
    {  6292, CLKOLD, CLK25M, PRE1280 },
};

/* Falcon interne : 8195 .. 49170 */
static const freq_entry_t freq_falcon[] = {
    { 49170, CLK50K, CLK25M, -1 },
    { 32780, CLK33K, CLK25M, -1 },
    { 24585, CLK25K, CLK25M, -1 },
    { 19668, CLK20K, CLK25M, -1 },
    { 16390, CLK16K, CLK25M, -1 },
    { 12292, CLK12K, CLK25M, -1 },
    {  9834, CLK10K, CLK25M, -1 },
    {  8195, CLK8K,  CLK25M, -1 },
};

/* ARAnyM : 12300, 24594, 49165 */
static const freq_entry_t freq_aranym[] = {
    { 49165, CLK50K, CLK25M, -1 },
    { 24594, CLK25K, CLK25M, -1 },
    { 12300, CLK10K, CLK25M, -1 },
};

/* Vampire V4SA interne */
static const freq_entry_t freq_vampire[] = {
    { 50667, CLK50K, CLK25M, -1 },
    { 25335, CLK25K, CLK25M, -1 },
    { 12273, CLK12K, CLK25M, -1 },
};

/* Vampire V4SA CD/44.1k */
static const freq_entry_t freq_vampire_cd[] = {
    { 44186, CLK50K, CLKEXT, -1 },
    { 22094, CLK25K, CLKEXT, -1 },
    { 11047, CLK12K, CLKEXT, -1 },
};

/* Milan / FireBee 44.1k */
static const freq_entry_t freq_ext44[] = {
    { 44100, CLK50K, CLKEXT, -1 },
    { 29400, CLK33K, CLKEXT, -1 },
    { 22050, CLK25K, CLKEXT, -1 },
    { 17640, CLK20K, CLKEXT, -1 },
    { 14700, CLK16K, CLKEXT, -1 },
    { 11025, CLK12K, CLKEXT, -1 },
    {  8820, CLK10K, CLKEXT, -1 },
    {  7350, CLK8K,  CLKEXT, -1 },
};

/* Milan / FireBee 48k */
static const freq_entry_t freq_ext48[] = {
    { 48000, CLK50K, CLKEXT, -1 },
    { 32000, CLK33K, CLKEXT, -1 },
    { 24000, CLK25K, CLKEXT, -1 },
    { 19200, CLK20K, CLKEXT, -1 },
    { 16000, CLK16K, CLKEXT, -1 },
    { 12000, CLK12K, CLKEXT, -1 },
    {  9600, CLK10K, CLKEXT, -1 },
    {  8000, CLK8K,  CLKEXT, -1 },
};

static void hide_mouse(void);
static void show_mouse(void);
static void open_vwork(void);
static void set_clip(int16_t flag, int16_t x, int16_t y, int16_t w, int16_t h);
static void open_window(int win_width, int win_height, const char *title);
static void *exec_eventloop(void *p);
static void *st_Win_Redraw(void *p);
static void  st_Send_WM_REDRAW(void);

static void  snd_compute_prescale(void);
static void  snd_fill_logical(void);
static void  snd_init_dma(void);
static void  snd_unset_dma(void);
static void  snd_feed(void);
static int32_t snd_get_playback_pos(void);
static void *thread_snd_play(void *p);
static void *thread_demux_decode(void *p);
static void *thread_vid_display(void *p);
static void *thread_audio_ui(void *p);

static void  vid_init(const char *filename);
static void  cleanup(void);

static void  normalize_path(char *path);
static const char *get_basename(const char *path);

static void  ico_init(void);
static void  ico_free(void);
static void  ico_decompress(const char *file_name, MFDB *foreground_mfdb);
static void  ico_load(MFDB *dest_mfdb, ico_png_t *ico);
static void  ico_handle(int16_t mouse_x, int16_t mouse_y, int16_t mouse_button);
static void  ico_update_x(uint16_t index, int16_t new_pos_x, int16_t new_pos_y);
static void  ico_draw_bar(void);

static void  splash_init(void);
static void  splash_draw(void);
static void  splash_free(void);
static void  draw_audio_only_time(void);

static void  vol_set_shift(int shift);
static void  vol_up(void);
static void  vol_down(void);
static void  toggle_pause(void);
static void  toggle_mute(void);
static void  restart_playback(void);

static void set_bin_directory(const char *argv0)
{
    char cwd[512];
    g_bin_dir[0] = '\0';

    if (argv0 && argv0[0]) {
        const char *last_slash = strrchr(argv0, '/');
        const char *last_backslash = strrchr(argv0, '\\');
        const char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;

        if (sep) {
            size_t len = (size_t)(sep - argv0) + 1;
            if (len >= sizeof(g_bin_dir)) len = sizeof(g_bin_dir) - 1;
            memcpy(g_bin_dir, argv0, len);
            g_bin_dir[len] = '\0';
            return;
        }

        if (argv0[1] == ':') {
            int drive = toupper((unsigned char)argv0[0]) - 'A' + 1;
            if (Dgetcwd(cwd, drive, sizeof(cwd)) == 0) {
                size_t len = strlen(cwd);
                if (len > 0 && cwd[len - 1] != '/' && cwd[len - 1] != '\\') {
                    if (len < sizeof(cwd) - 1) {
                        cwd[len] = '/';
                        cwd[len + 1] = '\0';
                    }
                }
                strncpy(g_bin_dir, cwd, sizeof(g_bin_dir) - 1);
                g_bin_dir[sizeof(g_bin_dir) - 1] = '\0';
                return;
            }
            snprintf(g_bin_dir, sizeof(g_bin_dir), "%c:/", argv0[0]);
            return;
        }
    }

    if (getcwd(cwd, sizeof(cwd) - 1)) {
        size_t len = strlen(cwd);
        if (len > 0 && cwd[len - 1] != '/' && cwd[len - 1] != '\\') {
            strcat(cwd, "/");
        }
        strncpy(g_bin_dir, cwd, sizeof(g_bin_dir) - 1);
        g_bin_dir[sizeof(g_bin_dir) - 1] = '\0';
    } else {
        strcpy(g_bin_dir, "./");
    }
}

static void detect_machine_type(void)
{
    long mch = 0;
    if (Getcookie(C__MCH, &mch) == C_FOUND) {
        g_machine_type = (int16_t)(mch >> 16);
    }
}

/* ------------------------------------------------------------------ */
/*  Falcon : timing test pour horloge externe                         */
/*  Retourne 0 = pas d'ext, 1 = 44.1k, 2 = 48k                      */
/* ------------------------------------------------------------------ */

/* Fonction appelée via Supexec() — tourne en superviseur */
static int32_t falcon_ext_clock_test_asm(void)
{
    register int32_t ret __asm__("d0");

    __asm__ volatile(
        "   move.w  #0x2500,%%sr          \n"
        "   lea     0xffff8901.w,%%a1     \n"
        "   lea     0x4ba.w,%%a0          \n"
        "   moveq   #2,%%d2               \n"
        "   moveq   #50,%%d1              \n"
        "   add.l   (%%a0),%%d2           \n"
        "   add.l   %%d2,%%d1             \n"
        "tstart%=:                         \n"
        "   cmp.l   (%%a0),%%d2           \n"
        "   bne.s   tstart%=              \n"
        "   move.b  #1,(%%a1)             \n"
        "   nop                             \n"
        "tloop%=:                          \n"
        "   tst.b   (%%a1)                \n"
        "   beq.s   tstop%=               \n"
        "   cmp.l   (%%a0),%%d1           \n"
        "   bne.s   tloop%=               \n"
        "   clr.b   (%%a1)                \n"
        "tstop%=:                          \n"
        "   move.l  (%%a0),%%d0           \n"
        "   sub.l   %%d2,%%d0             \n"
        "   move.w  #0x2300,%%sr          \n"
        : "=d"(ret)
        :
        : "d1", "d2", "a0", "a1", "cc", "memory"
    );
    return ret;
}

/* Configure CLKEXT et retourne recv_pathclk — appelée via Supexec() */
static long falcon_setup_ext_clock(void)
{
    long recvPathclk;

    __asm__ volatile(
        "   and.w   #0x0FFF,0xFFFF8930:w  \n"
        "   or.w    #0x6000,0xFFFF8930:w  \n"
        "   move.w  0xFFFF8932:w,%0       \n"
        : "=d"(recvPathclk)
        :
        : "cc", "memory"
    );

    /* Devconnect(DMAPLAY, DAC, CLKEXT, CLK50K, NO_SHAKE) */
    __asm__ volatile(
        "   move.w  %5,%%sp@-             \n"
        "   move.w  %4,%%sp@-             \n"
        "   move.w  %3,%%sp@-             \n"
        "   move.w  %2,%%sp@-             \n"
        "   move.w  %1,%%sp@-             \n"
        "   move.w  #139,%%sp@-           \n"
        "   move.w  %0,%%d2               \n"
        "   trap    #14                   \n"
        "   lea     12(%%sp),%%sp         \n"
        :
        : "d"(recvPathclk), "i"(DMAPLAY), "i"(DAC),
          "i"(CLKEXT), "i"(CLK50K), "i"(NO_SHAKE)
        : "d1", "d2", "a0", "a1", "a2", "cc", "memory"
    );

    return 0;   /* valeur de retour ignorée */
}

static int falcon_detect_ext_clock(void)
{
    if (g_machine_type != 3)
        return 0;

    const int TEST_BUFSIZE = 8820;
    int8_t *bufs = (int8_t *)Mxalloc(TEST_BUFSIZE, MX_STRAM);
    if ((long)bufs == -32)
        bufs = (int8_t *)Malloc(TEST_BUFSIZE);
    if (!bufs)
        return 0;

    int8_t *bufe = bufs + TEST_BUFSIZE;
    memset(bufs, 0, TEST_BUFSIZE);

    Locksnd();
    Sndstatus(SND_RESET);

    /* CHANGED: tout le setup hardware passe par Supexec() */
    Supexec(falcon_setup_ext_clock);

    Setmode(MODE_MONO);
    Soundcmd(ADDERIN, MATIN);
    Setbuffer(SR_PLAY, bufs, bufe);

    Gpio(GPIO_SET, 0x07);
    Gpio(GPIO_WRITE, 0x03);

    long ticks = Supexec(falcon_ext_clock_test_asm);

    Buffoper(0x00);
    Sndstatus(SND_RESET);
    Unlocksnd();
    Mfree(bufs);

    if (ticks >= 36 && ticks <= 38)
        return 2;   /* 48 kHz */
    if (ticks >= 39 && ticks <= 41)
        return 1;   /* 44.1 kHz */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Milan / Vampire : lecture GPIO                                    */
/* ------------------------------------------------------------------ */

static int milan_vampire_gpio_clock(void)
{
    if (g_machine_type != 4 && g_machine_type != 6)
        return 0;

    long gpio = Gpio(GPIO_READ, SND_INQUIRE);
    /* bit 0 = 0 → 44.1k, bit 0 = 1 → 48k (selon ton strap) */
    return ((gpio & 1L) == 0L) ? 1 : 2;
}

static void fullscreen_lock(void)
{
    wind_update(BEG_UPDATE);   /* verrouille l'écran AES */
    if (fullscreen_lock_count == 0) {
        graf_mouse(M_OFF, NULL);
        cursor_is_hidden = TRUE;
    }
    fullscreen_lock_count++;
    form_dial(FMD_START, 0, 0, 0, 0, xdesk, ydesk, wdesk, hdesk);
}

static void fullscreen_unlock(void)
{
    form_dial(FMD_FINISH, 0, 0, 0, 0, xdesk, ydesk, wdesk, hdesk);
    fullscreen_lock_count--;
    if (fullscreen_lock_count <= 0) {
        fullscreen_lock_count = 0;
        graf_mouse(M_ON, NULL);
        cursor_is_hidden = FALSE;
    }
    wind_update(END_UPDATE);   /* déverrouille l'écran AES */
}

static void hide_mouse(void)
{
    if (fullscreen_lock_count > 0) return;   /* souris déjà verrouillée par fullscreen */
    if (!cursor_is_hidden) {
        graf_mouse(M_OFF, NULL);
        cursor_is_hidden = TRUE;
    }
}

static void show_mouse(void)
{
    if (fullscreen_lock_count > 0) return;   /* garder cachée en plein écran */
    if (cursor_is_hidden) {
        graf_mouse(M_ON, NULL);
        cursor_is_hidden = FALSE;
    }
}

static void vid_compute_fullscreen_offset(void)
{
    g_vid_off_x = 0;
    g_vid_off_y = 0;
    if (g_vid.stream_index < 0) return;
    if (wwork > (int16_t)g_vid.width)  g_vid_off_x = (wwork - (int16_t)g_vid.width) >> 1;
    if (hwork > (int16_t)g_vid.height) g_vid_off_y = (hwork - (int16_t)g_vid.height) >> 1;
}

static void vid_fill_black_bars_once(void)
{
    if (g_vid_off_x <= 0 && g_vid_off_y <= 0) return;

    short old_rgb0[3], black[3] = {0, 0, 0};
    int16_t bg_pxy[4];

    vq_color(handle, 0, 0, old_rgb0);
    vs_color(handle, 0, black);
    vsf_color(handle, 0);
    vsf_interior(handle, 1);
    vsf_perimeter(handle, 0);
    set_clip(1, xwork, ywork, wwork, hwork);

    if (g_vid_off_y > 0) {
        bg_pxy[0] = xwork;               bg_pxy[1] = ywork;
        bg_pxy[2] = xwork + wwork - 1;   bg_pxy[3] = ywork + g_vid_off_y - 1;
        vr_recfl(handle, bg_pxy);
        int16_t bot = ywork + g_vid_off_y + (int16_t)g_vid.height;
        bg_pxy[0] = xwork;               bg_pxy[1] = bot;
        bg_pxy[2] = xwork + wwork - 1;   bg_pxy[3] = ywork + hwork - 1;
        vr_recfl(handle, bg_pxy);
    }
    if (g_vid_off_x > 0) {
        bg_pxy[0] = xwork;               bg_pxy[1] = ywork + g_vid_off_y;
        bg_pxy[2] = xwork + g_vid_off_x - 1;
        bg_pxy[3] = ywork + g_vid_off_y + (int16_t)g_vid.height - 1;
        vr_recfl(handle, bg_pxy);
        int16_t rx = xwork + g_vid_off_x + (int16_t)g_vid.width;
        bg_pxy[0] = rx;                  bg_pxy[1] = ywork + g_vid_off_y;
        bg_pxy[2] = xwork + wwork - 1;   bg_pxy[3] = bg_pxy[3];
        vr_recfl(handle, bg_pxy);
    }

    vs_color(handle, 0, old_rgb0);
}

static void shadow_push(AVPacket *pkt) {   /* CHANGED: blocking */
    pthread_mutex_lock(&g_shadow_mutex);
    int next = (g_shadow_head + 1) % SHADOW_BUF_SIZE;
    while (next == g_shadow_tail && !app_end) {
        pthread_mutex_unlock(&g_shadow_mutex);
        pthread_yield_np();
        pthread_mutex_lock(&g_shadow_mutex);
        next = (g_shadow_head + 1) % SHADOW_BUF_SIZE;
    }
    if (app_end) {
        pthread_mutex_unlock(&g_shadow_mutex);
        av_packet_free(&pkt);
        return;
    }
    g_audio_shadow[g_shadow_head] = pkt;
    g_shadow_head = next;
    pthread_mutex_unlock(&g_shadow_mutex);
}

static int shadow_pop(AVPacket **out_pkt) {
    pthread_mutex_lock(&g_shadow_mutex);
    if (g_shadow_head != g_shadow_tail) {
        *out_pkt = g_audio_shadow[g_shadow_tail];
        g_shadow_tail = (g_shadow_tail + 1) % SHADOW_BUF_SIZE;
        pthread_mutex_unlock(&g_shadow_mutex);
        return 0;
    }
    pthread_mutex_unlock(&g_shadow_mutex);
    *out_pkt = NULL;
    return 1;
}

static void __attribute__((interrupt)) timerA(void)
{
    loadNewSample = 1;
    *((volatile unsigned char *)0xFFFFFA0FL) &= ~(1 << 5);
}

static void normalize_path(char *path)
{
    if (path[0] != '\0' && path[1] == ':') {
        char drive = path[0];
        if ((drive >= 'a' && drive <= 'z') || (drive >= 'A' && drive <= 'Z')) {
            int len = (int)strlen(path);
            for (int i = len; i >= 0; --i) path[i + 1] = path[i];
            path[0] = '/';
            path[1] = drive;
            path[2] = '/';
            memmove(path + 2, path + 3, len - 1);
        }
    }
    for (char *p = path; *p; ++p) {
        if (*p == '\\') *p = '/';
    }
}

static const char *get_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

static void open_vwork(void)
{
    int16_t i;
    for (i = 0; i < 10; work_in[i++] = 1)
        work_in[10] = 2;
    handle = phys_handle;
    v_opnvwk(work_in, &handle, work_out);
}

static void set_clip(int16_t flag, int16_t x, int16_t y, int16_t w, int16_t h)
{
    int16_t clip[4];
    clip[0] = x;
    clip[1] = y;
    clip[2] = (int16_t)(x + w - 1);
    clip[3] = (int16_t)(y + h - 1);
    vs_clip(handle, flag, clip);
}

static void open_window(int win_width, int win_height, const char *title)
{
    int16_t wi_kind = (MOVER | CLOSER | NAME);
    int16_t pxy[4];
    int16_t outer_x, outer_y, outer_w, outer_h;

    wi_handle = wind_create(wi_kind, xdesk, ydesk, wdesk, hdesk);
    wind_set_str(wi_handle, WF_NAME, title);

    /* Convert work-area size to full outer size (decorations included) */
    wind_calc(WC_BORDER, wi_kind, 0, 0, win_width, win_height,
              &outer_x, &outer_y, &outer_w, &outer_h);
    wind_open(wi_handle, xdesk + 80, ydesk + 20, outer_w, outer_h);

    wind_get(wi_handle, WF_WORKXYWH, &xwork, &ywork, &wwork, &hwork);
    wind_get(wi_handle, WF_CURRXYWH, &xext,  &yext,  &wext,  &hext);

    vsf_interior(handle, 0);
    pxy[0] = xwork; pxy[1] = ywork;
    pxy[2] = xwork + wwork; pxy[3] = ywork + hwork;
    set_clip(1, xwork, ywork, wwork, hwork);
    vr_recfl(handle, pxy);
    set_clip(0, xwork, ywork, wwork, hwork);

    mm_ico_pxy_control_bar[0] = xwork;
    mm_ico_pxy_control_bar[1] = ywork + hwork - mm_ico_win_delta_y;
    mm_ico_pxy_control_bar[2] = xwork + wwork;
    mm_ico_pxy_control_bar[3] = ywork + hwork;
}

static void st_Send_WM_REDRAW(void)
{
    int16_t msg[8];
    msg[0] = WM_REDRAW; msg[1] = gl_apid; msg[2] = 0;
    msg[3] = wi_handle;
    msg[4] = xwork;     msg[5] = ywork;
    msg[6] = wwork;     msg[7] = hwork;
    appl_write(gl_apid, 16, &msg);
}

static void *st_Win_Redraw(void * /*p*/)
{
    GRECT  rect;
    int16_t pxy[8];

    hide_mouse();
    wind_update(BEG_UPDATE);
    wind_get(wi_handle, WF_FIRSTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    while (rect.g_h != 0 && rect.g_w != 0) {
        if (rc_intersect((GRECT *)&msgbuff[4], &rect)) {
            pxy[0] = rect.g_x - xwork;
            pxy[1] = rect.g_y - ywork;
            pxy[2] = pxy[0] + rect.g_w - 1;
            pxy[3] = pxy[1] + rect.g_h - 1;
            grect_to_array(&rect, &pxy[4]);
            set_clip(1, rect.g_x, rect.g_y, rect.g_w, rect.g_h);
            vr_recfl(handle, &pxy[4]);
            vro_cpyfm(handle, S_ONLY, pxy, &mm_wi_mfdb, &screen_mfdb);
        }
        wind_get(wi_handle, WF_NEXTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    }
    wind_update(END_UPDATE);
    graf_mouse(ARROW, 0L);
    show_mouse();
    return NULL;
}

static void ico_decompress(const char *file_name, MFDB *foreground_mfdb)
{
    char full_path[512];

    if (g_bin_dir[0] && file_name[0] != '/' && file_name[1] != ':') {
        /* Chemin relatif : on le préfixe avec le répertoire du binaire */
        snprintf(full_path, sizeof(full_path), "%s%s", g_bin_dir, file_name);
    } else {
        strncpy(full_path, file_name, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }

    FILE *fp = fopen(full_path, "rb");
    if (!fp) { printf("WARN: icon not found: %s\n", full_path); return; }

    uint8_t header[BYTES_TO_CHECK];
    fread(header, 1, BYTES_TO_CHECK, fp);
    if (png_sig_cmp((png_const_bytep)header, 0, BYTES_TO_CHECK)) {
        fclose(fp); return;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) { fclose(fp); return; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) { png_destroy_read_struct(&png_ptr, NULL, NULL); fclose(fp); return; }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, BYTES_TO_CHECK);
    png_read_info(png_ptr, info_ptr);

    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth  = png_get_bit_depth(png_ptr, info_ptr);

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);
    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);

    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++)
        row_pointers[y] = (png_byte *)malloc(png_get_rowbytes(png_ptr, info_ptr));
    png_read_image(png_ptr, row_pointers);

    int16_t nb_px_needed_to_resize = 0;
    while ((width + nb_px_needed_to_resize) % 16 != 0) nb_px_needed_to_resize++;

    uint32_t dest_size = (width + nb_px_needed_to_resize) * height * 4;
    uint8_t *destination_buffer = (uint8_t *)malloc(dest_size);
    memset(destination_buffer, 0, dest_size);

    for (int y = (height - 1); y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            uint32_t i = (x + y * (width + nb_px_needed_to_resize)) * 4;
            uint8_t r = row_pointers[y][x * channels];
            uint8_t g = row_pointers[y][x * channels + 1];
            uint8_t b = row_pointers[y][x * channels + 2];
            uint8_t a = (channels == 4) ? row_pointers[y][x * channels + 3] : 0xFF;
            destination_buffer[i++] = a;
            destination_buffer[i++] = r;
            destination_buffer[i++] = g;
            destination_buffer[i++] = b;
        }
    }

    png_read_end(png_ptr, info_ptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);
    for (int y = 0; y < height; y++) free(row_pointers[y]);
    free(row_pointers);

    foreground_mfdb->fd_w = width + nb_px_needed_to_resize;
    foreground_mfdb->fd_h = height;
    foreground_mfdb->fd_wdwidth = MFDB_STRIDE(foreground_mfdb->fd_w) >> 4;
    foreground_mfdb->fd_nplanes = work_out_extended[4];
    foreground_mfdb->fd_addr = (char *)destination_buffer;
    foreground_mfdb->fd_stand = 0;
}

static void ico_load(MFDB *dest_mfdb, ico_png_t *ico)
{
    uint8_t *src = (uint8_t *)ico->mfdb.fd_addr;
    int16_t w = ico->mfdb.fd_w;
    int16_t h = ico->mfdb.fd_h;
    int16_t dst_w = dest_mfdb->fd_w;
    int16_t bpp = dest_mfdb->fd_nplanes;
    int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
    uint8_t *dst = (uint8_t *)dest_mfdb->fd_addr;

    for (int16_t y = 0; y < h; y++) {
        for (int16_t x = 0; x < w; x++) {
            uint32_t si = ((y * w) + x) << 2;
            uint8_t a = src[si];
            uint8_t r = src[si + 1];
            uint8_t g = src[si + 2];
            uint8_t b = src[si + 3];

            int16_t dst_x = x + ico->x;
            int16_t dst_y = y + ico->y;
            if (dst_x < 0 || dst_x >= dst_w || dst_y < 0 || dst_y >= dest_mfdb->fd_h)
                continue;

            uint32_t di = (dst_y * dst_w + dst_x) * bytes_pp;

            if (bpp == 32) {
                if (a == 255) {
                    dst[di + 1] = r; dst[di + 2] = g; dst[di + 3] = b;
                } else if (a > 0) {
                    uint16_t op = a;
                    uint16_t inv = 255 - op;
                    dst[di + 1] = (uint8_t)((op * r + inv * dst[di + 1] + 127) >> 8);
                    dst[di + 2] = (uint8_t)((op * g + inv * dst[di + 2] + 127) >> 8);
                    dst[di + 3] = (uint8_t)((op * b + inv * dst[di + 3] + 127) >> 8);
                }
                dst[di] = 0xFF;
            } else if (bpp == 24) {
                /* BGR order for Atari VDI 24bpp */
                if (a == 255) {
                    dst[di] = b; dst[di + 1] = g; dst[di + 2] = r;
                } else if (a > 0) {
                    uint16_t op = a;
                    uint16_t inv = 255 - op;
                    dst[di]     = (uint8_t)((op * b + inv * dst[di]     + 127) >> 8);
                    dst[di + 1] = (uint8_t)((op * g + inv * dst[di + 1] + 127) >> 8);
                    dst[di + 2] = (uint8_t)((op * r + inv * dst[di + 2] + 127) >> 8);
                }
            } else if (bpp == 16) {
                /* CHANGED: big-endian RGB565 for m68k */
                if (a == 255) {
                    uint16_t fg = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                    dst[di] = (fg >> 8) & 0xFF;
                    dst[di + 1] = fg & 0xFF;
                } else if (a > 0) {
                    uint16_t bg = (dst[di] << 8) | dst[di + 1];
                    uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                    uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                    uint8_t bg_b = (bg & 0x1F) << 3;
                    uint16_t op = a;
                    uint16_t inv = 255 - op;
                    uint8_t out_r = (uint8_t)((op * r + inv * bg_r + 127) >> 8);
                    uint8_t out_g = (uint8_t)((op * g + inv * bg_g + 127) >> 8);
                    uint8_t out_b = (uint8_t)((op * b + inv * bg_b + 127) >> 8);
                    uint16_t out = ((out_r & 0xF8) << 8) | ((out_g & 0xFC) << 3) | (out_b >> 3);
                    dst[di] = (out >> 8) & 0xFF;
                    dst[di + 1] = out & 0xFF;
                }
            }
        }
    }
}

static void ico_update_x(uint16_t index, int16_t new_pos_x, int16_t new_pos_y)
{
    if (new_pos_x >= 0) {
        control_bar[index].pos_x = new_pos_x;
    }
    if (new_pos_y >= 0) {
        control_bar[index].pos_y = new_pos_y;
    } else if (mm_ico_win_delta_y > 0) {
        int16_t ico_h = control_bar[index].main_ico->mfdb.fd_h;
        control_bar[index].pos_y = (mm_ico_win_delta_y - ico_h) / 2;
        if (control_bar[index].pos_y < 2) control_bar[index].pos_y = 2;
    }

    control_bar[index].main_ico->x = control_bar[index].pos_x;
    control_bar[index].main_ico->y = control_bar[index].pos_y;
    control_bar[index].main_ico->x2 = control_bar[index].main_ico->x + control_bar[index].main_ico->mfdb.fd_w;
    control_bar[index].main_ico->y2 = control_bar[index].main_ico->y + control_bar[index].main_ico->mfdb.fd_h;

    if (control_bar[index].alt_path != NULL && control_bar[index].alt_ico != NULL) {
        /* keep alt_ico in sync with main_ico position */
        control_bar[index].alt_ico->x = control_bar[index].pos_x;
        control_bar[index].alt_ico->y = control_bar[index].pos_y;
        control_bar[index].alt_ico->x2 = control_bar[index].alt_ico->x + control_bar[index].alt_ico->mfdb.fd_w;
        control_bar[index].alt_ico->y2 = control_bar[index].alt_ico->y + control_bar[index].alt_ico->mfdb.fd_h;
    }
}

static void ico_init(void)
{
    uint16_t i = 0;
    while (control_bar[i].id > 0) {
        control_bar[i].main_ico->x = control_bar[i].pos_x;
        control_bar[i].main_ico->y = control_bar[i].pos_y;
        ico_decompress(control_bar[i].main_path, &control_bar[i].main_ico->mfdb);
        control_bar[i].main_ico->x2 = control_bar[i].main_ico->x + control_bar[i].main_ico->mfdb.fd_w;
        control_bar[i].main_ico->y2 = control_bar[i].main_ico->y + control_bar[i].main_ico->mfdb.fd_h;
        if (control_bar[i].alt_path != NULL) {
            control_bar[i].alt_ico->x = control_bar[i].pos_x;
            control_bar[i].alt_ico->y = control_bar[i].pos_y;
            ico_decompress(control_bar[i].alt_path, &control_bar[i].alt_ico->mfdb);
            control_bar[i].alt_ico->x2 = control_bar[i].alt_ico->x + control_bar[i].alt_ico->mfdb.fd_w;
            control_bar[i].alt_ico->y2 = control_bar[i].alt_ico->y + control_bar[i].alt_ico->mfdb.fd_h;
        }
        i++;
    }
    ico_decompress(ICO_PATH_FFMPEG_LOGO, &ico_ffmpeg.mfdb);

    /* Vertically center icons in control bar */
    if (mm_ico_win_delta_y > 0) {
        i = 0;
        while (control_bar[i].id > 0) {
            int16_t ico_h = control_bar[i].main_ico->mfdb.fd_h;
            int16_t new_y = (mm_ico_win_delta_y - ico_h) / 2;
            if (new_y < 2) new_y = 2;
            control_bar[i].pos_y = new_y;
            control_bar[i].main_ico->y = new_y;
            control_bar[i].main_ico->y2 = new_y + ico_h;
            if (control_bar[i].alt_path != NULL && control_bar[i].alt_ico != NULL) {
                control_bar[i].alt_ico->y = new_y;
                control_bar[i].alt_ico->y2 = new_y + control_bar[i].alt_ico->mfdb.fd_h;
            }
            i++;
        }
    }
}

static void ico_free(void)
{
    uint16_t i = 0;
    while (control_bar[i].id > 0) {
        if (control_bar[i].main_ico->mfdb.fd_addr) {
            free(control_bar[i].main_ico->mfdb.fd_addr);
            control_bar[i].main_ico->mfdb.fd_addr = NULL;
        }
        if (control_bar[i].alt_ico && control_bar[i].alt_ico->mfdb.fd_addr) {
            free(control_bar[i].alt_ico->mfdb.fd_addr);
            control_bar[i].alt_ico->mfdb.fd_addr = NULL;
        }
        i++;
    }
    if (ico_ffmpeg.mfdb.fd_addr) {
        free(ico_ffmpeg.mfdb.fd_addr);
        ico_ffmpeg.mfdb.fd_addr = NULL;
    }
}

static void ico_handle(int16_t mouse_x, int16_t mouse_y, int16_t mouse_button)
{
    uint16_t i = 0;
    ico_png_t *ico_ptr = NULL;

    if (mouse_button == 1) {
        while (control_bar[i].id > 0) {
            int16_t abs_x1 = mm_ico_pxy_control_bar[0] + control_bar[i].main_ico->x;
            int16_t abs_x2 = mm_ico_pxy_control_bar[0] + control_bar[i].main_ico->x2;
            int16_t abs_y1 = mm_ico_pxy_control_bar[1] + control_bar[i].main_ico->y;
            int16_t abs_y2 = mm_ico_pxy_control_bar[1] + control_bar[i].main_ico->y2;
            if (mouse_x > abs_x1 && mouse_x < abs_x2 && mouse_y > abs_y1 && mouse_y < abs_y2) {
                if (control_bar[i].handler) control_bar[i].handler();
                /* REMOVED: handler now owns alt_state; no toggle here */
                break;
            }
            i++;
        }
    } else {
        while (control_bar[i].id > 0) {
            if (control_bar[i].alt_state && control_bar[i].alt_ico != NULL)
                ico_ptr = control_bar[i].alt_ico;
            else
                ico_ptr = control_bar[i].main_ico;
            ico_load(&mm_control_bar_mfdb, ico_ptr);
            i++;
        }
    }
}

static void ico_draw_bar(void)
{
    if (mm_ico_win_delta_y <= 0) return;
    GRECT rect;
    GRECT bar_rect;
    int16_t pxy[8];

    bar_rect.g_x = mm_ico_pxy_control_bar[0];
    bar_rect.g_y = mm_ico_pxy_control_bar[1];
    bar_rect.g_w = mm_ico_pxy_control_bar[2] - mm_ico_pxy_control_bar[0];
    bar_rect.g_h = mm_ico_pxy_control_bar[3] - mm_ico_pxy_control_bar[1];

    hide_mouse();
    wind_update(BEG_UPDATE);
    wind_get(wi_handle, WF_FIRSTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    while (rect.g_h != 0 && rect.g_w != 0) {
        if (rc_intersect(&bar_rect, &rect)) {
            int16_t src_x = rect.g_x - bar_rect.g_x;
            int16_t src_y = rect.g_y - bar_rect.g_y;
            int16_t w = rect.g_w;
            int16_t h = rect.g_h;

            pxy[0] = src_x;
            pxy[1] = src_y;
            pxy[2] = src_x + w - 1;
            pxy[3] = src_y + h - 1;
            pxy[4] = rect.g_x;
            pxy[5] = rect.g_y;
            pxy[6] = rect.g_x + w - 1;
            pxy[7] = rect.g_y + h - 1;

            set_clip(1, rect.g_x, rect.g_y, rect.g_w, rect.g_h);
            vro_cpyfm(handle, S_ONLY, pxy, &mm_control_bar_mfdb, &screen_mfdb);
        }
        wind_get(wi_handle, WF_NEXTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    }
    wind_update(END_UPDATE);
    show_mouse();
}

static void splash_init(void)
{
    if (g_audio_only) return;

    int16_t sw = wwork;
    int16_t sh = hwork;
    int16_t nb_px = MFDB_STRIDE(sw) - sw;
    int16_t bpp = work_out_extended[4];
    int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
    uint32_t size = (sw + nb_px) * sh * bytes_pp;

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return;
    memset(buf, 0, size);

    for (int16_t y = 0; y < sh; y++) {
        for (int16_t x = 0; x < sw; x++) {
            uint32_t i = (y * (sw + nb_px) + x) * bytes_pp;
            if (bpp == 32) {
                buf[i] = 0xFF;
                buf[i + 1] = SPLASH_GREY_R;
                buf[i + 2] = SPLASH_GREY_G;
                buf[i + 3] = SPLASH_GREY_B;
            } else if (bpp == 24) {
                buf[i] = SPLASH_GREY_R;
                buf[i + 1] = SPLASH_GREY_G;
                buf[i + 2] = SPLASH_GREY_B;
            } else if (bpp == 16) {
                uint16_t c = ((SPLASH_GREY_R & 0xF8) << 8) | ((SPLASH_GREY_G & 0xFC) << 3) | (SPLASH_GREY_B >> 3);
                buf[i] = (c >> 8) & 0xFF; 
                buf[i + 1] = c & 0xFF;
            }
        }
    }

    if (ico_ffmpeg.mfdb.fd_addr != NULL) {
        int16_t lw = ico_ffmpeg.mfdb.fd_w;
        int16_t lh = ico_ffmpeg.mfdb.fd_h;
        ico_ffmpeg.x = (sw - lw) >> 1;
        ico_ffmpeg.y = (sh - lh) >> 1;
        ico_ffmpeg.x2 = ico_ffmpeg.x + lw;
        ico_ffmpeg.y2 = ico_ffmpeg.y + lh;

        uint8_t *src = (uint8_t *)ico_ffmpeg.mfdb.fd_addr;
        for (int16_t y = 0; y < lh; y++) {
            for (int16_t x = 0; x < lw; x++) {
                uint32_t si = ((y * lw) + x) << 2;
                uint8_t a = src[si];
                uint8_t r = src[si + 1];
                uint8_t g = src[si + 2];
                uint8_t b = src[si + 3];

                int16_t dst_x = ico_ffmpeg.x + x;
                int16_t dst_y = ico_ffmpeg.y + y;
                uint32_t di = (dst_y * (sw + nb_px) + dst_x) * bytes_pp;

                if (bpp == 32) {
                    if (a == 255) {
                        buf[di + 1] = r; buf[di + 2] = g; buf[di + 3] = b;
                    } else if (a > 0) {
                        uint16_t op = a;
                        uint16_t inv = 255 - op;
                        buf[di + 1] = (uint8_t)((op * r + inv * buf[di + 1] + 127) >> 8);
                        buf[di + 2] = (uint8_t)((op * g + inv * buf[di + 2] + 127) >> 8);
                        buf[di + 3] = (uint8_t)((op * b + inv * buf[di + 3] + 127) >> 8);
                    }
                    buf[di] = 0xFF;
                } else if (bpp == 24) {
                    if (a == 255) {
                        buf[di] = r; buf[di + 1] = g; buf[di + 2] = b;
                    } else if (a > 0) {
                        uint16_t op = a;
                        uint16_t inv = 255 - op;
                        buf[di]     = (uint8_t)((op * r + inv * buf[di]     + 127) >> 8);
                        buf[di + 1] = (uint8_t)((op * g + inv * buf[di + 1] + 127) >> 8);
                        buf[di + 2] = (uint8_t)((op * b + inv * buf[di + 2] + 127) >> 8);
                    }
                } else if (bpp == 16) {
                    if (a == 255) {
                        uint16_t fg = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                        buf[di] = (fg >> 8) & 0xFF;      /* CHANGED: big-endian */
                        buf[di + 1] = fg & 0xFF;
                    } else if (a > 0) {
                        uint16_t bg = (buf[di] << 8) | buf[di + 1];   /* big-endian read */
                        uint8_t bg_r = ((bg >> 11) & 0x1F) << 3;
                        uint8_t bg_g = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bg_b = (bg & 0x1F) << 3;
                        uint16_t op = a;
                        uint16_t inv = 255 - op;
                        uint8_t out_r = (uint8_t)((op * r + inv * bg_r + 127) >> 8);
                        uint8_t out_g = (uint8_t)((op * g + inv * bg_g + 127) >> 8);
                        uint8_t out_b = (uint8_t)((op * b + inv * bg_b + 127) >> 8);
                        uint16_t out = ((out_r & 0xF8) << 8) | ((out_g & 0xFC) << 3) | (out_b >> 3);
                        buf[di] = (out >> 8) & 0xFF;     /* big-endian */
                        buf[di + 1] = out & 0xFF;
                    }
                }
            }
        }
    }

    g_splash_mfdb.fd_addr = (char *)buf;
    g_splash_mfdb.fd_w = sw + nb_px;
    g_splash_mfdb.fd_h = sh;
    g_splash_mfdb.fd_wdwidth = MFDB_STRIDE(sw + nb_px) >> 4;
    g_splash_mfdb.fd_stand = 0;
    g_splash_mfdb.fd_nplanes = bpp;
    g_splash_active = true;
}

static void splash_draw(void)
{
    if (!g_splash_active || g_splash_mfdb.fd_addr == NULL) return;
    GRECT rect;
    int16_t pxy[8];
    pxy[0] = 0; pxy[1] = 0;
    pxy[2] = g_splash_mfdb.fd_w - 1;
    pxy[3] = g_splash_mfdb.fd_h - 1;
    pxy[4] = xwork; pxy[5] = ywork;
    pxy[6] = xwork + wwork - 1;
    pxy[7] = ywork + hwork - 1;

    hide_mouse();
    wind_update(BEG_UPDATE);
    wind_get(wi_handle, WF_FIRSTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    while (rect.g_h != 0 && rect.g_w != 0) {
        if (rc_intersect((GRECT *)&pxy[4], &rect)) {
            set_clip(1, rect.g_x, rect.g_y, rect.g_w, rect.g_h);
            vro_cpyfm(handle, S_ONLY, pxy, &g_splash_mfdb, &screen_mfdb);
        }
        wind_get(wi_handle, WF_NEXTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    }
    wind_update(END_UPDATE);
    show_mouse();

    vswr_mode(handle, MD_TRANS);
    vsf_color(handle, 0);

    int16_t tx = xwork + (gl_wbox * 2);
    int16_t ty = ywork + (hwork >> 1) - (gl_hbox * 4);

    v_gtext(handle, tx, ty,                           "SPACE to play");
    v_gtext(handle, tx, ty + gl_hbox,                 "UP/DOWN: volume");
    v_gtext(handle, tx, ty + gl_hbox * 2,            "RIGHT CLICK: show icons");
    v_gtext(handle, tx, ty + gl_hbox * 3,            "ESC: exit");
    v_gtext(handle, tx, ty + gl_hbox * 5,             "ARGS:");
    v_gtext(handle, tx, ty + gl_hbox * 6,             "  --disable-resample");
    v_gtext(handle, tx, ty + gl_hbox * 7,             "    (no audio resampling)");
    v_gtext(handle, tx, ty + gl_hbox * 8,             "  --direct-play");
    v_gtext(handle, tx, ty + gl_hbox * 9,             "    (skip splash, play now)");

    vswr_mode(handle, MD_REPLACE);
}

static void splash_free(void)
{
    if (g_splash_mfdb.fd_addr) {
        free(g_splash_mfdb.fd_addr);
        g_splash_mfdb.fd_addr = NULL;
    }
    g_splash_active = false;
}

static void vol_set_shift(int shift)
{
    if (shift < VOL_SHIFT_MIN) shift = VOL_SHIFT_MIN;
    if (shift > VOL_SHIFT_MAX) shift = VOL_SHIFT_MAX;
    g_vol_shift = shift;
    printf("Volume shift: %d/8 (%d%%)\n", shift, (int)(100 - (shift * 100 / VOL_SHIFT_MAX)));
}

static void vol_up(void)   { vol_set_shift(g_vol_shift - VOL_SHIFT_STEP); }
static void vol_down(void) { vol_set_shift(g_vol_shift + VOL_SHIFT_STEP); }

static uint8_t  g_surplus_buf[65536];
static uint32_t g_surplus_size = 0;

static void audio_only_fill_background(void)
{
    if (mm_wi_mfdb.fd_addr == NULL) return;

    int16_t bpp = mm_wi_mfdb.fd_nplanes;
    int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
    int16_t w = mm_wi_mfdb.fd_w;
    int16_t h = mm_wi_mfdb.fd_h;

    /* Grey background */
    for (int16_t y = 0; y < h; y++) {
        for (int16_t x = 0; x < w; x++) {
            uint32_t i = ((uint32_t)y * w + x) * bytes_pp;
            if (bpp == 32) {
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i]   = 0xFF;
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+1] = SPLASH_GREY_R;
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+2] = SPLASH_GREY_G;
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+3] = SPLASH_GREY_B;
            } else if (bpp == 24) {
                /* BGR order for Atari VDI 24bpp */
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i]   = SPLASH_GREY_B;
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+1] = SPLASH_GREY_G;
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+2] = SPLASH_GREY_R;
            } else if (bpp == 16) {
                uint16_t c = (uint16_t)(((SPLASH_GREY_R & 0xF8) << 8) |
                                        ((SPLASH_GREY_G & 0xFC) << 3) |
                                         (SPLASH_GREY_B >> 3));
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i]   = (uint8_t)((c >> 8) & 0xFF);
                ((uint8_t*)mm_wi_mfdb.fd_addr)[i+1] = (uint8_t)(c & 0xFF);
            }
        }
    }

    /* Re-centre logo in content area (above control bar) */
    if (ico_ffmpeg.mfdb.fd_addr != NULL) {
        int16_t content_area_h = h - mm_ico_win_delta_y;
        ico_ffmpeg.x  = (int16_t)((w - ico_ffmpeg.mfdb.fd_w) >> 1);
        ico_ffmpeg.y  = (int16_t)((content_area_h - ico_ffmpeg.mfdb.fd_h) >> 1);
        if (ico_ffmpeg.y < 4) ico_ffmpeg.y = 4;
        ico_ffmpeg.x2 = ico_ffmpeg.x + ico_ffmpeg.mfdb.fd_w;
        ico_ffmpeg.y2 = ico_ffmpeg.y + ico_ffmpeg.mfdb.fd_h;
        ico_load(&mm_wi_mfdb, &ico_ffmpeg);
    }

    /* Re-bake icons into control bar area */
    ico_handle(0, 0, 0);
}

static void ico_refresh_bar(void)
{
    if (mm_ico_win_delta_y <= 0) return;

    if (g_audio_only) {
        int16_t bpp = work_out_extended[4];
        int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
        uint8_t *bar = (uint8_t *)mm_control_bar_mfdb.fd_addr;
        int16_t w = mm_control_bar_mfdb.fd_w;
        int16_t h = mm_control_bar_mfdb.fd_h;

        /* Clear bar area back to grey so old icons don't bleed through */
        for (int16_t y = 0; y < h; y++) {
            for (int16_t x = 0; x < w; x++) {
                uint32_t i = ((uint32_t)y * w + x) * bytes_pp;
                if (bpp == 32) {
                    bar[i] = 0xFF; bar[i+1] = SPLASH_GREY_R;
                    bar[i+2] = SPLASH_GREY_G; bar[i+3] = SPLASH_GREY_B;
                } else if (bpp == 24) {
                    /* CHANGED: BGR order for Atari VDI 24bpp */
                    bar[i] = SPLASH_GREY_B; bar[i+1] = SPLASH_GREY_G;
                    bar[i+2] = SPLASH_GREY_R;
                } else if (bpp == 16) {
                    uint16_t c = (uint16_t)(((SPLASH_GREY_R & 0xF8) << 8) |
                                            ((SPLASH_GREY_G & 0xFC) << 3) |
                                             (SPLASH_GREY_B >> 3));
                    /* CHANGED: big-endian RGB565 for m68k */
                    bar[i] = (uint8_t)((c >> 8) & 0xFF);
                    bar[i+1] = (uint8_t)(c & 0xFF);
                }
            }
        }
        ico_handle(0, 0, 0);
        ico_draw_bar();
    }
    /* Video mode: thread_vid_display() will pick up the new alt_state
     * on the next frame it blits, so we do nothing here. */
}

static void toggle_mute(void)
{
    g_snd.is_muted = !g_snd.is_muted;
    if (g_snd.is_muted && g_snd.dma_started) {
        memset(g_snd.pPhysical, 0, g_snd.bufferSize);
    }
    /* Sync icon: muted → show mute icon (alt), unmuted → show volume icon (main) */
    if (control_bar[2].alt_path != NULL)
        control_bar[2].alt_state = g_snd.is_muted;

    ico_refresh_bar();

    printf("Audio %s\n", g_snd.is_muted ? "MUTED" : "UNMUTED");
}

static void toggle_pause(void)
{
    if (g_snd.stream_index >= 0) {
        g_snd.is_paused = !g_snd.is_paused;
    }
    g_vid.is_paused = !g_vid.is_paused;
    if (g_snd.is_paused && g_snd.dma_started) {
        memset(g_snd.pPhysical, 0, g_snd.bufferSize);
        memset(g_snd.pLogical, 0, g_snd.bufferSize);
        g_surplus_size = 0;
        memset(g_surplus_buf, 0, sizeof(g_surplus_buf));
    }
    /* Sync icon: paused → show play triangle (alt), playing → show pause bars (main) */
    if (control_bar[0].alt_path != NULL)
        control_bar[0].alt_state = g_snd.is_paused;

    ico_refresh_bar();

    printf("Playback %s\n", g_snd.is_paused ? "PAUSED" : "RESUMED");
}

static void restart_playback(void)
{
    printf("Restart requested\n");
    g_restart_requested = true;
    app_end = true;
}

static void draw_audio_only_time(void)
{
    if (!g_audio_only || !g_snd.dma_started) return;

    uint32_t samples_per_buffer = g_snd.bufferSize >> 2;
    uint32_t total_samples = (uint32_t)(g_snd.duration_s * g_snd.atari_effective_sr);
    uint32_t played_samples = g_snd.feed_count * samples_per_buffer
                            + (uint32_t)snd_get_playback_pos();

    if (played_samples > total_samples) played_samples = total_samples;
    uint32_t remaining_samples = total_samples - played_samples;
    double remaining_sec = (double)remaining_samples / (double)g_snd.atari_effective_sr;

    int mins = (int)(remaining_sec / 60.0);
    int secs = (int)(remaining_sec) % 60;
    char time_str[16];
    sprintf(time_str, "-%02d:%02d", mins, secs);

    int16_t tx   = xwork + wwork - (gl_wbox * 6);
    int16_t ty   = ywork + gl_hbox + 2;
    int16_t tw   = gl_wbox * 6;
    int16_t ttop = ty - gl_hbox;
    int16_t th   = gl_hbox + 4;

    GRECT text_rect;
    text_rect.g_x = tx;
    text_rect.g_y = ttop;
    text_rect.g_w = tw;
    text_rect.g_h = th;

    GRECT rect;
    int16_t pxy[8];

    /* walk visible rectangles so we never draw over foreign windows
     * and we always use a fresh clip after WM_MOVED                  */
    wind_get(wi_handle, WF_FIRSTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    while (rect.g_h != 0 && rect.g_w != 0) {
        if (rc_intersect(&text_rect, &rect)) {
            /* Source offset into the static background buffer */
            pxy[0] = rect.g_x - xwork;
            pxy[1] = rect.g_y - ywork;
            pxy[2] = pxy[0] + rect.g_w - 1;
            pxy[3] = pxy[1] + rect.g_h - 1;
            pxy[4] = rect.g_x;
            pxy[5] = rect.g_y;
            pxy[6] = rect.g_x + rect.g_w - 1;
            pxy[7] = rect.g_y + rect.g_h - 1;

            set_clip(1, rect.g_x, rect.g_y, rect.g_w, rect.g_h);
            vro_cpyfm(handle, S_ONLY, pxy, &mm_wi_mfdb, &screen_mfdb);

            vswr_mode(handle, MD_TRANS);
            vsf_color(handle, 0);
            v_gtext(handle, tx, ty, time_str);
            vswr_mode(handle, MD_REPLACE);
        }
        wind_get(wi_handle, WF_NEXTXYWH, &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
    }
}

static void apply_fullscreen(void)
{
    if (g_fullscreen == g_fs_requested) return;

    /* Fermer et détruire l'ancienne fenêtre */
    wind_close(wi_handle);
    wind_delete(wi_handle);

    if (g_fs_requested) {
        /* Sauvegarde de la géométrie fenêtrée SEULEMENT si on vient du mode fenêtré */
        if (!g_fullscreen && g_saved_w > 0 && g_saved_h > 0) {
            /* déjà sauvé par WM_MOVED ou par l'init, on garde */
        } else if (!g_fullscreen) {
            /* fallback si jamais sauvé */
            g_saved_x = xdesk + 80;
            g_saved_y = ydesk + 20;
            int16_t tmp_ox, tmp_oy;
            int16_t def_w = (int16_t)MIN(g_vid.width, wdesk - 20);
            int16_t def_h = (int16_t)MIN(g_vid.height, hdesk - 80);
            wind_calc(WC_BORDER, MOVER | CLOSER | NAME,
                      0, 0, def_w, def_h,
                      &tmp_ox, &tmp_oy, &g_saved_w, &g_saved_h);
        }

        fullscreen_lock();

        /* Fenêtre sans bordures, taille du bureau */
        wi_handle = wind_create(0, xdesk, ydesk, wdesk, hdesk);
        wind_open(wi_handle, xdesk, ydesk, wdesk, hdesk);

        wind_get(wi_handle, WF_WORKXYWH, &xwork, &ywork, &wwork, &hwork);
        wind_get(wi_handle, WF_CURRXYWH, &xext, &yext, &wext, &hext);

        vid_compute_fullscreen_offset();
        vid_fill_black_bars_once();   /* ← AJOUT : une seule fois */
        mm_ico_win_delta_y = 0;   /* pas de barre d'icônes en plein écran */
        g_fullscreen = true;          /* ← assure que g_fullscreen est mis */        
    } else {
        /* Retour fenêtré : d'abord rendre l'écran, PUIS créer la fenêtre */
        fullscreen_unlock();

        int16_t open_x, open_y, open_w, open_h;

        if (g_saved_w > 0 && g_saved_h > 0) {
            open_x = g_saved_x;
            open_y = g_saved_y;
            open_w = g_saved_w;
            open_h = g_saved_h;
        } else {
            /* Jamais été fenêtré dans cette session → calcul par défaut */
            int16_t win_w = (int16_t)g_vid.width;
            int16_t win_h = (int16_t)g_vid.height;
            if (win_w > wdesk - 20) win_w = wdesk - 20;
            if (win_h > hdesk - 80) win_h = hdesk - 80;

            int16_t outer_x, outer_y;
            wind_calc(WC_BORDER, MOVER | CLOSER | NAME,
                      0, 0, win_w, win_h,
                      &outer_x, &outer_y, &open_w, &open_h);

            open_x = xdesk + 80;
            open_y = ydesk + 20;
        }

        wi_handle = wind_create(MOVER | CLOSER | NAME, xdesk, ydesk, wdesk, hdesk);
        wind_set_str(wi_handle, WF_NAME, win_title);
        wind_open(wi_handle, open_x, open_y, open_w, open_h);

        wind_get(wi_handle, WF_WORKXYWH, &xwork, &ywork, &wwork, &hwork);
        wind_get(wi_handle, WF_CURRXYWH, &xext, &yext, &wext, &hext);

        mm_ico_win_delta_y = 40;
        ico_update_x(2, wwork - 72, -1);
        g_vid_off_x = 0;
        g_vid_off_y = 0;
        g_fullscreen = false;        
    }

    mm_ico_pxy_control_bar[0] = xwork;
    mm_ico_pxy_control_bar[1] = ywork + hwork - mm_ico_win_delta_y;
    mm_ico_pxy_control_bar[2] = xwork + wwork;
    mm_ico_pxy_control_bar[3] = ywork + hwork;

    /* Recrée le splash à la nouvelle taille s'il est actif */
    if (g_splash_active) {
        splash_free();
        splash_init();
    }

    g_fullscreen = g_fs_requested;
    st_Send_WM_REDRAW();
}

static void *exec_eventloop(void * /*p*/)
{
    control_bar[0].handler = toggle_pause;
    control_bar[2].handler = toggle_mute;
    control_bar[1].handler = restart_playback;

    while (!app_end) {
        int16_t event = evnt_multi(
            MU_MESAG | MU_BUTTON | MU_KEYBD | MU_TIMER,
            256 | 2, 3, button_is_down,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            msgbuff, 0,
            &mx, &my, &mb, &mc, &keycode, &dummy_rv);

        wind_update(TRUE);

        if (event & MU_MESAG) {
            switch (msgbuff[0]) {
            case WM_CLOSED:
                app_end = true;
                break;
            case WM_REDRAW:
                if (g_splash_active) {
                    st_Win_Redraw(NULL);
                    splash_draw();
                } else if (!g_vid.is_playing) {
                    st_Win_Redraw(NULL);
                }
                break;
            case AP_MSG_TIME_UPDATE:
                if (g_audio_only) draw_audio_only_time();
                break;
            case WM_TOPPED:
                wind_set(wi_handle, WF_TOP, 0, 0, 0, 0);
                break;
            case WM_MOVED:
                if (msgbuff[6] < (2 * gl_wbox)) msgbuff[6] = 2 * gl_wbox;
                if (msgbuff[7] < (3 * gl_hbox)) msgbuff[7] = 3 * gl_hbox;
                wind_set(wi_handle, WF_CURRXYWH, msgbuff[4], msgbuff[5], msgbuff[6], msgbuff[7]);
                wind_get(wi_handle, WF_WORKXYWH, &xwork, &ywork, &wwork, &hwork);
                wind_get(wi_handle, WF_CURRXYWH, &xext,  &yext,  &wext,  &hext);
                mm_ico_pxy_control_bar[0] = xwork;
                mm_ico_pxy_control_bar[1] = ywork + hwork - mm_ico_win_delta_y;
                mm_ico_pxy_control_bar[2] = xwork + wwork;
                mm_ico_pxy_control_bar[3] = ywork + hwork;
                ico_update_x(2, wwork - 72, -1);
                break;
            default:
                break;
            }
        }

        if (event & MU_BUTTON) {
            /* Clic droit : toggle barre d'icônes (UNIQUEMENT en mode fenêtré) */
            if (mb == 2 && !g_fullscreen &&
                xwork < mx && mx < (xwork + wwork) &&
                ywork < my && my < (ywork + hwork)) {
                mm_ico_win_delta_y = (mm_ico_win_delta_y > 0) ? 0 : 40;
                mm_ico_pxy_control_bar[0] = xwork;
                mm_ico_pxy_control_bar[2] = xwork + wwork;
                mm_ico_pxy_control_bar[3] = ywork + hwork;
                mm_ico_pxy_control_bar[1] = mm_ico_pxy_control_bar[3] - mm_ico_win_delta_y;
                if (mm_ico_win_delta_y > 0) {
                    for (uint16_t idx = 0; control_bar[idx].id > 0; idx++) {
                        ico_update_x(idx, -1, -1);
                    }
                    if (g_audio_only) {
                        int16_t bpp = work_out_extended[4];
                        int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
                        memset(mm_control_bar_mfdb.fd_addr, 0,
                               mm_control_bar_mfdb.fd_w * mm_control_bar_mfdb.fd_h * bytes_pp);
                        ico_handle(0, 0, 0);
                        ico_draw_bar();
                    } else {
                        st_Send_WM_REDRAW();
                    }
                } else {
                    st_Send_WM_REDRAW();
                }
            }
            /* Clic gauche sur la barre d'icônes (UNIQUEMENT si elle est visible) */
            if (mb == 1 && mm_ico_win_delta_y > 0 && !g_fullscreen &&
                xwork < mx && mx < (xwork + wwork) &&
                ywork < my && my < (ywork + hwork)) {
                ico_handle(mx, my, mb);
                if (!g_playback_started) {
                    st_Send_WM_REDRAW();
                }
            }
        }

        if (event & MU_KEYBD) {
            int16_t ascii    = (int16_t)( keycode        & 0xFF);
            int16_t scancode = (int16_t)((keycode >> 8)  & 0xFF);

            if (ascii == 27 || ascii == 'q' || ascii == 'Q') {
                app_end = true;
            } else if (ascii == ' ') {
                if (!g_playback_started) {
                    g_playback_started = true;
                    g_splash_active = false;
                    g_vid.is_paused = false;
                    g_snd.is_paused = false;
                    st_Send_WM_REDRAW();
                } else {
                    toggle_pause();
                }
            } else if (ascii == 'f' || ascii == 'F') {
                if (g_vid.stream_index >= 0) {
                    g_fs_requested = !g_fs_requested;
                }                
            } else if (scancode == 0x48) {
                if (g_snd.stream_index >= 0) vol_up();
            } else if (scancode == 0x50) {
                if (g_snd.stream_index >= 0) vol_down();
            }
        }

        if (g_fs_requested != g_fullscreen) {
            apply_fullscreen();
        }

        wind_update(FALSE);
        pthread_yield_np();
    }
    return NULL;
}

static void snd_compute_prescale(void)
{
    int32_t sr = g_snd.src_samplerate;
    const freq_entry_t *base = NULL;
    size_t base_count = 0;
    int ext = 0;

    switch (g_machine_type) {
        case 0:                     /* ST/E */
            base = freq_ste;
            base_count = sizeof(freq_ste) / sizeof(freq_ste[0]);
            break;

        case 2:                     /* TT */
            base = freq_tt;
            base_count = sizeof(freq_tt) / sizeof(freq_tt[0]);
            break;

        case 5:                     /* ARAnyM */
            base = freq_aranym;
            base_count = sizeof(freq_aranym) / sizeof(freq_aranym[0]);
            break;

        case 6:                     /* Vampire V4SA */
            ext = milan_vampire_gpio_clock();
            if (ext == 1) {
                base = freq_vampire_cd;
                base_count = sizeof(freq_vampire_cd) / sizeof(freq_vampire_cd[0]);
            } else {
                base = freq_vampire;
                base_count = sizeof(freq_vampire) / sizeof(freq_vampire[0]);
            }
            break;

        case 4:                     /* Milan */
            ext = milan_vampire_gpio_clock();
            if (ext == 1) {
                base = freq_ext44;
                base_count = sizeof(freq_ext44) / sizeof(freq_ext44[0]);
            } else if (ext == 2) {
                base = freq_ext48;
                base_count = sizeof(freq_ext48) / sizeof(freq_ext48[0]);
            } else {
                base = freq_falcon;
                base_count = sizeof(freq_falcon) / sizeof(freq_falcon[0]);
            }
            break;

        case 3:                     /* Falcon */
        default:
            ext = falcon_detect_ext_clock();
            if (ext == 1) {
                base = freq_ext44;
                base_count = sizeof(freq_ext44) / sizeof(freq_ext44[0]);
            } else if (ext == 2) {
                base = freq_ext48;
                base_count = sizeof(freq_ext48) / sizeof(freq_ext48[0]);
            } else {
                base = freq_falcon;
                base_count = sizeof(freq_falcon) / sizeof(freq_falcon[0]);
            }
            break;
    }

    /* Nearest match */
    const freq_entry_t *best = &base[0];
    int32_t best_delta = labs(base[0].freq - sr);

    for (size_t i = 1; i < base_count; i++) {
        int32_t d = labs(base[i].freq - sr);
        if (d < best_delta) {
            best_delta = d;
            best = &base[i];
        }
    }

    g_snd.clk_prescale       = best->prescale;
    g_snd.atari_effective_sr = best->freq;
    g_snd.clk_source         = best->clk;
    g_snd.setpre_val         = best->setpre;

    if (g_disable_resample)
        g_snd.wanted_sr = g_snd.src_samplerate;
    else
        g_snd.wanted_sr = g_snd.atari_effective_sr;

    printf("Audio: wanted %ldHz -> DMA %ldHz (machine=%d prescale=%d clk=%s)\n",
           sr, g_snd.atari_effective_sr, g_machine_type, best->prescale,
           best->clk == CLKEXT ? "EXT" : "25M");
}

static void apply_volume_shift(int16_t *buf, uint32_t samples)
{
    int shift = g_vol_shift;
    if (shift == 0) return;
    if (shift >= 8) { memset(buf, 0, samples * sizeof(int16_t)); return; }

    for (uint32_t i = 0; i < samples; i++) {
        buf[i] >>= shift;
    }
}

static void snd_fill_logical(void)
{
    if (g_snd.is_paused) {
        memset(g_snd.pLogical, 0, g_snd.bufferSize);
        return;
    }

    AVCodecContext *dec = g_snd.codec_ctx;
    AVFrame        *frm = g_snd.frame;
    SwrContext     *swr = g_snd.swr_ctx;

    uint8_t *pDst      = (uint8_t *)g_snd.pLogical;
    uint32_t data_size = 0;

    const int out_ch  = 2;
    const int out_smp = 2048;

    if (g_surplus_size > 0) {
        uint32_t take = MIN(g_surplus_size, (uint32_t)g_snd.bufferSize);
        if (!g_snd.is_muted) memcpy(pDst, g_surplus_buf, take);
        else                 memset(pDst, 0, take);
        data_size       = take;
        g_surplus_size -= take;
        if (g_surplus_size > 0)
            memmove(g_surplus_buf, g_surplus_buf + take, g_surplus_size);
    }

    while (data_size < (uint32_t)g_snd.bufferSize && !app_end) {
        int ret;

        ret = avcodec_receive_frame(dec, frm);
        if (ret == 0) {
            int got = swr_convert(swr, &g_snd.swr_tmp_buf, out_smp,
                                  (const uint8_t **)frm->data, frm->nb_samples);
            if (got < 0) continue;

            while (got > 0) {
                int len = av_samples_get_buffer_size(NULL, out_ch, got, AV_SAMPLE_FMT_S16, 1);
                int32_t available = (int32_t)g_snd.bufferSize - (int32_t)data_size;

                if (!g_snd.is_muted && g_vol_shift > 0) {
                    apply_volume_shift((int16_t *)g_snd.swr_tmp_buf, (uint32_t)(len >> 1));
                }

                if (len <= available) {
                    if (!g_snd.is_muted) memcpy(pDst + data_size, g_snd.swr_tmp_buf, len);
                    else                 memset(pDst + data_size, 0, len);
                    data_size += len;
                } else {
                    if (!g_snd.is_muted) memcpy(pDst + data_size, g_snd.swr_tmp_buf, available);
                    else                 memset(pDst + data_size, 0, available);
                    data_size = g_snd.bufferSize;

                    uint32_t leftover = len - available;
                    uint32_t surplus_space = sizeof(g_surplus_buf) - g_surplus_size;
                    if (leftover > surplus_space) leftover = surplus_space;
                    if (leftover > 0) {
                        memcpy(g_surplus_buf + g_surplus_size, g_snd.swr_tmp_buf + available, leftover);
                        g_surplus_size += leftover;
                    }
                }
                got = swr_convert(swr, &g_snd.swr_tmp_buf, out_smp, NULL, 0);
                if (got < 0) break;
            }
            continue;
        } else if (ret != AVERROR(EAGAIN)) {
            break;
        }

        AVPacket *pkt = NULL;
        if (shadow_pop(&pkt) != 0) {
            if (g_snd.dma_started && !g_demux_finished) {
                pthread_yield_np();
                continue;
            } else {
                break;
            }
        }

        if (pkt->data == NULL) {
            avcodec_flush_buffers(dec);
            g_surplus_size = 0;
            memset(g_surplus_buf, 0, sizeof(g_surplus_buf));
            av_packet_free(&pkt);
            break;          /* CHANGED: was continue; EOF sentinel */
        }

        ret = avcodec_send_packet(dec, pkt);
        av_packet_free(&pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) continue;
    }

    if (data_size < (uint32_t)g_snd.bufferSize)
        memset(pDst + data_size, 0, g_snd.bufferSize - data_size);
}

static void snd_init_dma(void)
{
    g_snd.bufferSize = g_snd.atari_effective_sr << 2;

    g_snd.pBuffer = (int8_t *)Mxalloc(g_snd.bufferSize * 2, MX_STRAM);
    if (!g_snd.pBuffer) { printf("ERROR: Mxalloc DMA audio buffer\n"); app_end = true; return; }
    memset(g_snd.pBuffer, 0, g_snd.bufferSize * 2);

    g_snd.pPhysical = g_snd.pBuffer;
    g_snd.pLogical  = g_snd.pBuffer + g_snd.bufferSize;
    g_snd.feed_count = 0;
    g_surplus_size   = 0;
    g_snd.dma_started = false;

    int tmp_size = av_samples_get_buffer_size(NULL, 2, 2048, AV_SAMPLE_FMT_S16, 1);
    g_snd.swr_tmp_buf = (uint8_t *)av_malloc(tmp_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!g_snd.swr_tmp_buf) { printf("ERROR: av_malloc swr_tmp_buf\n"); app_end = true; return; }

    Locksnd();
    attenuation_left  = (int16_t)Soundcmd(LTATTEN, SND_INQUIRE);
    attenuation_right = (int16_t)Soundcmd(RTATTEN, SND_INQUIRE);
    g_vol_shift = 0;
    Sndstatus(SND_RESET);
    Soundcmd(ADCINPUT, 0);

    if (Setmode(MODE_STEREO16) != 0) printf("ERROR: Can not set MODE_STEREO16\n");
    Devconnect(DMAPLAY, DAC, g_snd.clk_source, g_snd.clk_prescale, NO_SHAKE);
    if (g_snd.setpre_val >= 0) {
        Soundcmd(SETPRESCALE, g_snd.setpre_val);
    }

    if (Setbuffer(SR_PLAY, g_snd.pPhysical, g_snd.pPhysical + g_snd.bufferSize) != 0) 
        printf("ERROR: st_Setbuffer\n");
    if (Setinterrupt(SI_TIMERA, SI_PLAY) != 0) 
        printf("ERROR: st_Setinterrupt\n");

    Xbtimer(XB_TIMERA, 1 << 3, 1, timerA);
    st_enableTimerASei();
    Jenabint(MFP_TIMERA);

    g_snd.is_playing = true;
}

static void snd_unset_dma(void)
{
    if (g_snd.dma_started) {
        Buffoper(0x00);
    }
    Jdisint(MFP_TIMERA);
    g_snd.is_playing = false;
    g_snd.is_paused  = false;
    Soundcmd(LTATTEN, attenuation_left);
    Soundcmd(RTATTEN, attenuation_right);
    Unlocksnd();
}

static int32_t snd_get_playback_pos(void)
{
    if (!g_snd.dma_started) return 0;

    SndBufPtr local_ptr;
    Buffptr((int32_t *)&local_ptr);
    int32_t pos = local_ptr.play - (char *)g_snd.pBuffer;
    if (pos >= g_snd.bufferSize) pos -= g_snd.bufferSize;
    return pos >> 2;
}

static void snd_feed(void)
{
    if (loadNewSample) {
        if (!g_snd.is_paused)
            g_snd.feed_count++;
        int8_t *tmp      = g_snd.pPhysical;
        g_snd.pPhysical  = g_snd.pLogical;
        g_snd.pLogical   = tmp;
        Setbuffer(SR_PLAY, g_snd.pPhysical, g_snd.pPhysical + g_snd.bufferSize);
        loadNewSample = 0;
        snd_fill_logical();
    }
}

static void *thread_snd_play(void * /*p*/)
{
    while (!app_end) {
        if (!g_snd.dma_started) {
            int count = (g_shadow_head - g_shadow_tail + SHADOW_BUF_SIZE) % SHADOW_BUF_SIZE;
            if (count >= 24) {
                snd_fill_logical();
                memcpy(g_snd.pPhysical, g_snd.pLogical, g_snd.bufferSize);
                snd_fill_logical();

                if (Buffoper(SB_PLA_ENA | SB_PLA_RPT) != 0)
                    printf("ERROR: st_Buffoper\n");

                g_snd.dma_started = true;
            } else {
                pthread_yield_np();
            }
        } else {
            if (loadNewSample) snd_feed();
            else pthread_yield_np();
        }
    }
    return NULL;
}

static void *thread_audio_ui(void * /*p*/)
{
    int16_t msg[8] = { AP_MSG_TIME_UPDATE, 0, 0, 0, 0, 0, 0, 0 };
    while (!app_end) {
        uint32_t start = (uint32_t)st_Supexec(get200hz);
        while (!app_end) {
            uint32_t now = (uint32_t)st_Supexec(get200hz);
            if ((now - start) >= 100) break;
            pthread_yield_np();
        }
        if (!app_end) appl_write(gl_apid, 16, msg);
    }
    return NULL;
}

static void vid_init_pixel_format(void)
{
    g_vid.screen_bpp = work_out_extended[4];
    switch (g_vid.screen_bpp) {
    case 16: g_vid.screen_pix_fmt = AV_PIX_FMT_RGB565; break;
    case 24: g_vid.screen_pix_fmt = AV_PIX_FMT_BGR24;  break;
    case 32: g_vid.screen_pix_fmt = AV_PIX_FMT_ARGB;   break;
    default:
        printf("WARNING: unsupported BPP %d, defaulting to RGB565\n", g_vid.screen_bpp);
        g_vid.screen_pix_fmt = AV_PIX_FMT_RGB565;
        g_vid.screen_bpp = 16;
        break;
    }
}

static void vid_init(const char *filename)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;
    if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) != 0) {
        printf("ERROR: Cannot open '%s'\n", filename); app_end = true; return;
    }
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        printf("ERROR: Cannot find stream info\n"); app_end = true; return;
    }

    g_vid.fmt_ctx = fmt_ctx;
    g_snd.fmt_ctx = fmt_ctx;
    g_vid.stream_index = -1;
    g_snd.stream_index = -1;

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVMediaType t = fmt_ctx->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && g_vid.stream_index < 0) g_vid.stream_index = (int)i;
        if (t == AVMEDIA_TYPE_AUDIO && g_snd.stream_index < 0) g_snd.stream_index = (int)i;
    }

    if (g_vid.stream_index >= 0) {
        AVStream *vs = fmt_ctx->streams[g_vid.stream_index];
        AVCodecParameters *vpar = vs->codecpar;
        const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);

        if (!vcodec) { printf("ERROR: No video decoder\n"); g_vid.stream_index = -1; goto audio_init; }

        g_vid.codec_ctx = avcodec_alloc_context3(vcodec);
        avcodec_parameters_to_context(g_vid.codec_ctx, vpar);
        if (avcodec_open2(g_vid.codec_ctx, vcodec, NULL) < 0) {
            printf("ERROR: Cannot open video codec\n"); g_vid.stream_index = -1; goto audio_init;
        }

        g_vid.frame_yuv = av_frame_alloc();
        g_vid.frame_rgb = av_frame_alloc();
        g_vid.packet    = av_packet_alloc();
        g_vid.width     = (uint16_t)g_vid.codec_ctx->width;
        g_vid.height    = (uint16_t)g_vid.codec_ctx->height;

        g_vid.fps = (vs->r_frame_rate.den > 0) ? (double)vs->r_frame_rate.num / vs->r_frame_rate.den : 25.0;
        g_vid.total_frames = (vs->nb_frames > 0) ? (uint32_t)vs->nb_frames : 0xFFFFFFFF;
        g_vid.frame_counter = 0;
        g_vid.frames_dropped = 0;

        vid_init_pixel_format();

        int dst_buf_size = av_image_get_buffer_size(g_vid.screen_pix_fmt, g_vid.width, g_vid.height, g_vid.screen_bpp >> 3);
        g_vid_buf_size = dst_buf_size;

        g_vid_ring_buf = (uint8_t *)malloc(dst_buf_size * VID_BUF_SIZE);
        if (!g_vid_ring_buf) { printf("ERROR: malloc video ring buffer\n"); app_end = true; return; }
        memset(g_vid_ring_buf, 0, dst_buf_size * VID_BUF_SIZE);

        av_image_fill_arrays(g_vid.frame_rgb->data, g_vid.frame_rgb->linesize, 
                             g_vid_ring_buf, 
                             g_vid.screen_pix_fmt, g_vid.width, g_vid.height, g_vid.screen_bpp >> 3);

        g_vid.sws_ctx = sws_getContext(g_vid.width, g_vid.height, g_vid.codec_ctx->pix_fmt,
                                       g_vid.width, g_vid.height, g_vid.screen_pix_fmt,
                                       SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!g_vid.sws_ctx) { printf("ERROR: sws_getContext failed\n"); app_end = true; return; }

        int stride = MFDB_STRIDE(g_vid.width);
        mm_wi_mfdb.fd_addr = g_vid_ring_buf; 
        mm_wi_mfdb.fd_w = g_vid.width; 
        mm_wi_mfdb.fd_h = g_vid.height;
        mm_wi_mfdb.fd_wdwidth = stride >> 4; 
        mm_wi_mfdb.fd_stand = 0; 
        mm_wi_mfdb.fd_nplanes = g_vid.screen_bpp;

        printf("Video: %dx%d  %.2f fps  stream=%d  codec=%s\n",
               g_vid.width, g_vid.height, g_vid.fps, g_vid.stream_index,
               g_vid.codec_ctx->codec->long_name);
               
        /* Pré-calcul des champs statiques de la barre de contrôle */
        mm_control_bar_mfdb.fd_w       = MFDB_STRIDE(g_vid.width);
        mm_control_bar_mfdb.fd_wdwidth = MFDB_STRIDE(g_vid.width) >> 4;
        mm_control_bar_mfdb.fd_stand   = 0;
        mm_control_bar_mfdb.fd_nplanes = g_vid.screen_bpp;               
    }

audio_init:
    if (g_snd.stream_index >= 0) {
        AVStream *as = fmt_ctx->streams[g_snd.stream_index];
        AVCodecParameters *apar = as->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);

        if (!acodec) { printf("WARNING: No audio decoder\n"); g_snd.stream_index = -1; goto init_done; }

        g_snd.codec_ctx = avcodec_alloc_context3(acodec);
        avcodec_parameters_to_context(g_snd.codec_ctx, apar);
        if (avcodec_open2(g_snd.codec_ctx, acodec, NULL) < 0) {
            printf("WARNING: Cannot open audio codec\n"); g_snd.stream_index = -1; goto init_done;
        }

        g_snd.src_samplerate = g_snd.codec_ctx->sample_rate;
        g_snd.src_channels   = g_snd.codec_ctx->ch_layout.nb_channels;
        g_snd.src_fmt        = g_snd.codec_ctx->sample_fmt;
        g_snd.duration_s     = (double)fmt_ctx->duration / AV_TIME_BASE;
        g_snd.frame          = av_frame_alloc();

        snd_compute_prescale();

        if (g_vid.stream_index >= 0 && g_vid.fps > 0.0) {
            if (g_disable_resample) {
                double speed_factor = (double)g_snd.atari_effective_sr / g_snd.src_samplerate;
                g_vid.time_per_frame = (double)g_snd.src_samplerate / g_vid.fps;
                g_vid.time_per_frame_wall = 1000.0 / (g_vid.fps * speed_factor);
                printf("Video FPS adjusted: %.2f -> %.2f (speed factor %.4f)\n", 
                       g_vid.fps, g_vid.fps * speed_factor, speed_factor);
            } else {
                g_vid.time_per_frame = (double)g_snd.atari_effective_sr / g_vid.fps;
                g_vid.time_per_frame_wall = 1000.0 / g_vid.fps;
            }
        } else {
            g_vid.time_per_frame = 0.0;
            g_vid.time_per_frame_wall = 1000.0 / 25.0;
        }

        /* pre-calculate drop threshold in samples (avoids division in hot loop) */
        if (g_vid.stream_index >= 0 && g_vid.fps > 0.0) {
            uint32_t fps_delta = (uint32_t)MAX(1.0, g_vid.fps / (double)FPS_MAX_DELTA_DIV);
            g_vid.drop_thresh_samples = (uint32_t)(fps_delta * g_vid.time_per_frame);
        } else {
            g_vid.drop_thresh_samples = 0;
        }

        AVChannelLayout in_ch_layout = g_snd.codec_ctx->ch_layout;
        AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        ret = swr_alloc_set_opts2(&g_snd.swr_ctx, &out_ch_layout, AV_SAMPLE_FMT_S16, g_snd.wanted_sr,
                                  &in_ch_layout, g_snd.src_fmt, g_snd.src_samplerate, 0, NULL);
        if (ret < 0 || !g_snd.swr_ctx) { printf("ERROR: swr_alloc_set_opts2\n"); app_end = true; return; }
        if (swr_init(g_snd.swr_ctx) < 0) { printf("ERROR: swr_init\n"); app_end = true; return; }

        printf("Audio: %dHz  %dch  stream=%d  codec=%s\n",
               g_snd.src_samplerate, g_snd.src_channels, g_snd.stream_index,
               g_snd.codec_ctx->codec->long_name);
        printf("Falcon DMA: %dHz (prescale %d)  Swr target: %dHz  %s\n", 
               g_snd.atari_effective_sr, g_snd.clk_prescale, g_snd.wanted_sr,
               g_disable_resample ? "[NO RESAMPLE]" : "[RESAMPLING]");
    }

    /* Si pas de son : initialise le framerate wall-clock pour la vidéo seule */
    if (g_snd.stream_index < 0 && g_vid.stream_index >= 0) {
        if (g_vid.fps > 0.0)
            g_vid.time_per_frame_wall = 1000.0 / g_vid.fps;
        else
            g_vid.time_per_frame_wall = 1000.0 / 25.0;
    }

init_done:;
}

static void *thread_demux_decode(void * /*p*/)
{
    if (g_vid.stream_index < 0 && g_snd.stream_index < 0) return NULL;

    AVFormatContext *fmt    = g_snd.fmt_ctx;
    AVCodecContext  *vid_dec= g_vid.codec_ctx;
    AVFrame         *frm_yuv= g_vid.frame_yuv;
    SwsContext      *sws    = g_vid.sws_ctx;
    AVPacket        *pkt    = av_packet_alloc();

    if (!pkt) { app_end = true; return NULL; }

    g_vid.is_playing = (g_vid.stream_index >= 0);

    while (!app_end) {
        int ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            if (g_vid.stream_index >= 0) app_end = true;

            /* EOF sentinel for the audio thread */
            AVPacket *sentinel = av_packet_alloc();
            if (sentinel) {
                sentinel->data = NULL;
                sentinel->size = 0;
                shadow_push(sentinel);
            }
            break;
        }

        if (pkt->stream_index == g_snd.stream_index) {
            AVPacket *clone = av_packet_alloc();
            if (clone) { av_packet_move_ref(clone, pkt); shadow_push(clone); }
            else        av_packet_unref(pkt);
            pthread_yield_np();
            continue;
        }

        if (g_vid.stream_index < 0 || pkt->stream_index != g_vid.stream_index) {
            av_packet_unref(pkt);
            pthread_yield_np();
            continue;
        }

        int64_t pkt_pts = pkt->pts;
        ret = avcodec_send_packet(vid_dec, pkt);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(vid_dec, frm_yuv);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int next_h = (g_vid_head + 1) % VID_BUF_SIZE;
            while (next_h == g_vid_tail && !app_end) pthread_yield_np();
            if (app_end) break;

            int64_t raw_pts = (frm_yuv->pts != AV_NOPTS_VALUE) ? frm_yuv->pts : pkt_pts;

            g_vid.frame_rgb->data[0] = g_vid_ring_buf + (g_vid_head * g_vid_buf_size);
            sws_scale(sws, (const uint8_t * const *)frm_yuv->data, frm_yuv->linesize,
                      0, vid_dec->height, g_vid.frame_rgb->data, g_vid.frame_rgb->linesize);

            g_vid_pts_buf[g_vid_head] = raw_pts;
            g_vid_head = (g_vid_head + 1) % VID_BUF_SIZE;
        }
    }

    g_demux_finished = true;
    av_packet_free(&pkt);
    g_vid.is_playing = false;
    return NULL;
}

static void *thread_vid_display(void * /*p*/)
{
    if (g_vid.stream_index < 0) return NULL;

    AVStream *vstream = g_vid.fmt_ctx->streams[g_vid.stream_index];
    const double tb = av_q2d(vstream->time_base);
    const uint32_t samples_per_buffer = (g_snd.stream_index >= 0) ? (uint32_t)(g_snd.bufferSize >> 2) : 0;
    const int32_t sync_rate = g_disable_resample ? g_snd.src_samplerate : g_snd.atari_effective_sr;

    uint32_t time_200hz_start = 0;
    if (g_snd.stream_index < 0) time_200hz_start = (uint32_t)st_Supexec(get200hz);

    while (!app_end) {
        while (g_vid.is_paused && !app_end) {
            pthread_yield_np();
        }
        if (g_snd.stream_index < 0)
            time_200hz_start = (uint32_t)st_Supexec(get200hz);

        while (g_vid_head == g_vid_tail && !app_end) {
            pthread_yield_np();
        }
        if (app_end) break;

        int64_t raw_pts = g_vid_pts_buf[g_vid_tail];

        /* CHANGED: compare directly in sample space — zero divisions in hot loop */
        if (g_snd.dma_started && g_vid.time_per_frame > 0.0) {
            uint32_t pts_buf_pos = (raw_pts != AV_NOPTS_VALUE)
                ? (uint32_t)(tb * (double)raw_pts * (double)sync_rate)
                : (uint32_t)(g_vid.frame_counter * (uint32_t)g_vid.time_per_frame);

            uint32_t audio_pos = g_snd.feed_count * samples_per_buffer
                               + (uint32_t)snd_get_playback_pos();
            if (audio_pos > AUDIO_LATENCY_COMPENSATION)
                audio_pos -= AUDIO_LATENCY_COMPENSATION;
            else
                audio_pos = 0;

            /* Drop late frames: audio is ahead of video by more than threshold */
            while (audio_pos > pts_buf_pos + g_vid.drop_thresh_samples && !app_end) {
                g_vid.frames_dropped++;
                g_vid.frame_counter++;
                g_vid_tail = (g_vid_tail + 1) % VID_BUF_SIZE;

                while (g_vid_head == g_vid_tail && !app_end) pthread_yield_np();
                if (app_end) break;

                raw_pts = g_vid_pts_buf[g_vid_tail];
                pts_buf_pos = (raw_pts != AV_NOPTS_VALUE)
                    ? (uint32_t)(tb * (double)raw_pts * (double)sync_rate)
                    : (uint32_t)(g_vid.frame_counter * (uint32_t)g_vid.time_per_frame);
            }
            if (app_end) break;

            /* Wait if video is ahead of audio */
            while (pts_buf_pos > audio_pos && !app_end) {
                pthread_yield_np();
                audio_pos = g_snd.feed_count * samples_per_buffer
                          + (uint32_t)snd_get_playback_pos();
                if (audio_pos > AUDIO_LATENCY_COMPENSATION)
                    audio_pos -= AUDIO_LATENCY_COMPENSATION;
                else
                    audio_pos = 0;
            }
        } else {
            uint32_t t_end = (uint32_t)st_Supexec(get200hz);
            double elapsed_ms = (double)(t_end - time_200hz_start) * 5.0;
            while (elapsed_ms < g_vid.time_per_frame_wall && !app_end) {
                pthread_yield_np();
                t_end = (uint32_t)st_Supexec(get200hz);
                elapsed_ms = (double)(t_end - time_200hz_start) * 5.0;
            }
            time_200hz_start = (uint32_t)st_Supexec(get200hz);
        }

        mm_wi_mfdb.fd_addr = g_vid_ring_buf + (g_vid_tail * g_vid_buf_size);

        if (mm_ico_win_delta_y > 0) {
            int16_t bpp = g_vid.screen_bpp;
            int16_t bytes_per_pixel = (bpp == 24) ? 3 : (bpp / 8);
            int frame_row_bytes = (int)g_vid.width * bytes_per_pixel;

            mm_control_bar_mfdb.fd_addr = (char *)mm_wi_mfdb.fd_addr +
                (g_vid.height - mm_ico_win_delta_y) * frame_row_bytes;
            mm_control_bar_mfdb.fd_h = mm_ico_win_delta_y;
            /* fd_w, fd_h, fd_wdwidth, fd_stand, fd_nplanes déjà pré-calculés */
            ico_handle(0, 0, 0);
        }

        {
            GRECT rect;
            GRECT dst_rect;
            int16_t win_xy[8];

            dst_rect.g_x = xwork + g_vid_off_x;
            dst_rect.g_y = ywork + g_vid_off_y;
            dst_rect.g_w = g_vid.width;
            dst_rect.g_h = g_vid.height;

            if (g_fullscreen) {
                /* Pas de hide/show/wind_update ici : déjà verrouillé par fullscreen_lock() */
                win_xy[0] = 0;
                win_xy[1] = 0;
                win_xy[2] = (int16_t)g_vid.width - 1;
                win_xy[3] = (int16_t)g_vid.height - 1;
                win_xy[4] = dst_rect.g_x;
                win_xy[5] = dst_rect.g_y;
                win_xy[6] = dst_rect.g_x + (int16_t)g_vid.width - 1;
                win_xy[7] = dst_rect.g_y + (int16_t)g_vid.height - 1;

                set_clip(1, dst_rect.g_x, dst_rect.g_y,
                         (int16_t)g_vid.width, (int16_t)g_vid.height);
                vro_cpyfm(handle, S_ONLY, win_xy, &mm_wi_mfdb, &screen_mfdb);
            } else {
                hide_mouse();
                wind_update(BEG_UPDATE);

                wind_get(wi_handle, WF_FIRSTXYWH,
                         &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
                while (rect.g_h != 0 && rect.g_w != 0) {
                    if (rc_intersect(&dst_rect, &rect)) {
                        int16_t src_x = rect.g_x - dst_rect.g_x;
                        int16_t src_y = rect.g_y - dst_rect.g_y;
                        int16_t w = rect.g_w;
                        int16_t h = rect.g_h;

                        win_xy[0] = src_x;
                        win_xy[1] = src_y;
                        win_xy[2] = src_x + w - 1;
                        win_xy[3] = src_y + h - 1;
                        win_xy[4] = rect.g_x;
                        win_xy[5] = rect.g_y;
                        win_xy[6] = rect.g_x + w - 1;
                        win_xy[7] = rect.g_y + h - 1;

                        set_clip(1, rect.g_x, rect.g_y, rect.g_w, rect.g_h);
                        vro_cpyfm(handle, S_ONLY, win_xy,
                                  &mm_wi_mfdb, &screen_mfdb);
                    }
                    wind_get(wi_handle, WF_NEXTXYWH,
                             &rect.g_x, &rect.g_y, &rect.g_w, &rect.g_h);
                }
                wind_update(END_UPDATE);
                show_mouse();
            }
        }
        
        g_vid_tail = (g_vid_tail + 1) % VID_BUF_SIZE;
        g_vid.frame_counter++;
    }
    return NULL;
}

static void cleanup(void)
{
    AVPacket *tmp_pkt = NULL;
    while (shadow_pop(&tmp_pkt) == 0) { 
        av_packet_free(&tmp_pkt);
    }

    if (g_snd.stream_index >= 0) {
        if (g_snd.swr_ctx) swr_free(&g_snd.swr_ctx);
        if (g_snd.swr_tmp_buf) av_freep(&g_snd.swr_tmp_buf);
        if (g_snd.frame) av_frame_free(&g_snd.frame);
        if (g_snd.codec_ctx) avcodec_free_context(&g_snd.codec_ctx);
        if (g_snd.pBuffer) Mfree(g_snd.pBuffer);
    }
    if (g_vid.stream_index >= 0) {
        if (g_vid.sws_ctx) sws_freeContext(g_vid.sws_ctx);
        if (g_vid.frame_rgb) av_frame_free(&g_vid.frame_rgb); 
        if (g_vid.frame_yuv) av_frame_free(&g_vid.frame_yuv);
        if (g_vid.packet) av_packet_free(&g_vid.packet);
        if (g_vid.codec_ctx) avcodec_free_context(&g_vid.codec_ctx);
        if (g_vid_ring_buf) free(g_vid_ring_buf);
    }
    if (g_vid.fmt_ctx) avformat_close_input(&g_vid.fmt_ctx);

    printf("Frames decoded: %lu  dropped: %lu\n",
           (unsigned long)g_vid.frame_counter, (unsigned long)g_vid.frames_dropped);
}

int main(int argc, char *argv[])
{
    pthread_t thread_demux, thread_vid_display_id, thread_audio, thread_eventloop, thread_aud_ui;
    char path_buf[512];
    const char *filename = NULL;

set_bin_directory(argv[0]);

    int first_file_arg = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disable-resample") == 0) {
            g_disable_resample = true;
        } else if (strcmp(argv[i], "--direct-play") == 0) {
            g_direct_play = true;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            g_fs_requested = true;            
        } else if (first_file_arg < 0) {
            first_file_arg = i;
        }
    }

    if (first_file_arg >= 0) {
        /* Reconstruit le chemin en joignant les arguments restants avec des espaces.
           Cela corrige le cas où un chemin avec espace a été découpé par le shell. */
        size_t pos = 0;
        for (int i = first_file_arg; i < argc && pos < sizeof(path_buf) - 1; i++) {
            if (i > first_file_arg) {
                if (pos < sizeof(path_buf) - 1)
                    path_buf[pos++] = ' ';
            }
            size_t arglen = strlen(argv[i]);
            if (pos + arglen >= sizeof(path_buf) - 1)
                arglen = sizeof(path_buf) - 1 - pos;
            if (arglen > 0)
                memcpy(path_buf + pos, argv[i], arglen);
            pos += arglen;
        }
        path_buf[pos] = '\0';

        /* Certains C runtimes Atari conservent les guillemets — on les retire */
        size_t len = strlen(path_buf);
        if (len >= 2 && ((path_buf[0] == '"' && path_buf[len - 1] == '"') ||
                         (path_buf[0] == '\'' && path_buf[len - 1] == '\''))) {
            memmove(path_buf, path_buf + 1, len - 2);
            path_buf[len - 2] = '\0';
        }

        normalize_path(path_buf);
        filename  = path_buf;
        win_title = get_basename(path_buf);
    }

    if (filename == NULL) {
        printf("Usage: mm_mint_ffmpeg.prg [--disable-resample] [--direct-play] <mediafile>\n");
        return 1;
    }

    /* ---------- GEM init: once ---------- */
    appl_init();
    phys_handle = graf_handle(&gl_wchar, &gl_hchar, &gl_wbox, &gl_hbox);
    wind_get(0, WF_WORKXYWH, &xdesk, &ydesk, &wdesk, &hdesk);
    open_vwork();
    vq_extnd(handle, 1, work_out_extended);
    graf_mouse(ARROW, NULL);
    cursor_is_hidden = FALSE;
    av_log_set_level(AV_LOG_QUIET);

    g_saved_x = xdesk + 80;
    g_saved_y = ydesk + 20;
    g_saved_w = 0;
    g_saved_h = 0;

    bool first_run = true;

    do {   /* restart loop */
        app_end = false;
        g_restart_requested = false;
        g_playback_started = false;
        g_audio_only = false;
        g_splash_active = false;
        g_demux_finished = false;

        memset(&g_vid, 0, sizeof(g_vid));
        memset(&g_snd, 0, sizeof(g_snd));
        g_vid.stream_index = -1;
        g_snd.stream_index = -1;

        /* Reset shadow queue */
        g_shadow_head = 0;
        g_shadow_tail = 0;
        memset(g_audio_shadow, 0, sizeof(g_audio_shadow));

        if (!first_run)
            g_fs_requested = false;   // preserve --fullscreen arg on first pass

        g_fullscreen = false;
        g_saved_w = 0;
        g_saved_h = 0;

        detect_machine_type();
        vid_init(filename);
        if (app_end) break;

        if (g_vid.stream_index < 0 && g_snd.stream_index < 0) {
            printf("ERROR: No playable stream found\n");
            break;
        }

        if (first_run) {
            ico_init();

            if (g_vid.stream_index >= 0) {
                int win_w = (int)g_vid.width;
                int win_h = (int)g_vid.height;
                if (win_w > wdesk - 20) win_w = wdesk - 20;
                if (win_h > hdesk - 80) win_h = hdesk - 80;
                /* Prépare une géométrie fenêtrée par défaut au cas où on toggle */
                g_saved_x = xdesk + 80;
                g_saved_y = ydesk + 20;

                int16_t tmp_ox, tmp_oy;
                int16_t def_w = (int16_t)win_w;
                int16_t def_h = (int16_t)win_h;

                wind_calc(WC_BORDER, MOVER | CLOSER | NAME,
                          0, 0, def_w, def_h,
                          &tmp_ox, &tmp_oy, &g_saved_w, &g_saved_h);

                /* Prépare une géométrie fenêtrée par défaut au cas où on toggle */
                {
                    int16_t tmp_ox, tmp_oy;
                    wind_calc(WC_BORDER, MOVER | CLOSER | NAME,
                              0, 0, win_w, win_h,
                              &tmp_ox, &tmp_oy, &g_saved_w, &g_saved_h);
                    g_saved_x = xdesk + 80;
                    g_saved_y = ydesk + 20;
                }

                if (g_fs_requested) {
                    fullscreen_lock();
                    wi_handle = wind_create(0, xdesk, ydesk, wdesk, hdesk);
                    wind_open(wi_handle, xdesk, ydesk, wdesk, hdesk);
                    wind_set(wi_handle, WF_TOP, 0, 0, 0, 0);
                    wind_get(wi_handle, WF_WORKXYWH, &xwork, &ywork, &wwork, &hwork);
                    wind_get(wi_handle, WF_CURRXYWH, &xext, &yext, &wext, &hext);

                    vid_compute_fullscreen_offset();
                    vid_fill_black_bars_once();                   
                    mm_ico_win_delta_y = 0;
                    g_fullscreen = true;
                } else {
                    open_window(win_w, win_h, win_title);
                }

                ico_update_x(2, wwork - 72, -1);
                if (g_direct_play) {
                    g_playback_started = true;
                    g_splash_active = false;
                    g_vid.is_paused = false;
                    g_snd.is_paused = false;
                } else {
                    splash_init();
                    g_vid.is_paused = true;
                    g_snd.is_paused = true;
                }
            } else if (g_snd.stream_index >= 0) {
                g_audio_only = true;
                mm_ico_win_delta_y = 40;

                int16_t logo_h = (ico_ffmpeg.mfdb.fd_addr != NULL) ? ico_ffmpeg.mfdb.fd_h : 0;
                int16_t logo_w = (ico_ffmpeg.mfdb.fd_addr != NULL) ? ico_ffmpeg.mfdb.fd_w : 0;

                int16_t content_h = (int16_t)MAX(60, logo_h + 16);
                int win_w = (int)MAX(320, logo_w + 16);
                int win_h = (int)(content_h + mm_ico_win_delta_y);

                open_window(win_w, win_h, win_title);

                int16_t buf_w    = wwork;
                int16_t buf_h    = hwork;
                int16_t nb_px    = (int16_t)(MFDB_STRIDE(buf_w) - buf_w);
                int16_t stride   = buf_w + nb_px;
                int16_t bpp      = work_out_extended[4];
                int16_t bytes_pp = (bpp == 24) ? 3 : (bpp / 8);
                uint32_t buf_size = (uint32_t)stride * buf_h * bytes_pp;

                uint8_t *buf = (uint8_t *)malloc(buf_size);
                if (buf) {
                    for (int16_t y = 0; y < buf_h; y++) {
                        for (int16_t x = 0; x < stride; x++) {
                            uint32_t i = ((uint32_t)y * stride + x) * bytes_pp;
                            if (bpp == 32) {
                                buf[i] = 0xFF; buf[i+1] = SPLASH_GREY_R;
                                buf[i+2] = SPLASH_GREY_G; buf[i+3] = SPLASH_GREY_B;
                            } else if (bpp == 24) {
                                buf[i] = SPLASH_GREY_R; buf[i+1] = SPLASH_GREY_G;
                                buf[i+2] = SPLASH_GREY_B;
                            } else if (bpp == 16) {
                                uint16_t c = (uint16_t)(((SPLASH_GREY_R & 0xF8) << 8) |
                                                        ((SPLASH_GREY_G & 0xFC) << 3) |
                                                        (SPLASH_GREY_B >> 3));
                                buf[i] = (uint8_t)((c >> 8) & 0xFF);   /* big-endian */
                                buf[i+1] = (uint8_t)(c & 0xFF);
                            }
                        }
                    }

                    mm_wi_mfdb.fd_addr    = (char *)buf;
                    mm_wi_mfdb.fd_w       = stride;
                    mm_wi_mfdb.fd_h       = buf_h;
                    mm_wi_mfdb.fd_wdwidth = MFDB_STRIDE(stride) >> 4;
                    mm_wi_mfdb.fd_stand   = 0;
                    mm_wi_mfdb.fd_nplanes = bpp;

                    int16_t bar_row  = buf_h - mm_ico_win_delta_y;
                    uint8_t *bar_buf = buf + (uint32_t)bar_row * stride * bytes_pp;
                    mm_control_bar_mfdb.fd_addr    = (char *)bar_buf;
                    mm_control_bar_mfdb.fd_h       = mm_ico_win_delta_y;
                    mm_control_bar_mfdb.fd_w       = stride;
                    mm_control_bar_mfdb.fd_stand   = 0;
                    mm_control_bar_mfdb.fd_wdwidth = MFDB_STRIDE(stride) >> 4;
                    mm_control_bar_mfdb.fd_nplanes = bpp;

                    mm_ico_pxy_control_bar[0] = xwork;
                    mm_ico_pxy_control_bar[1] = ywork + buf_h - mm_ico_win_delta_y;
                    mm_ico_pxy_control_bar[2] = xwork + wwork;
                    mm_ico_pxy_control_bar[3] = ywork + buf_h;
                }

                ico_update_x(2, wwork - 72, -1);
                ico_handle(0, 0, 0);

                if (ico_ffmpeg.mfdb.fd_addr != NULL) {
                    int16_t content_area_h = buf_h - mm_ico_win_delta_y;
                    ico_ffmpeg.x  = (int16_t)((stride - logo_w) >> 1);
                    ico_ffmpeg.y  = (int16_t)((content_area_h - logo_h) >> 1);
                    if (ico_ffmpeg.y < 4) ico_ffmpeg.y = 4;
                    ico_ffmpeg.x2 = ico_ffmpeg.x + logo_w;
                    ico_ffmpeg.y2 = ico_ffmpeg.y + logo_h;
                    ico_load(&mm_wi_mfdb, &ico_ffmpeg);
                }

                g_playback_started = true;
            }

            st_Send_WM_REDRAW();
            first_run = false;
        } else {
            /* Restart: keep window, refill background, reset state */
            wind_set_str(wi_handle, WF_NAME, win_title);

            /* reset icons to default playing/unmuted state */
            control_bar[0].alt_state = false;
            control_bar[2].alt_state = false;

            if (g_vid.stream_index >= 0) {
                if (g_direct_play) {
                    g_playback_started = true;
                    g_splash_active = false;
                    g_vid.is_paused = false;
                    g_snd.is_paused = false;
                } else {
                    splash_init();
                    g_vid.is_paused = true;
                    g_snd.is_paused = true;
                }
            } else {
                g_audio_only = true;
                g_playback_started = true;
                audio_only_fill_background();
            }
            st_Send_WM_REDRAW();
        }

        pthread_create(&thread_eventloop, NULL, exec_eventloop, NULL);

        if (g_snd.stream_index >= 0) {
            snd_init_dma();
            if (app_end) goto loop_end;
            pthread_create(&thread_audio, NULL, thread_snd_play, NULL);
        }

        if (g_vid.stream_index >= 0 || g_snd.stream_index >= 0) {
            pthread_create(&thread_demux, NULL, thread_demux_decode, NULL);
        }

        if (g_audio_only) {
            pthread_create(&thread_aud_ui, NULL, thread_audio_ui, NULL);
        }

        if (g_vid.stream_index >= 0) {
            pthread_create(&thread_vid_display_id, NULL, thread_vid_display, NULL);
            pthread_join(thread_vid_display_id, NULL);
            app_end = true;
            pthread_join(thread_demux, NULL);
            if (g_snd.stream_index >= 0) {
                pthread_join(thread_audio, NULL);
                snd_unset_dma();
            }
            if (g_audio_only) pthread_join(thread_aud_ui, NULL);
            pthread_join(thread_eventloop, NULL);
        } else if (g_snd.stream_index >= 0) {
            pthread_join(thread_eventloop, NULL);
            app_end = true;
            pthread_join(thread_demux, NULL);
            pthread_join(thread_audio, NULL);
            snd_unset_dma();
            pthread_join(thread_aud_ui, NULL);
        }

    loop_end:
        splash_free();
        /* CHANGED: do NOT free audio-only background buffer here —
         * it is reused across restarts. Freed once after the loop. */
        cleanup();
    } while (g_restart_requested);

    /* ---------- GEM shutdown: once ---------- */
    if (g_audio_only && mm_wi_mfdb.fd_addr != NULL) {
        free(mm_wi_mfdb.fd_addr);
        mm_wi_mfdb.fd_addr = NULL;
    }
    ico_free();
    if (g_fullscreen) {
        fullscreen_unlock();
    }    
    wind_close(wi_handle);
    wind_delete(wi_handle);
    v_clsvwk(handle);
    appl_exit();
    return 0;
}
