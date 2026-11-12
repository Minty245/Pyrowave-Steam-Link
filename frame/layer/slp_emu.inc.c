// Steam Link Pyrowave - V4L2 decoder stand-in (included into slp_layer.c).
// SPDX-License-Identifier: MIT
//
// v7 auto-detect: armed by /tmp/slp_decode (one boot) or ~/.local/lib/slp/AUTO (persistent).  vrlink's
// decoder open always reaches the hardware.  Until the first OUTPUT frame every call goes to the real
// driver; if that frame starts with the SLPW magic the session is taken over (the hardware keeps its
// OUTPUT buffers, which we read; everything else is emulated), otherwise it stays on the hardware
// untouched for the whole session (stock HEVC).  The emulated conversation mirrors the iris driver:
//
//   S_FMT OUT (HEVC) -> REQBUFS OUT 16 MMAP -> QUERYBUF/mmap -> SUBSCRIBE SOURCE_CHANGE -> STREAMON OUT
//   QBUF OUT (one SLPW frame each) ...
//   first full SLPW frame -> SOURCE_CHANGE -> G_FMT CAP 1344x5376 Q08C (8-bit NV12 UBWC; see EMU_BITS) -> REQBUFS CAP 16 DMABUF
//   -> QBUF CAP (vrlink's own UBWC P010 dma-bufs) -> STREAMON CAP
//   steady state: worker decodes each OUT frame with Pyrowave straight into the next queued CAP
//   surface, then the buffers become dequeueable.
//
// Wake-ups: vrlink's decoder thread waits in epoll_wait(epfd, max 1, -1) and handles EPOLLIN (DQBUF CAP),
// EPOLLOUT (DQBUF OUT) and EPOLLPRI (DQEVENT).  We register an eventfd in vrlink's epoll set under
// vrlink's data value and rewrite the returned event mask from the emulated queues.

#include <sys/eventfd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct slpd_timing_l { double cpu_ms, gpu_decode_ms, gpu_pack_copy_ms; int complete; } slpd_timing_l;
typedef void *(*pfn_slpd_create_l)(int, int, int, char *, size_t);
typedef int (*pfn_slpd_import_l)(void *, int, uint64_t, const uint64_t *, const uint64_t *, uint64_t, char *, size_t);
typedef void (*pfn_slpd_release_l)(void *, int);
typedef int (*pfn_slpd_decode_l)(void *, const void *, size_t, int, slpd_timing_l *, char *, size_t);
// v7.1 decoder API (optional): output modes and a finish/copy split.
typedef struct slpd_timing2_l { double cpu_ms, gpu_decode_ms, gpu_pack_copy_ms, gpu_finish_ms, gpu_copy_ms; int complete, direct; } slpd_timing2_l;
typedef void *(*pfn_slpd_create2_l)(int, int, int, int, char *, size_t);
typedef int (*pfn_slpd_decode2_l)(void *, const void *, size_t, int, slpd_timing2_l *, char *, size_t);
typedef void (*pfn_slpd_alias_status_l)(void *, char *, size_t);
typedef void (*pfn_slpd_set_dither_l)(void *, int);
typedef int (*pfn_slpd_set_postfx_l)(void *, float, float, float, float);
typedef int (*pfn_slpd_set_postfx2_l)(void *, float, float, float, float, float, float);
typedef void (*pfn_slpd_set_postfx_mode_l)(void *, int);
typedef void (*pfn_slpd_set_postfx_seam_l)(void *, int, int);
typedef void (*pfn_slpd_set_postfx_blur_l)(void *, float, float);
typedef void (*pfn_slpd_set_ramp_l)(void *, const float *);
typedef void (*pfn_slpd_set_postfx_temporal_l)(void *, float, float);
typedef void (*pfn_slpd_destroy_l)(void *);
typedef int (*pfn_slpd_submit_l)(void *, const void *, size_t, int, char *, size_t);
typedef int (*pfn_slpd_wait_l)(void *, int, slpd_timing2_l *, char *, size_t);
// v7.19 full colour (decoder mode 6)
typedef struct slpd_rgba_l { int fd; uint32_t width, height, vk_format; uint64_t modifier, offset, row_pitch, size; } slpd_rgba_l;
typedef int (*pfn_slpd_full_colour_l)(void *);
typedef int (*pfn_slpd_rgba_export_l)(void *, int, slpd_rgba_l *, char *, size_t);
typedef void (*pfn_slpd_set_target_output_l)(void *, int, int);
typedef int (*pfn_slpd_target_output_l)(void *, int);
typedef int (*pfn_slpd_frame_chroma_l)(const void *, size_t);
// v7.21 power options and per-pass GPU times (optional)
typedef void (*pfn_slpd_set_merge_l)(void *, int);
typedef void (*pfn_slpd_set_rgba_compressed_l)(int);
typedef int (*pfn_slpd_pass_report_l)(void *, double *, double *);

static __thread int g_internal;   // set while our own decoder creates its Vulkan instance

// The stand-in reports an 8-bit decoder (Q08C), so vrlink allocates NV12 UBWC surfaces: the Frame's Turnip
// cannot write UBWC P010/TP10 from the GPU (self-test v3), but NV12 UBWC copies are exact.
#define EMU_BITS 8
// vrlink's decode-surface layout, captured in vkCreateImage (defaults = vrlink's Q08C formula at 1344x5376).
static uint64_t g_cl_mod = 0x0500000000000001ull;
static uint64_t g_cl_off[2] = {0, 7614464}, g_cl_pitch[2] = {1408, 1408};
static int g_cl_seen;
static volatile uint32_t g_cl_want_w = 1344;   // width of the decode surfaces we expect vrlink to create

// v7.4: any stream size. Venus / iris NV12 UBWC layout (msm_media_info.h): plane 0 = Y metadata + Y data,
// plane 1 = UV metadata + UV data. Matches iris at 1344x5376 (bpl 1408, plane 1 at 7,614,464, 11,423,744 B).
static void emu_nv12_ubwc_layout(uint32_t w, uint32_t h, uint32_t *bpl, uint64_t *off1, uint32_t *size)
{
#define AL(v, a) ((((uint64_t)(v)) + (a) - 1) / (a) * (a))
    uint64_t stride = AL(w, 128);
    uint64_t y_meta = AL(AL((w + 31) / 32, 64) * AL((h + 7) / 8, 16), 4096);
    uint64_t y_plane = stride * AL(h, 32);
    uint64_t uv_meta = AL(AL((w / 2 + 15) / 16, 64) * AL((h / 2 + 7) / 8, 16), 4096);
    uint64_t uv_plane = stride * AL((h + 1) / 2, 32);
    *bpl = (uint32_t)stride;
    *off1 = y_meta + y_plane;
    *size = (uint32_t)(y_meta + y_plane + uv_meta + uv_plane);
#undef AL
}

#define EMU_NBUF 32
#define EMU_RING 64

typedef struct { uint32_t idx; struct timeval ts; uint32_t bytes; uint32_t flags; double t_q; } emu_item;
typedef struct { emu_item a[EMU_RING]; int h, n; } emu_ring;

static int ring_push(emu_ring *r, emu_item it)
{
    if (r->n >= EMU_RING) return -1;
    r->a[(r->h + r->n) % EMU_RING] = it;
    r->n++;
    return 0;
}
static int ring_pop(emu_ring *r, emu_item *out)
{
    if (!r->n) return -1;
    *out = r->a[r->h];
    r->h = (r->h + 1) % EMU_RING;
    r->n--;
    return 0;
}

static struct {
    int fd;                  // vrlink's decoder fd once taken over (-1 = none); still the real /dev/video22 fd
    int efd, efd_set;        // readiness eventfd
    int epfd;                // vrlink's epoll set (-1 until epoll_ctl)
    uint64_t epdata;
    pthread_mutex_t mu;
    pthread_cond_t cv;       // job queued / CAP queued / stop
    pthread_cond_t dq_cv;    // something dequeueable (blocking DQBUF)
    // OUTPUT
    uint32_t out_w, out_h, out_fourcc, out_sizeimage, out_count;
    uint64_t out_off[EMU_NBUF];          // QUERYBUF mem_offset per OUTPUT buffer (hardware's)
    uint8_t *out_addr[EMU_NBUF];         // vrlink's own mmap of each hardware OUTPUT buffer
    size_t out_len[EMU_NBUF];
    int out_streaming;
    emu_ring jobs, out_done;
    // CAPTURE
    uint32_t cap_w, cap_h, cap_count, cap_sizeimage, cap_bpl;
    int cap_fmt_ready, cap_streaming;
    int cap_fd[EMU_NBUF], cap_target[EMU_NBUF], cap_queued[EMU_NBUF];
    uint64_t cap_ino[EMU_NBUF];   // v7.19: dma-buf inode of each CAPTURE surface (full-colour pairing)
    int imp_pending;              // v7.19: worker should import the queued surfaces now (full colour)
    uint64_t cap_qseq[EMU_NBUF], qseq;
    emu_ring cap_done;
    // events
    int subscribed, ev_pending;
    uint32_t ev_seq;
    // worker
    pthread_t worker;
    int worker_running, stop;
} g_emu = {.fd = -1, .efd = -1, .epfd = -1, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
           .dq_cv = PTHREAD_COND_INITIALIZER};

// Decoder library (loaded once, kept for the life of vrlink; surfaces are released per session).
static void *g_dec;
static pfn_slpd_create_l p_create;
static pfn_slpd_import_l p_import;
static pfn_slpd_release_l p_release;
static pfn_slpd_decode_l p_decode;
static pfn_slpd_create2_l p_create2;
static pfn_slpd_decode2_l p_decode2;
static pfn_slpd_alias_status_l p_alias_status;
static pfn_slpd_set_dither_l p_set_dither;
static pfn_slpd_set_postfx_l p_set_postfx;
static pfn_slpd_set_postfx2_l p_set_postfx2;
static pfn_slpd_set_postfx_mode_l p_set_postfx_mode;
static pfn_slpd_set_postfx_seam_l p_set_postfx_seam;
static pfn_slpd_set_postfx_blur_l p_set_postfx_blur;   /* v7.13 */
static pfn_slpd_set_ramp_l p_set_ramp;                 /* v7.14 */
static pfn_slpd_set_postfx_temporal_l p_set_temporal;  /* v7.17 */
static pfn_slpd_destroy_l p_destroy;
static pfn_slpd_submit_l p_submit;
static pfn_slpd_wait_l p_wait;
static pfn_slpd_full_colour_l p_full_colour;              /* v7.19 */
static pfn_slpd_rgba_export_l p_rgba_export;
static pfn_slpd_set_target_output_l p_set_target_output;
static pfn_slpd_target_output_l p_target_output;
static pfn_slpd_frame_chroma_l p_frame_chroma;
static pfn_slpd_set_merge_l p_set_merge, p_set_fast_pyramid;   /* v7.21 */
static pfn_slpd_set_rgba_compressed_l p_set_rgba_compressed;
static pfn_slpd_pass_report_l p_pass_report;
static int g_rgba_compressed, g_no_merge, g_exact_pyramid;     /* v7.21: arm files, see below */
static int g_fc_armed;      // v7.19: ~/.local/lib/slp/FULLCOLOUR or /tmp/slp_fullcolour present
static int g_fc;            // v7.19: the current decoder is in full-colour mode
static pthread_mutex_t g_fc_mu = PTHREAD_MUTEX_INITIALIZER;   // g_dec lifetime vs the full-colour socket thread
static pthread_cond_t g_fc_cv = PTHREAD_COND_INITIALIZER;     // with g_emu.mu: an eager import finished
static int g_mode_420;      // v7.19: the MODE to fall back to if the full-colour decoder cannot be created
static int g_out_mode;      // ~/.local/lib/slp/MODE: 0 (default), 1, 4, 5 -- see set_mode.sh
static int g_no_dither;     // ~/.local/lib/slp/NODITHER present
static int g_dec_failed;

#if defined(__aarch64__)   // keep the layer loadable on glibc >= 2.17, like vrlink itself
__asm__(".symver dlopen,dlopen@GLIBC_2.17");
__asm__(".symver dlerror,dlerror@GLIBC_2.17");
__asm__(".symver pthread_create,pthread_create@GLIBC_2.17");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.17");
#endif

static int emu_auto_file(void)
{
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/.local/lib/slp/AUTO", home ? home : "/home/steamos");
    return file_exists(path);
}
static int emu_armed(void) { return file_exists("/tmp/slp_decode") || emu_auto_file(); }

// Candidate session: a hardware decoder fd opened while armed, not yet decided.
typedef struct { uint64_t off; uint8_t *addr; size_t len; } emu_map;
static struct {
    int fd;                       // -1 = none
    uint32_t out_count, sizeimage, out_w, out_h, fourcc;
    uint64_t out_off[EMU_NBUF];
    emu_map maps[EMU_NBUF];
    int subscribed, out_streaming;
    int epfd;
    uint64_t epdata;
} g_cand = {.fd = -1, .epfd = -1};
static int emu_is_cand(int fd) { return fd >= 0 && fd == g_cand.fd; }
static int emu_is_fd(int fd) { return fd >= 0 && fd == g_emu.fd; }

// Caller holds g_emu.mu.
static void emu_update_ready_locked(void)
{
    int ready = g_emu.cap_done.n > 0 || g_emu.out_done.n > 0 || g_emu.ev_pending;
    if (ready && !g_emu.efd_set && g_emu.efd >= 0) {
        uint64_t one = 1;
        if (write(g_emu.efd, &one, sizeof(one)) == sizeof(one)) g_emu.efd_set = 1;
    } else if (!ready && g_emu.efd_set && g_emu.efd >= 0) {
        uint64_t v;
        if (read(g_emu.efd, &v, sizeof(v)) == sizeof(v) || errno == EAGAIN) g_emu.efd_set = 0;
    }
    if (ready) pthread_cond_broadcast(&g_emu.dq_cv);
}

static uint32_t emu_ready_mask_locked(void)
{
    uint32_t m = 0;
    if (g_emu.cap_done.n) m |= EPOLLIN | EPOLLRDNORM;
    if (g_emu.out_done.n) m |= EPOLLOUT | EPOLLWRNORM;
    if (g_emu.ev_pending) m |= EPOLLPRI;
    return m;
}

static uint32_t g_dec_w, g_dec_h;

static int emu_load_library(void)
{
    if (p_create) return 1;
    if (g_dec_failed) return 0;
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s/.local/lib/slp/libslp_pyrodec.so", home ? home : "/home/steamos");
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { slog("EMU: dlopen(%s) failed: %s", path, dlerror()); g_dec_failed = 1; return 0; }
    p_create = (pfn_slpd_create_l)dlsym(h, "slpd_create");
    p_import = (pfn_slpd_import_l)dlsym(h, "slpd_import");
    p_release = (pfn_slpd_release_l)dlsym(h, "slpd_release");
    p_decode = (pfn_slpd_decode_l)dlsym(h, "slpd_decode");
    if (!p_create || !p_import || !p_release || !p_decode) { slog("EMU: decoder library is missing symbols"); g_dec_failed = 1; return 0; }
    p_create2 = (pfn_slpd_create2_l)dlsym(h, "slpd_create2");
    p_decode2 = (pfn_slpd_decode2_l)dlsym(h, "slpd_decode2");
    p_alias_status = (pfn_slpd_alias_status_l)dlsym(h, "slpd_alias_status");
    p_set_dither = (pfn_slpd_set_dither_l)dlsym(h, "slpd_set_dither");
    p_set_postfx = (pfn_slpd_set_postfx_l)dlsym(h, "slpd_set_postfx");
    p_set_postfx2 = (pfn_slpd_set_postfx2_l)dlsym(h, "slpd_set_postfx2");
    p_set_postfx_mode = (pfn_slpd_set_postfx_mode_l)dlsym(h, "slpd_set_postfx_mode");
    p_set_postfx_seam = (pfn_slpd_set_postfx_seam_l)dlsym(h, "slpd_set_postfx_seam");
    p_set_postfx_blur = (pfn_slpd_set_postfx_blur_l)dlsym(h, "slpd_set_postfx_blur");
    p_set_ramp = (pfn_slpd_set_ramp_l)dlsym(h, "slpd_set_ramp");
    p_set_temporal = (pfn_slpd_set_postfx_temporal_l)dlsym(h, "slpd_set_postfx_temporal");
    p_destroy = (pfn_slpd_destroy_l)dlsym(h, "slpd_destroy");
    p_submit = (pfn_slpd_submit_l)dlsym(h, "slpd_submit");
    p_wait = (pfn_slpd_wait_l)dlsym(h, "slpd_wait");
    if (!p_submit || !p_wait) p_submit = NULL, p_wait = NULL;
    if (!p_create2 || !p_decode2) p_create2 = NULL, p_decode2 = NULL;
    p_full_colour = (pfn_slpd_full_colour_l)dlsym(h, "slpd_full_colour");
    p_rgba_export = (pfn_slpd_rgba_export_l)dlsym(h, "slpd_rgba_export");
    p_set_target_output = (pfn_slpd_set_target_output_l)dlsym(h, "slpd_set_target_output");
    p_target_output = (pfn_slpd_target_output_l)dlsym(h, "slpd_target_output");
    p_frame_chroma = (pfn_slpd_frame_chroma_l)dlsym(h, "slpd_frame_chroma");
    p_set_merge = (pfn_slpd_set_merge_l)dlsym(h, "slpd_set_merge");
    p_set_fast_pyramid = (pfn_slpd_set_merge_l)dlsym(h, "slpd_set_fast_pyramid");
    p_set_rgba_compressed = (pfn_slpd_set_rgba_compressed_l)dlsym(h, "slpd_set_rgba_compressed");
    p_pass_report = (pfn_slpd_pass_report_l)dlsym(h, "slpd_pass_report");
    {
        typedef const char *(*pfn_ver)(void);
        pfn_ver ver = (pfn_ver)dlsym(h, "slpd_version");
        slog("EMU: decoder library %s", ver ? ver() : "older than v7.22");
    }
    {
        char mp[512];
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/MODE", home ? home : "/home/steamos");
        g_out_mode = 1;   // v7.22: default = mode 1 (8-bit straight from Pyrowave; the tested, lighter path)
        int mfd = open(mp, O_RDONLY | O_CLOEXEC);
        if (mfd >= 0) {
            char c = 0;
            if (read(mfd, &c, 1) == 1 && c >= '0' && c <= '5') g_out_mode = c - '0';   // 6 only via FULLCOLOUR
            close(mfd);
        }
        if (g_out_mode == 5) {
            slog("EMU: MODE 5 writes a corrupt luma plane on this GPU driver; using mode 4");
            g_out_mode = 4;
        }
        if (g_out_mode == 2 || g_out_mode == 3) {
            slog("EMU: MODE %d is broken on this GPU driver (corrupt picture); using mode 0", g_out_mode);
            g_out_mode = 0;
        }
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/NODITHER", home ? home : "/home/steamos");
        g_no_dither = file_exists(mp);
        // v7.21 power options (files in ~/.local/lib/slp, see power_options.sh): NO_MERGE / EXACT_PYRAMID = back to the
        // v7.20 passes. v7.22: compressed (UBWC) full-colour images are the default; RGBA_LINEAR turns them off.
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/RGBA_LINEAR", home ? home : "/home/steamos");
        g_rgba_compressed = !file_exists(mp);
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/NO_MERGE", home ? home : "/home/steamos");
        g_no_merge = file_exists(mp);
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/EXACT_PYRAMID", home ? home : "/home/steamos");
        g_exact_pyramid = file_exists(mp);
        if (g_out_mode && !p_create2) { slog("EMU: MODE %d needs the newer decoder library; using mode 0", g_out_mode); g_out_mode = 0; }
        // v7.19 full colour: decoder mode 6 (overrides MODE). Off by default; see fullcolour_arm.sh.
        snprintf(mp, sizeof(mp), "%s/.local/lib/slp/FULLCOLOUR", home ? home : "/home/steamos");
        g_fc_armed = file_exists(mp) || file_exists("/tmp/slp_fullcolour");
        if (g_fc_armed) {
            if (p_create2 && p_full_colour && p_rgba_export && p_set_target_output && p_target_output && p_frame_chroma) {
                if (g_out_mode != 6) slog("EMU: FULL COLOUR armed: decoder mode 6 (instead of MODE %d)", g_out_mode);
                g_mode_420 = g_out_mode;
                g_out_mode = 6;
            } else {
                slog("EMU: FULL COLOUR armed, but the decoder library is too old (no slpd_full_colour); staying 4:2:0");
                g_fc_armed = 0;
            }
        }
    }
    return 1;
}

// v7.5: ~/.local/lib/slp/postfx.txt ("deblock=1", "sharpen=0.4", "sharpen_r0=0.35", "sharpen_r1=0.8"), re-read
// within ~1 s of a change. Missing file = filter off.
// v7.11: the PC sends the post-filter settings inside the stream: a 48-byte "SLPS" trailer after the packets of
// every SLPW frame (see frame/decoder/slp_pyro_dec.hpp). A local ~/.local/lib/slp/postfx.txt, if present, overrides
// it (developer use). Without either the filter is off.
typedef struct {
    float deblock, deblock_inset, sharpen, r0, r1, wide_step, mode, coarse, bpp;
    int on;
    float blur, blur_contrast;   /* v7.13: wide-view blur px, contrast restoration (trailer bytes 40..47) */
} emu_pfx;
static struct { int have; uint32_t gen; emu_pfx v; } g_ib;   // decode thread only

/* v7.14: per-frame "SLPR" block in front of the SLPS trailer (PC backend v45+): the shared edge ramp and where each
   inset is (see pc/encoder/slp_nvenc_hook.cpp append_ramp). Passed straight to the decoder. */
static void emu_ramp_parse(const uint8_t *p, uint32_t n)
{
    static float last[6] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    if (!p_set_ramp || !g_dec || n < 32 + 48 + 72) return;
    /* v7.17: version 2 is 80 bytes (+ headset stability, clamp); version 1 (backend v45-v47) 72 */
    const uint8_t *r = NULL;
    uint32_t magic;
    uint16_t ver, size;
    for (int v = 2; v >= 1 && !r; v--) {
        uint32_t sz = v == 2 ? 80u : 72u;
        if (n < 32 + 48 + sz) continue;
        const uint8_t *c = p + n - 48 - sz;
        memcpy(&magic, c, 4);
        memcpy(&ver, c + 4, 2);
        memcpy(&size, c + 6, 2);
        if (magic == 0x52504c53u && ver == v && size == sz) r = c;
    }
    if (!r) return;
    float tm[2] = {0.0f, 1.0f};
    if (ver == 2) {
        memcpy(tm, r + 72, 8);
        if (!(tm[0] >= 0.0f && tm[0] <= 1.0f)) tm[0] = 0.0f;
        if (!(tm[1] >= 0.0f && tm[1] <= 4.0f)) tm[1] = 1.0f;
    }
    if (p_set_temporal) p_set_temporal(g_dec, (g_ib.have && !g_ib.v.on) ? 0.0f : tm[0], tm[1]);   /* PC post-filter switch */
    float f[16];
    memcpy(f, r + 8, 64);
    for (int i = 0; i < 16; i++) if (!(f[i] > -100.0f && f[i] < 100.0f)) return;   /* also rejects NaN */
    p_set_ramp(g_dec, f);
    if (memcmp(last, f, 4 * sizeof(float)) != 0 || last[4] != tm[0] || last[5] != tm[1]) {
        memcpy(last, f, 4 * sizeof(float));
        last[4] = tm[0]; last[5] = tm[1];
        slog("EMU: edge ramp from the PC: %.2f (%.0f-%.0f deg), contrast overshoot %.2f, headset stability %.2f (clamp %.2f)",
             f[0], f[1], f[2], f[3], tm[0], tm[1]);
    }
}

static void emu_inband_parse(const uint8_t *p, uint32_t n)
{
    emu_ramp_parse(p, n);
    if (n < 32 + 48) return;
    const uint8_t *t = p + n - 48;
    uint32_t magic, gen;
    uint16_t ver, size;
    memcpy(&magic, t, 4);
    memcpy(&ver, t + 4, 2);
    memcpy(&size, t + 6, 2);
    if (magic != 0x53504c53u || ver != 1 || size != 48) return;
    memcpy(&gen, t + 8, 4);
    if (g_ib.have && gen == g_ib.gen) return;
    float f[6];
    memcpy(f, t + 12, 24);
    for (int i = 0; i < 6; i++) if (!(f[i] >= 0.0f && f[i] <= 8.0f)) return;   // also rejects NaN
    g_ib.v.deblock = f[0]; g_ib.v.deblock_inset = f[1]; g_ib.v.sharpen = f[2];
    g_ib.v.r0 = f[3]; g_ib.v.r1 = f[4]; g_ib.v.wide_step = f[5];
    g_ib.v.mode = t[36]; g_ib.v.coarse = t[37]; g_ib.v.bpp = t[38]; g_ib.v.on = t[39] & 1;
    /* v7.13: the former reserved words carry the wide-view blur (0 from older PCs = off) */
    float b[2];
    memcpy(b, t + 40, 8);
    g_ib.v.blur = (b[0] >= 0.0f && b[0] <= 8.0f) ? b[0] : 0.0f;
    g_ib.v.blur_contrast = (b[1] >= 0.0f && b[1] <= 3.0f) ? b[1] : 1.0f;
    g_ib.gen = gen;
    g_ib.have = 1;
}

static void emu_postfx_poll(int force)
{
    // Re-read the small local file once a second (no stat()/sscanf(): both would pull in newer glibc symbol versions
    // than vrlink's baseline) and apply whichever source is active when it changed.
    static double last_check;
    static int had = -1, src_last = -1;
    static uint32_t ib_gen_applied = 0xffffffffu;
    static emu_pfx local;
    if (!g_dec || !p_set_postfx) return;
    double now = mono_ms();
    int local_changed = 0;
    if (force || now - last_check >= 1000.0) {
        last_check = now;
        static char last[512];
        const char *home = getenv("HOME");
        char path[512];
        snprintf(path, sizeof(path), "%s/.local/lib/slp/postfx.txt", home ? home : "/home/steamos");
        char buf[512];
        int n = -1;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { n = (int)read(fd, buf, sizeof(buf) - 1); close(fd); }
        int has = n >= 0;
        if (n < 0) n = 0;
        buf[n] = 0;
        if (has != had || strcmp(buf, last)) {
            local_changed = 1;
            had = has;
            memcpy(last, buf, (size_t)n + 1);
            emu_pfx v = {0.0f, 0.0f, 0.0f, 0.35f, 0.8f, 2.0f, 0.0f, 2.0f, 1.0f, has, 0.0f, 1.0f};
            for (char *line = buf; line && *line; ) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                char *hash = strchr(line, '#');
                if (hash) *hash = 0;
                char *eq = strchr(line, '=');
                if (eq) {
                    char *k = line;
                    while (*k == ' ' || *k == '\t') k++;
                    char *ke = eq;
                    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r')) ke--;
                    *ke = 0;
                    float f = strtof(eq + 1, NULL);
                    if (!strcmp(k, "deblock") || !strcmp(k, "deblock_wide")) v.deblock = f;
                    else if (!strcmp(k, "deblock_inset")) v.deblock_inset = f;
                    else if (!strcmp(k, "wide_step")) v.wide_step = f;
                    else if (!strcmp(k, "deblock_mode")) v.mode = f;
                    else if (!strcmp(k, "deblock_coarse")) v.coarse = f;
                    else if (!strcmp(k, "deblock_bpp")) v.bpp = f;
                    else if (!strcmp(k, "sharpen")) v.sharpen = f;
                    else if (!strcmp(k, "sharpen_r0")) v.r0 = f;
                    else if (!strcmp(k, "sharpen_r1")) v.r1 = f;
                    else if (!strcmp(k, "wide_blur")) v.blur = f;
                    else if (!strcmp(k, "wide_blur_contrast")) v.blur_contrast = f;
                }
                line = nl ? nl + 1 : NULL;
            }
            local = v;
        }
    }
    // source: 2 = local file, 1 = from the PC, 0 = none (off)
    int src = had == 1 ? 2 : g_ib.have ? 1 : 0;
    int changed = force || src != src_last || (src == 2 && local_changed) || (src == 1 && g_ib.gen != ib_gen_applied);
    if (!changed) return;
    src_last = src;
    if (src == 1) ib_gen_applied = g_ib.gen;
    emu_pfx v = src == 2 ? local : src == 1 ? g_ib.v : (emu_pfx){0};
    if (src == 1 && !g_ib.v.on) { v.deblock = v.deblock_inset = v.sharpen = v.blur = 0.0f; }
    if (p_set_postfx_blur) p_set_postfx_blur(g_dec, v.blur, v.blur_contrast);
    if (p_set_postfx_mode) p_set_postfx_mode(g_dec, v.mode > 0.5f ? 1 : 0);
    if (p_set_postfx_seam) p_set_postfx_seam(g_dec, (int)(v.coarse + 0.5f), v.bpp > 0.5f);
    int ok = p_set_postfx2 ? p_set_postfx2(g_dec, v.deblock, v.deblock_inset, v.sharpen, v.r0, v.r1, v.wide_step)
                           : p_set_postfx(g_dec, v.deblock, v.sharpen, v.r0, v.r1);
    slog("EMU: post-filter %s: %s deblock wide %.2f, inset %.2f (%s); inset sharpen %.2f (full to %.2f, off at %.2f); "
         "wide blur %.2f px (contrast %.2f)%s",
         src == 2 ? "(local postfx.txt overrides the PC)" : src == 1 ? (g_ib.v.on ? "(from the PC)" : "off (PC setting)")
                                                         : "off (no settings from the PC yet)",
         v.mode > 0.5f ? (v.wide_step > 1.5f ? "smooth (reach 8 px)" : "smooth (reach 4 px)")
                       : (v.coarse > 1.5f ? "seam 16+32+64 px" : v.coarse > 0.5f ? "seam 16+32 px" : "seam 16 px"),
         v.deblock, v.deblock_inset, v.bpp > 0.5f ? "follows bits/pixel" : "fixed strength", v.sharpen, v.r0, v.r1,
         v.blur, v.blur_contrast, ok ? "" : " -- NOT available in this output mode (needs MODE 0, 1 or 4)");
}

// Creates the Pyrowave decoder for the stream size (from the first SLPW frame).
// ---- v7.19 full colour: vrlink side of the surface handover ----
// vrcompositor (with this layer, FULLCOLOUR armed) asks over a private unix socket, per stream surface it imports,
// for the shared 10-bit RGBA image that belongs to that surface. Pairing is by dma-buf inode: both processes hold
// the same buffer. Abstract socket "slp-fullcolour-<uid>" (no file); only this user's vrcompositor is answered.
//   GET  inode -> status + layout, image fd by SCM_RIGHTS; the surface is then written both ways (NV12 + RGBA)
//   ACK  inode -> vrcompositor samples the RGBA image now: RGBA only
//   NACK inode -> it does not (failed, or the surface is gone): NV12 only
#define FC_MAGIC 0x46504c53u   /* "SLPF" */
typedef struct { uint32_t magic, op; uint64_t ino; } fc_req;
typedef struct { uint32_t magic; int32_t status; uint32_t width, height, vk_format, pad;
                 uint64_t modifier, offset, row_pitch, size; } fc_rep;
enum { FC_GET = 1, FC_ACK = 2, FC_NACK = 3 };
enum { FC_OK = 0, FC_UNKNOWN = -1, FC_NOT_READY = -2, FC_EXPORT_FAILED = -3, FC_OFF = -4 };

static void fc_sock_name(struct sockaddr_un *a, socklen_t *len)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    int n = snprintf(a->sun_path + 1, sizeof(a->sun_path) - 1, "slp-fullcolour-%u", (unsigned)getuid());
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
}

// fstat() without the GLIBC_2.33 symbol (the layer stays loadable on glibc 2.17): the kernel's struct stat starts
// with st_dev, st_ino (64-bit each) on aarch64 and x86_64.
static uint64_t fd_inode(int fd)
{
    unsigned long long st[32];
    memset(st, 0, sizeof(st));
    if (fd < 0 || syscall(SYS_fstat, fd, st) != 0) return 0;
    return st[1];
}

static int fc_peer_is_compositor(int c)
{
    struct ucred cr;
    socklen_t l = sizeof(cr);
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0 || cr.uid != getuid()) return 0;
    char p[64], exe[512];
    snprintf(p, sizeof(p), "/proc/%d/exe", (int)cr.pid);
    ssize_t n = readlink(p, exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *leaf = strrchr(exe, '/');
    leaf = leaf ? leaf + 1 : exe;
    return strcmp(leaf, "vrcompositor") == 0 || (getenv("SLP_FC_TEST") && strcmp(leaf, "slp_fc_test") == 0);
}

// CAPTURE index of a surface inode (caller holds g_emu.mu), or -1.
static int fc_find_locked(uint64_t ino)
{
    if (!ino) return -1;
    for (int i = 0; i < EMU_NBUF; ++i)
        if (g_emu.cap_fd[i] >= 0 && g_emu.cap_ino[i] == ino) return i;
    return -1;
}

// v7.19b: vrlink hands each surface to vrcompositor (which imports it, and asks us) BEFORE it queues the surface
// into the decoder, so surfaces are registered when vrlink exports them (vkGetMemoryFdKHR) and imported right away.
// A later QBUF of the same dma-buf takes the already imported target over ("claimed"). Under g_emu.mu.
#define FC_PRE 32
static struct { uint64_t ino; int fd, target, claimed; } g_fc_pre[FC_PRE];
static int fc_pre_find_locked(uint64_t ino)
{
    if (!ino) return -1;
    for (int i = 0; i < FC_PRE; ++i) if (g_fc_pre[i].ino == ino) return i;
    return -1;
}
// Called from the vkGetMemoryFdKHR hook (vrlink's own export of a stream surface).
static void fc_note_export(int fd)
{
    uint64_t ino = fd_inode(fd);
    if (!ino) return;
    pthread_mutex_lock(&g_emu.mu);
    if (g_fc) {
        int k = fc_pre_find_locked(ino);
        for (int i = 0; k < 0 && i < FC_PRE; ++i) if (!g_fc_pre[i].ino) k = i;
        if (k >= 0 && g_fc_pre[k].ino != ino) {
            g_fc_pre[k].ino = ino; g_fc_pre[k].fd = fd; g_fc_pre[k].target = -1; g_fc_pre[k].claimed = 0;
            g_emu.imp_pending = 1;
            pthread_cond_broadcast(&g_emu.cv);
        }
    }
    pthread_mutex_unlock(&g_emu.mu);
}
// Drops every unclaimed pre-imported surface (stream end / new CAPTURE buffers). Caller holds g_emu.mu.
static void fc_pre_clear_locked(void)
{
    for (int i = 0; i < FC_PRE; ++i) {
        int t = (g_fc_pre[i].ino && !g_fc_pre[i].claimed) ? g_fc_pre[i].target : -1;
        memset(&g_fc_pre[i], 0, sizeof(g_fc_pre[i]));
        g_fc_pre[i].fd = g_fc_pre[i].target = -1;
        if (t >= 0 && g_dec) {
            pthread_mutex_unlock(&g_emu.mu);
            p_release(g_dec, t);
            pthread_mutex_lock(&g_emu.mu);
        }
    }
}
// Target of a surface inode, from the CAPTURE table or the pre-import table. *known: the surface exists (maybe
// still being imported), *ci: CAPTURE index or -1.
static int fc_lookup_locked(uint64_t ino, int *known, int *ci)
{
    *ci = fc_find_locked(ino);
    if (*ci >= 0 && g_emu.cap_target[*ci] >= 0) { *known = 1; return g_emu.cap_target[*ci]; }
    int k = fc_pre_find_locked(ino);
    *known = *ci >= 0 || k >= 0;
    return (k >= 0 && !g_fc_pre[k].claimed) ? g_fc_pre[k].target : -1;
}

static int fc_send(int c, const fc_rep *r, int fd)
{
    struct iovec iov = {(void *)r, sizeof(*r)};
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u;
    if (fd >= 0) {
        memset(&u, 0, sizeof(u));
        mh.msg_control = u.buf;
        mh.msg_controllen = sizeof(u.buf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return sendmsg(c, &mh, MSG_NOSIGNAL) == (ssize_t)sizeof(*r) ? 0 : -1;
}

// Waits (bounded) until the surface is known and imported by the worker. Caller holds g_emu.mu. wait_ms bounds the
// wait for an unknown surface; a known one being imported gets up to 500 ms (the import burst at stream start).
static int fc_wait_target_locked(uint64_t ino, int *ci_out, int *known_out, double wait_ms)
{
    const double t0 = mono_ms();
    for (;;) {
        int ci, known;
        int t = fc_lookup_locked(ino, &known, &ci);
        *ci_out = ci;
        *known_out = known;
        if (t >= 0) return t;
        if (known && !g_emu.imp_pending) { g_emu.imp_pending = 1; pthread_cond_broadcast(&g_emu.cv); }
        double left = t0 + (known ? (wait_ms > 500.0 ? wait_ms : 500.0) : wait_ms) - mono_ms();
        if (left <= 0) return -1;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        long ns = (long)(left * 1e6);
        if (ns > 20 * 1000000L) ns = 20 * 1000000L;
        dl.tv_nsec += ns;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_fc_cv, &g_emu.mu, &dl);
    }
}

#define FC_MAXCL 4
static void *fc_server(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "slp-fullcolour");   // v7.22: named threads (top -H)
    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    socklen_t al;
    fc_sock_name(&a, &al);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, al) != 0 || listen(ls, FC_MAXCL) != 0) {
        slog("FULLCOLOUR: socket setup failed (%s); vrcompositor will keep the NV12 surfaces", strerror(errno));
        if (ls >= 0) close(ls);
        return NULL;
    }
    slog("FULLCOLOUR: handover socket ready");
    int cl[FC_MAXCL];
    uint64_t both[FC_MAXCL];   // per client: targets it put into "both" (GET without ACK yet)
    for (int i = 0; i < FC_MAXCL; ++i) cl[i] = -1, both[i] = 0;
    int unknown_seen = 0;
    for (;;) {
        struct pollfd pf[1 + FC_MAXCL];
        int map[1 + FC_MAXCL], n = 0;
        pf[n].fd = ls; pf[n].events = POLLIN; pf[n].revents = 0; map[n++] = -1;
        for (int i = 0; i < FC_MAXCL; ++i)
            if (cl[i] >= 0) { pf[n].fd = cl[i]; pf[n].events = POLLIN; pf[n].revents = 0; map[n++] = i; }
        if (poll(pf, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int k = 0; k < n; ++k) {
            if (!pf[k].revents) continue;
            if (map[k] < 0) {
                int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
                if (c < 0) continue;
                int slot = -1;
                for (int i = 0; i < FC_MAXCL; ++i) if (cl[i] < 0) { slot = i; break; }
                if (slot < 0 || !fc_peer_is_compositor(c)) {
                    slog("FULLCOLOUR: refused a connection (%s)", slot < 0 ? "too many clients" : "not this user's vrcompositor");
                    close(c);
                    continue;
                }
                cl[slot] = c;
                both[slot] = 0;
                slog("FULLCOLOUR: vrcompositor connected");
                continue;
            }
            int s = map[k], c = cl[s];
            fc_req q;
            ssize_t r = recv(c, &q, sizeof(q), 0);
            if (r != (ssize_t)sizeof(q) || q.magic != FC_MAGIC) {
                // gone (or garbage): every surface it had not confirmed goes back to NV12
                pthread_mutex_lock(&g_fc_mu);
                for (int t = 0; t < 64; ++t)
                    if ((both[s] >> t) & 1) {
                        if (g_dec && g_fc && p_target_output(g_dec, t) == 2) p_set_target_output(g_dec, t, 0);
                    }
                pthread_mutex_unlock(&g_fc_mu);
                slog("FULLCOLOUR: vrcompositor disconnected%s", r == 0 ? "" : " (bad request)");
                close(c);
                cl[s] = -1;
                continue;
            }
            fc_rep rep;
            memset(&rep, 0, sizeof(rep));
            rep.magic = FC_MAGIC;
            int give_fd = -1, ci = -1, target = -1;
            pthread_mutex_lock(&g_fc_mu);
            pthread_mutex_lock(&g_emu.mu);
            if (!g_dec || !g_fc) rep.status = FC_OFF;
            else if (q.op == FC_GET) {
                // the first unknown surface may just be early (vrlink queues it right after sharing it): wait a
                // little; later misses answer at once so vrcompositor never stalls for long
                int known = 0;
                target = fc_wait_target_locked(q.ino, &ci, &known, unknown_seen ? 20.0 : 250.0);
                if (target < 0) { rep.status = known ? FC_NOT_READY : FC_UNKNOWN; unknown_seen |= !known; }
            } else {
                int known = 0;
                target = fc_lookup_locked(q.ino, &known, &ci);
                rep.status = target >= 0 ? FC_OK : FC_UNKNOWN;
            }
            pthread_mutex_unlock(&g_emu.mu);
            if (rep.status == FC_OK && q.op == FC_GET) {
                slpd_rgba_l e;
                char err[256] = {0};
                memset(&e, 0, sizeof(e));
                if (p_rgba_export(g_dec, target, &e, err, sizeof(err)) != 0) {
                    rep.status = FC_EXPORT_FAILED;
                    slog("FULLCOLOUR: no shared image for CAPTURE %d: %s", ci, err);
                } else {
                    give_fd = e.fd;
                    rep.width = e.width; rep.height = e.height; rep.vk_format = e.vk_format;
                    rep.modifier = e.modifier; rep.offset = e.offset; rep.row_pitch = e.row_pitch; rep.size = e.size;
                    static uint64_t logged_mod = ~0ull;   // v7.21
                    if (e.modifier != logged_mod) {
                        logged_mod = e.modifier;
                        slog("FULLCOLOUR: shared images are %s (modifier 0x%llx, row pitch %llu, %llu bytes)",
                             e.modifier == 0 ? "LINEAR" : "compressed (UBWC)", (unsigned long long)e.modifier,
                             (unsigned long long)e.row_pitch, (unsigned long long)e.size);
                    }
                    p_set_target_output(g_dec, target, 2);   // both until vrcompositor confirms
                    if (target < 64) both[s] |= 1ull << target;
                }
            } else if (rep.status == FC_OK && q.op == FC_ACK) {
                p_set_target_output(g_dec, target, 1);
                if (target < 64) both[s] &= ~(1ull << target);
            } else if (rep.status == FC_OK && q.op == FC_NACK) {
                p_set_target_output(g_dec, target, 0);
                if (target < 64) both[s] &= ~(1ull << target);
            }
            pthread_mutex_unlock(&g_fc_mu);
            if (fc_send(c, &rep, give_fd) != 0) slog("FULLCOLOUR: reply failed (%s)", strerror(errno));
            if (give_fd >= 0) close(give_fd);
            static int logged;
            if (logged++ < 96)
                slog("FULLCOLOUR: %s for CAPTURE %d (target %d) -> %s", q.op == FC_GET ? "GET" : q.op == FC_ACK ? "ACK" : "NACK", ci,
                     target, rep.status == FC_OK ? (q.op == FC_GET ? "shared image sent, writing NV12 + RGBA"
                                                    : q.op == FC_ACK ? "vrcompositor samples it: RGBA only" : "NV12 only")
                     : rep.status == FC_UNKNOWN ? "unknown surface" : rep.status == FC_NOT_READY ? "not imported yet"
                     : rep.status == FC_OFF ? "full colour off" : "export failed");
        }
    }
    close(ls);
    return NULL;
}

static void fc_server_start(void)
{
    static int started;
    if (started) return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    started = pthread_create(&t, &at, fc_server, NULL) == 0;
    pthread_attr_destroy(&at);
    if (!started) slog("FULLCOLOUR: could not start the handover thread");
}

static int emu_create_decoder(uint32_t w, uint32_t h)
{
    if (g_dec && g_dec_w == w && g_dec_h == h) return 1;
    if (g_dec) {
        // New session at another size (all surfaces of the old session were released in emu_close).
        if (!p_destroy) return 0;
        slog("EMU: stream size changed %ux%u -> %ux%u: recreating the decoder", g_dec_w, g_dec_h, w, h);
        pthread_mutex_lock(&g_fc_mu);   // v7.19: the full-colour socket thread uses g_dec
        p_destroy(g_dec);
        g_dec = NULL;
        g_fc = 0;
        pthread_mutex_unlock(&g_fc_mu);
    }
    if (g_dec_failed || !emu_load_library()) return 0;
    char msg[256] = {0};
    double t0 = mono_ms();
    g_internal = 1;          // our own vkCreateInstance must not load this layer again
    if (p_set_rgba_compressed) p_set_rgba_compressed(g_rgba_compressed && g_out_mode == 6);
    void *nd = p_create2 ? p_create2((int)w, (int)h, EMU_BITS, g_out_mode, msg, sizeof(msg))
                         : p_create((int)w, (int)h, EMU_BITS, msg, sizeof(msg));
    if (!nd && g_out_mode == 6) {
        // v7.19: full colour failed to initialise: 4:2:0 as before
        slog("EMU: full-colour decoder init failed (%s); falling back to the 4:2:0 decoder", msg);
        g_out_mode = g_mode_420;
        g_fc_armed = 0;
        nd = p_create2((int)w, (int)h, EMU_BITS, g_out_mode, msg, sizeof(msg));
    }
    g_internal = 0;
    if (!nd) { slog("EMU: Pyrowave decoder init failed: %s", msg); g_dec_failed = 1; return 0; }
    pthread_mutex_lock(&g_fc_mu);
    g_dec = nd;
    g_fc = g_fc_armed && p_full_colour && p_full_colour(g_dec) == 1;
    pthread_mutex_unlock(&g_fc_mu);
    if (g_fc_armed && !g_fc) slog("EMU: FULL COLOUR armed, but the decoder did not enable it; 4:2:0 only");
    if (g_fc) fc_server_start();
    if (g_no_dither && p_set_dither) p_set_dither(g_dec, 0);
    if (p_set_merge) p_set_merge(g_dec, !g_no_merge);
    if (p_set_fast_pyramid) p_set_fast_pyramid(g_dec, !g_exact_pyramid);
    if (p_set_merge)
        slog("EMU: v7.21 power: %s, %s blur pyramid, %s full-colour images", g_no_merge ? "separate filter passes (NO_MERGE)"
             : "blur + sharpening folded into the full-colour pack", g_exact_pyramid ? "exact (EXACT_PYRAMID)" : "fast",
             g_out_mode != 6 ? "no" : g_rgba_compressed ? "compressed (UBWC) where the driver allows," : "LINEAR (RGBA_LINEAR)");
    g_dec_w = w; g_dec_h = h;
    emu_postfx_poll(1);
    slog("EMU: Pyrowave decoder %ux%u ready on %s in %.0f ms (%d-bit NV12 client surfaces, output mode %d, dither %s, %s)", w, h,
         msg, mono_ms() - t0, EMU_BITS, g_out_mode, g_no_dither && p_set_dither ? "off" : "on",
         p_submit ? "pipelined: 2 frames in flight" : "one frame at a time");
    if (g_fc) slog("EMU: FULL COLOUR on: 4:2:0 and 4:4:4 frames; each surface gets a shared 10-bit image once vrcompositor asks for it");
    return 1;
}

// ---- SLPW header (see frame/decoder/slp_pyro_dec.hpp) ----
static int emu_frame_info(const uint8_t *p, uint32_t n, uint32_t *w, uint32_t *h, uint32_t *npk)
{
    if (n < 32) return -1;
    uint32_t hdr[8];
    memcpy(hdr, p, 32);
    if (hdr[0] != 0x57504c53u || (hdr[1] & 0xff) != 1 || (hdr[1] >> 16) != 32) return -1;
    *w = hdr[2]; *h = hdr[3]; *npk = hdr[5];
    return 0;
}

static struct {
    double win;
    uint32_t frames, repeats, dropped_nocap, dropped_early, errors, bad;
    double cpu_sum, cpu_max, dec_sum, pc_sum, lat_sum, lat_max, fin_sum, cp_sum;
    uint32_t direct;
    uint32_t rgba, both, c444;   // v7.19 full colour: frames written RGBA only / both ways; 4:4:4 frames
} g_es;

static void emu_stats_roll(double now)
{
    if (g_es.win == 0) g_es.win = now;
    if (now - g_es.win < 1000.0) return;
    uint32_t n = g_es.frames + g_es.repeats;
    if (n || g_es.dropped_nocap || g_es.errors || g_es.bad)
        slog("[EMU] 1s: %u decoded + %u repeat | drop: %u no-CAP, %u before CAP | %u errors, %u bad | decode cpu avg %.2f max %.2f ms "
             "(GPU decode %.2f, pack+copy %.2f = finish %.2f + copy %.2f, %u direct-written) | OUT QBUF->CAP ready avg %.2f max %.2f ms",
             g_es.frames, g_es.repeats, g_es.dropped_nocap, g_es.dropped_early, g_es.errors, g_es.bad,
             n ? g_es.cpu_sum / n : 0, g_es.cpu_max, g_es.frames ? g_es.dec_sum / g_es.frames : 0,
             n ? g_es.pc_sum / n : 0, n ? g_es.fin_sum / n : 0, n ? g_es.cp_sum / n : 0, g_es.direct,
             n ? g_es.lat_sum / n : 0, g_es.lat_max);
    if (g_fc && n)
        slog("[EMU] 1s full colour: %u 4:4:4 frame(s); surfaces written RGBA only %u, NV12 + RGBA %u, NV12 only %u", g_es.c444,
             g_es.rgba, g_es.both, n > g_es.rgba + g_es.both ? n - g_es.rgba - g_es.both : 0);
    if (n && p_pass_report && g_dec) {
        // v7.21: where the GPU time of each frame goes (averages over this second)
        double ps[6], tot = 0;
        int pn = p_pass_report(g_dec, ps, &tot);
        if (pn > 0)
            slog("[EMU] 1s GPU ms/frame: total %.3f = decode %.3f + finish %.3f + seam %.3f + pyramid %.3f + blur %.3f + "
                 "sharpen/deblock %.3f + pack/copies %.3f", tot, tot - (ps[0] + ps[1] + ps[2] + ps[3] + ps[4] + ps[5]), ps[0], ps[1],
                 ps[2], ps[3], ps[4], ps[5]);
    }
    memset(&g_es, 0, sizeof(g_es));
    g_es.win = now;
}

// Hands a decoded CAPTURE buffer to vrlink and updates the stats. Caller holds g_emu.mu.
static void emu_publish_locked(const emu_item *job, int ci, int rc, uint32_t npk, const slpd_timing2_l *t)
{
    if (!g_emu.cap_streaming) return;     // flushed while decoding: buffer is vrlink's again
    emu_item cd = *job;
    cd.idx = (uint32_t)ci;
    cd.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY | (rc == 0 ? 0 : V4L2_BUF_FLAG_ERROR);
    cd.bytes = rc == 0 ? g_emu.cap_sizeimage : 0;
    ring_push(&g_emu.cap_done, cd);
    emu_update_ready_locked();
    double done = mono_ms();
    if (rc == 0) {
        if (npk) { g_es.frames++; g_es.dec_sum += t->gpu_decode_ms; } else g_es.repeats++;
        g_es.cpu_sum += t->cpu_ms;
        if (t->cpu_ms > g_es.cpu_max) g_es.cpu_max = t->cpu_ms;
        g_es.pc_sum += t->gpu_pack_copy_ms;
        g_es.fin_sum += t->gpu_finish_ms;
        g_es.cp_sum += t->gpu_copy_ms;
        g_es.direct += t->direct ? 1 : 0;
        double l = done - job->t_q;
        g_es.lat_sum += l;
        if (l > g_es.lat_max) g_es.lat_max = l;
    } else g_es.errors++;
}

// v7.3 pipelining: the worker records + submits frame N+1 while the GPU still runs frame N; this thread waits
// for each frame (in order) and publishes it the moment its GPU work is done.
typedef struct { emu_item job; int ci, ticket; uint32_t npk; } emu_inflight;
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    emu_inflight q[8];
    unsigned head, n;
    int stop;
} g_infl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *emu_completer(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "slp-complete");
    for (;;) {
        pthread_mutex_lock(&g_infl.mu);
        while (!g_infl.stop && g_infl.n == 0) pthread_cond_wait(&g_infl.cv, &g_infl.mu);
        if (g_infl.n == 0) { pthread_mutex_unlock(&g_infl.mu); break; }   // stop and drained
        emu_inflight it = g_infl.q[g_infl.head];
        g_infl.head = (g_infl.head + 1) % 8;
        g_infl.n--;
        pthread_cond_broadcast(&g_infl.cv);
        pthread_mutex_unlock(&g_infl.mu);
        slpd_timing2_l t = {0};
        char err[256] = {0};
        int rc = it.ticket >= 0 ? p_wait(g_dec, it.ticket, &t, err, sizeof(err)) : -1;
        if (rc != 0 && it.ticket >= 0) {
            static int logged_w;
            if (logged_w++ < 5) slog("EMU: decode failed: %s", err);
        }
        pthread_mutex_lock(&g_emu.mu);
        emu_publish_locked(&it.job, it.ci, rc, it.npk, &t);
        pthread_mutex_unlock(&g_emu.mu);
    }
    return NULL;
}

static void emu_infl_push(const emu_inflight *it)
{
    pthread_mutex_lock(&g_infl.mu);
    while (g_infl.n == 8) pthread_cond_wait(&g_infl.cv, &g_infl.mu);   // never reached: submit blocks at 2
    g_infl.q[(g_infl.head + g_infl.n) % 8] = *it;
    g_infl.n++;
    pthread_cond_broadcast(&g_infl.cv);
    pthread_mutex_unlock(&g_infl.mu);
}

static void *emu_worker(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "slp-decode");
    emu_load_library();     // the decoder itself is created for the first frame's size
    uint8_t *buf = NULL;
    size_t cap = 0;
    pthread_t completer;
    int completer_running = 0;
    if (p_submit) {
        pthread_mutex_lock(&g_infl.mu);
        g_infl.stop = 0; g_infl.head = 0; g_infl.n = 0;
        pthread_mutex_unlock(&g_infl.mu);
        completer_running = pthread_create(&completer, NULL, emu_completer, NULL) == 0;
    }
    pthread_mutex_lock(&g_emu.mu);
    for (;;) {
        while (!g_emu.stop && g_emu.jobs.n == 0 && !g_emu.imp_pending) pthread_cond_wait(&g_emu.cv, &g_emu.mu);
        if (g_emu.stop) break;
        if (g_emu.imp_pending) {
            // v7.19 full colour: import every queued surface now, so its shared image exists when vrcompositor asks
            g_emu.imp_pending = 0;
            for (int i = 0; i < EMU_NBUF && g_dec && g_fc; ++i) {
                if (g_emu.cap_fd[i] < 0 || g_emu.cap_target[i] >= 0 || !g_emu.cap_fmt_ready) continue;
                int fd = g_emu.cap_fd[i];
                pthread_mutex_unlock(&g_emu.mu);
                char ierr[256] = {0};
                int t = p_import(g_dec, fd, g_cl_mod, g_cl_off, g_cl_pitch, g_emu.cap_sizeimage, ierr, sizeof(ierr));
                pthread_mutex_lock(&g_emu.mu);
                if (t < 0) { slog("EMU: import of CAPTURE %d (dma-buf fd %d) failed: %s", i, fd, ierr); continue; }
                if (g_emu.cap_fd[i] == fd && g_emu.cap_target[i] < 0) {
                    g_emu.cap_target[i] = t;
                    char as[320] = "";
                    if (p_alias_status) p_alias_status(g_dec, as, sizeof(as));
                    slog("EMU: imported CAPTURE %d early (full colour; modifier 0x%llx%s): %s", i, (unsigned long long)g_cl_mod,
                         g_cl_seen ? "" : ", default layout", as);
                } else {
                    pthread_mutex_unlock(&g_emu.mu);
                    p_release(g_dec, t);   // the surface changed meanwhile
                    pthread_mutex_lock(&g_emu.mu);
                }
            }
            // v7.19b: surfaces vrlink exported but has not queued yet (vrcompositor asks for them first)
            for (int k = 0; k < FC_PRE && g_dec && g_fc && g_emu.cap_fmt_ready; ++k) {
                if (!g_fc_pre[k].ino || g_fc_pre[k].claimed || g_fc_pre[k].target >= 0 || g_fc_pre[k].fd < 0) continue;
                const uint64_t ino = g_fc_pre[k].ino;
                const int fd = g_fc_pre[k].fd;
                if (fd_inode(fd) != ino) { g_fc_pre[k].fd = -1; continue; }   // vrlink closed / reused that fd
                pthread_mutex_unlock(&g_emu.mu);
                char ierr[256] = {0};
                int t = p_import(g_dec, fd, g_cl_mod, g_cl_off, g_cl_pitch, g_emu.cap_sizeimage, ierr, sizeof(ierr));
                pthread_mutex_lock(&g_emu.mu);
                if (t < 0) { slog("EMU: early import of a surface (dma-buf fd %d) failed: %s", fd, ierr); g_fc_pre[k].fd = -1; continue; }
                int kk = fc_pre_find_locked(ino), ci = fc_find_locked(ino);
                if (kk >= 0 && !g_fc_pre[kk].claimed && g_fc_pre[kk].target < 0) {
                    g_fc_pre[kk].target = t;
                    static int logged;
                    if (logged++ < 32) slog("EMU: surface exported by vrlink (dma-buf fd %d) imported early for full colour (target %d)", fd, t);
                } else if (ci >= 0 && g_emu.cap_target[ci] < 0) {
                    g_emu.cap_target[ci] = t;   // queued meanwhile: hand it to that CAPTURE buffer
                    if (kk >= 0) g_fc_pre[kk].claimed = 1;
                } else {
                    pthread_mutex_unlock(&g_emu.mu);
                    p_release(g_dec, t);
                    pthread_mutex_lock(&g_emu.mu);
                }
            }
            pthread_cond_broadcast(&g_fc_cv);
            if (g_emu.jobs.n == 0) continue;
        }
        emu_item job;
        ring_pop(&g_emu.jobs, &job);
        // Copy the frame out and hand the OUTPUT buffer straight back (vrlink can refill it).
        uint32_t n = job.bytes;
        int ok_copy = job.idx < EMU_NBUF && g_emu.out_addr[job.idx] && n <= g_emu.out_len[job.idx];
        if (ok_copy) {
            if (cap < n) { free(buf); cap = n + 65536; buf = malloc(cap); }
            if (buf) memcpy(buf, g_emu.out_addr[job.idx], n);
            else ok_copy = 0;
        }
        emu_item od = job;
        od.flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_COPY;
        od.bytes = 0;
        ring_push(&g_emu.out_done, od);
        emu_update_ready_locked();

        uint32_t fw = 0, fh = 0, npk = 0;
        double now = mono_ms();
        emu_stats_roll(now);
        if (ok_copy && n >= 32) emu_inband_parse(buf, n);   // v7.11: settings from the PC
        if (!ok_copy || emu_frame_info(buf, n, &fw, &fh, &npk) != 0) {
            g_es.bad++;
            static int logged;
            if (logged++ < 5)
                slog("EMU: OUTPUT buffer %u (%u bytes) is not an SLPW frame%s - is the PC in substitution mode?", job.idx, n,
                     ok_copy && n >= 4 ? "" : " (copy failed)");
            continue;
        }
        if (!g_emu.cap_fmt_ready) {
            if (npk == 0 || fw == 0 || fh == 0) { g_es.dropped_early++; continue; }
            // Decoder + surface format for this stream size (any size since v7.4).
            pthread_mutex_unlock(&g_emu.mu);
            int dec_ok = emu_create_decoder(fw, fh);
            pthread_mutex_lock(&g_emu.mu);
            if (!dec_ok) {
                g_es.errors++;
                static int logged_dc;
                if (logged_dc++ < 3) slog("EMU: no decoder for %ux%u; frames dropped", fw, fh);
                continue;
            }
            uint64_t off1 = 0;
            g_emu.cap_w = fw;
            g_emu.cap_h = fh;
            emu_nv12_ubwc_layout(fw, fh, &g_emu.cap_bpl, &off1, &g_emu.cap_sizeimage);
            g_cl_off[0] = 0; g_cl_off[1] = off1;
            g_cl_pitch[0] = g_cl_pitch[1] = g_emu.cap_bpl;
            g_cl_seen = 0;
            g_cl_want_w = fw;
            g_emu.cap_fmt_ready = 1;
            if (g_emu.subscribed) { g_emu.ev_pending = 1; emu_update_ready_locked(); }
            slog("EMU: first frame %ux%u -> SOURCE_CHANGE (NV12 UBWC bpl %u, plane 1 at %llu, %u bytes)", fw, fh,
                 g_emu.cap_bpl, (unsigned long long)off1, g_emu.cap_sizeimage);
            g_es.dropped_early++;
            continue;
        }
        if (!g_emu.cap_streaming || !g_dec) { g_es.dropped_early++; continue; }
        if (npk && (fw != g_dec_w || fh != g_dec_h)) {
            g_es.errors++;
            static int logged_sz;
            if (logged_sz++ < 3)
                slog("EMU: frame is %ux%u but this session decodes %ux%u (the PC changed size mid-stream); dropped - "
                     "restart the stream", fw, fh, g_dec_w, g_dec_h);
            continue;
        }
        // Oldest queued CAPTURE buffer; wait briefly if vrlink has none queued right now.
        int ci = -1;
        for (int tries = 0; tries < 2 && ci < 0; ++tries) {
            uint64_t best = UINT64_MAX;
            for (uint32_t i = 0; i < g_emu.cap_count && i < EMU_NBUF; ++i)
                if (g_emu.cap_queued[i] && g_emu.cap_qseq[i] < best) { best = g_emu.cap_qseq[i]; ci = (int)i; }
            if (ci < 0 && tries == 0) {
                struct timespec dl;
                clock_gettime(CLOCK_REALTIME, &dl);
                dl.tv_nsec += 30 * 1000000L;
                if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&g_emu.cv, &g_emu.mu, &dl);
                if (g_emu.stop || !g_emu.cap_streaming) break;
            }
        }
        if (ci < 0) { g_es.dropped_nocap++; continue; }
        g_emu.cap_queued[ci] = 0;
        int fd = g_emu.cap_fd[ci];
        int target = g_emu.cap_target[ci];
        pthread_mutex_unlock(&g_emu.mu);

        char err[256] = {0};
        if (target < 0) {
            target = p_import(g_dec, fd, g_cl_mod, g_cl_off, g_cl_pitch, g_emu.cap_sizeimage, err, sizeof(err));
            if (target < 0) slog("EMU: import of CAPTURE %d (dma-buf fd %d) failed: %s", ci, fd, err);
            else {
                char as[320] = "";
                if (p_alias_status) p_alias_status(g_dec, as, sizeof(as));
                slog("EMU: imported CAPTURE %d (dma-buf fd %d, modifier 0x%llx%s)%s%s", ci, fd, (unsigned long long)g_cl_mod,
                     g_cl_seen ? "" : ", default layout", as[0] ? ": " : "", as);
            }
        }
        emu_postfx_poll(0);
        if (g_fc && target >= 0) {
            int o = p_target_output(g_dec, target);
            pthread_mutex_lock(&g_emu.mu);
            g_es.rgba += o == 1;
            g_es.both += o == 2;
            g_es.c444 += p_frame_chroma(buf, n) == 1;
            pthread_mutex_unlock(&g_emu.mu);
        } else if (target >= 0 && p_frame_chroma && p_frame_chroma(buf, n) == 1) {
            static int logged_444;
            if (logged_444++ < 3)
                slog("EMU: the PC sends 4:4:4 (full colour) but full colour is not armed on the headset: frames will fail. "
                     "Arm it (fullcolour_arm.sh) or turn Full colour off on the PC.");
        }
        if (completer_running) {
            // Pipelined: submit now, the completer waits and publishes.
            emu_inflight it;
            it.job = job;
            it.ci = ci;
            it.npk = npk;
            it.ticket = target >= 0 ? p_submit(g_dec, buf, n, target, err, sizeof(err)) : -1;
            if (it.ticket < 0 && target >= 0) {
                static int logged_s;
                if (logged_s++ < 5) slog("EMU: decode submit failed: %s", err);
            }
            pthread_mutex_lock(&g_emu.mu);
            if (ci < (int)EMU_NBUF) g_emu.cap_target[ci] = target;
            pthread_mutex_unlock(&g_emu.mu);
            emu_infl_push(&it);
            pthread_mutex_lock(&g_emu.mu);
            continue;
        }
        slpd_timing2_l t = {0};
        int rc = -1;
        if (target >= 0) {
            if (p_decode2) rc = p_decode2(g_dec, buf, n, target, &t, err, sizeof(err));
            else {
                slpd_timing_l t1 = {0};
                rc = p_decode(g_dec, buf, n, target, &t1, err, sizeof(err));
                t.cpu_ms = t1.cpu_ms; t.gpu_decode_ms = t1.gpu_decode_ms; t.gpu_pack_copy_ms = t1.gpu_pack_copy_ms;
                t.complete = t1.complete;
            }
        }
        if (rc != 0 && target >= 0) {
            static int logged_dec;
            if (logged_dec++ < 5) slog("EMU: decode failed: %s", err);
        }

        pthread_mutex_lock(&g_emu.mu);
        if (ci < (int)EMU_NBUF) g_emu.cap_target[ci] = target;
        emu_publish_locked(&job, ci, rc, npk, &t);
    }
    pthread_mutex_unlock(&g_emu.mu);
    if (completer_running) {
        // Let the completer wait for every submitted frame (GPU idle, slots free) before teardown.
        pthread_mutex_lock(&g_infl.mu);
        g_infl.stop = 1;
        pthread_cond_broadcast(&g_infl.cv);
        pthread_mutex_unlock(&g_infl.mu);
        pthread_join(completer, NULL);
    }
    free(buf);
    return NULL;
}

// ---- lifecycle ----
static void emu_cand_reset(int fd)
{
    memset(&g_cand, 0, sizeof(g_cand));
    g_cand.fd = fd;
    g_cand.epfd = -1;
    g_cand.sizeimage = 8847360;
}

// Called on the first QBUF OUTPUT of a candidate that carries an SLPW frame.  Returns 0 on success.
static int emu_takeover(int fd)
{
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) { slog("EMU: eventfd failed (%s); staying on the hardware", strerror(errno)); return -1; }
    if (g_cand.epfd >= 0) {
        // vrlink's epoll entry for the decoder now follows our eventfd (same data value).
        struct epoll_event e = {0};
        e.events = EPOLLIN;
        e.data.u64 = g_cand.epdata;
        real_epoll_ctl(g_cand.epfd, EPOLL_CTL_DEL, fd, NULL);
        if (real_epoll_ctl(g_cand.epfd, EPOLL_CTL_ADD, efd, &e) != 0) {
            slog("EMU: could not register eventfd in epoll %d (%s); staying on the hardware", g_cand.epfd, strerror(errno));
            struct epoll_event back = {0};
            back.events = EPOLLIN | EPOLLPRI | EPOLLOUT;
            back.data.u64 = g_cand.epdata;
            real_epoll_ctl(g_cand.epfd, EPOLL_CTL_ADD, fd, &back);
            real_close(efd);
            return -1;
        }
    }
    pthread_mutex_lock(&g_emu.mu);
    g_emu.fd = fd;
    g_emu.efd = efd;
    g_emu.efd_set = 0;
    g_emu.epfd = g_cand.epfd;
    g_emu.epdata = g_cand.epdata;
    g_emu.out_w = g_cand.out_w; g_emu.out_h = g_cand.out_h; g_emu.out_fourcc = g_cand.fourcc;
    g_emu.out_count = g_cand.out_count;
    g_emu.out_sizeimage = g_cand.sizeimage;
    for (int i = 0; i < EMU_NBUF; ++i) {
        g_emu.out_off[i] = g_cand.out_off[i];
        g_emu.out_addr[i] = NULL;
        g_emu.out_len[i] = 0;
        for (int k = 0; k < EMU_NBUF; ++k)
            if (g_cand.maps[k].addr && g_cand.maps[k].off == g_cand.out_off[i]) {
                g_emu.out_addr[i] = g_cand.maps[k].addr;
                g_emu.out_len[i] = g_cand.maps[k].len;
            }
    }
    g_emu.out_streaming = g_cand.out_streaming;
    memset(&g_emu.jobs, 0, sizeof(g_emu.jobs));
    memset(&g_emu.out_done, 0, sizeof(g_emu.out_done));
    memset(&g_emu.cap_done, 0, sizeof(g_emu.cap_done));
    g_emu.cap_w = 1344; g_emu.cap_h = 5376;
    g_emu.cap_count = 0; g_emu.cap_fmt_ready = 0; g_emu.cap_streaming = 0;
    g_emu.cap_bpl = 1408; g_emu.cap_sizeimage = 11423744;   // NV12 UBWC at 1344x5376 (as iris reports for Q08C)
    for (int i = 0; i < EMU_NBUF; ++i) { g_emu.cap_fd[i] = -1; g_emu.cap_target[i] = -1; g_emu.cap_queued[i] = 0; g_emu.cap_ino[i] = 0; }
    g_emu.imp_pending = 0;
    g_emu.subscribed = g_cand.subscribed;
    g_emu.ev_pending = 0;
    g_emu.stop = 0;
    g_emu.worker_running = pthread_create(&g_emu.worker, NULL, emu_worker, NULL) == 0;
    pthread_mutex_unlock(&g_emu.mu);
    int mapped = 0;
    for (int i = 0; i < (int)g_emu.out_count && i < EMU_NBUF; ++i) mapped += g_emu.out_addr[i] != NULL;
    slog("EMU: first OUTPUT frame is Pyrowave (SLPW) -> taking over decoder fd %d: %u OUTPUT buffers (%d mapped), epoll %d, "
         "readiness eventfd %d", fd, g_emu.out_count, mapped, g_emu.epfd, efd);
    g_cand.fd = -1;
    return 0;
}

// Candidate hooks (before the first frame): observe what the hardware was told.
static int emu_cand_qbuf_is_slpw(void *arg)
{
    struct v4l2_buffer *b = arg;
    if (!b || b->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE || !b->m.planes || b->length < 1 || b->index >= EMU_NBUF) return 0;
    uint64_t off = g_cand.out_off[b->index];
    for (int k = 0; k < EMU_NBUF; ++k)
        if (g_cand.maps[k].addr && g_cand.maps[k].off == off && g_cand.maps[k].len >= 4 && b->m.planes[0].bytesused >= 32) {
            uint32_t magic;
            memcpy(&magic, g_cand.maps[k].addr, 4);
            return magic == 0x57504c53u;
        }
    return 0;
}

static void emu_cand_after_ioctl(unsigned long req, void *arg, int ret)
{
    if (ret != 0 || !arg) return;
    if (req == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            g_cand.out_w = f->fmt.pix_mp.width; g_cand.out_h = f->fmt.pix_mp.height; g_cand.fourcc = f->fmt.pix_mp.pixelformat;
            if (f->fmt.pix_mp.plane_fmt[0].sizeimage) g_cand.sizeimage = f->fmt.pix_mp.plane_fmt[0].sizeimage;
        }
    } else if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) g_cand.out_count = r->count;
    } else if (req == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && b->m.planes && b->index < EMU_NBUF)
            g_cand.out_off[b->index] = b->m.planes[0].m.mem_offset;
    } else if (req == VIDIOC_SUBSCRIBE_EVENT) {
        struct v4l2_event_subscription *e = arg;
        if (e->type == V4L2_EVENT_SOURCE_CHANGE) g_cand.subscribed = 1;
    } else if (req == VIDIOC_STREAMON || req == VIDIOC_STREAMOFF) {
        if (*(int *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) g_cand.out_streaming = req == VIDIOC_STREAMON;
    }
}

static void emu_cand_note_mmap(off_t off, void *addr, size_t len)
{
    if (addr == MAP_FAILED) return;
    for (int k = 0; k < EMU_NBUF; ++k)
        if (!g_cand.maps[k].addr) { g_cand.maps[k].off = (uint64_t)off; g_cand.maps[k].addr = addr; g_cand.maps[k].len = len; return; }
}

static void emu_note_munmap(void *addr)
{
    for (int k = 0; k < EMU_NBUF; ++k)
        if (g_cand.maps[k].addr == addr) g_cand.maps[k].addr = NULL;
    if (g_emu.fd < 0) return;
    pthread_mutex_lock(&g_emu.mu);
    for (int i = 0; i < EMU_NBUF; ++i)
        if (g_emu.out_addr[i] == addr) { g_emu.out_addr[i] = NULL; g_emu.out_len[i] = 0; }
    pthread_mutex_unlock(&g_emu.mu);
}

static void emu_close(int fd)
{
    pthread_mutex_lock(&g_emu.mu);
    g_emu.stop = 1;
    pthread_cond_broadcast(&g_emu.cv);
    pthread_cond_broadcast(&g_emu.dq_cv);
    pthread_mutex_unlock(&g_emu.mu);
    if (g_emu.worker_running) pthread_join(g_emu.worker, NULL);
    g_emu.worker_running = 0;
    for (int i = 0; i < EMU_NBUF; ++i)
        if (g_emu.cap_target[i] >= 0 && g_dec) { p_release(g_dec, g_emu.cap_target[i]); g_emu.cap_target[i] = -1; }
    pthread_mutex_lock(&g_emu.mu);
    fc_pre_clear_locked();   // v7.19b
    pthread_mutex_unlock(&g_emu.mu);
    for (int i = 0; i < EMU_NBUF; ++i) g_emu.out_addr[i] = NULL;   // vrlink unmaps its own mappings
    if (g_emu.efd >= 0) real_close(g_emu.efd);   // also drops it from vrlink's epoll set
    g_emu.efd = -1;
    g_emu.fd = -1;
    g_emu.epfd = -1;
    slog("EMU: emulated decoder fd %d closed", fd);
}

// ---- epoll ----
static int emu_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    struct epoll_event e = {0};
    if (ev) { e.events = EPOLLIN; e.data = ev->data; }
    int r = real_epoll_ctl(epfd, op, g_emu.efd, op == EPOLL_CTL_DEL ? NULL : &e);
    int err = errno;
    if (op != EPOLL_CTL_DEL && ev && r == 0) { g_emu.epfd = epfd; g_emu.epdata = ev->data.u64; }
    if (op == EPOLL_CTL_DEL && r == 0) g_emu.epfd = -1;
    slog("EMU: epoll_ctl(epfd %d, op %d, decoder fd %d, events 0x%x) -> eventfd %d registered instead -> %d", epfd, op, fd,
         ev ? ev->events : 0, g_emu.efd, r);
    errno = err;
    return r;
}

// Rewrites decoder entries returned by epoll_wait; returns the new count.
static int emu_filter_events(struct epoll_event *evs, int r)
{
    int out = 0;
    pthread_mutex_lock(&g_emu.mu);
    uint32_t mask = emu_ready_mask_locked();
    emu_update_ready_locked();      // clears a stale eventfd count
    pthread_mutex_unlock(&g_emu.mu);
    for (int i = 0; i < r; ++i) {
        if (evs[i].data.u64 == g_emu.epdata) {
            if (!mask) continue;
            evs[i].events = mask;
        }
        evs[out++] = evs[i];
    }
    return out;
}

// ---- mmap of OUTPUT buffers ----
static void *emu_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    void *p = real_mmap(addr, len, prot, flags, fd, off);   // hardware OUTPUT buffers stay the hardware's
    if (p != MAP_FAILED) {
        pthread_mutex_lock(&g_emu.mu);
        for (int i = 0; i < EMU_NBUF; ++i)
            if (g_emu.out_off[i] == (uint64_t)off) { g_emu.out_addr[i] = p; g_emu.out_len[i] = len; }
        pthread_mutex_unlock(&g_emu.mu);
    }
    return p;
}

// ---- ioctls ----
static void emu_fill_cap_fmt(struct v4l2_format *f)
{
    f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_pix_format_mplane *m = &f->fmt.pix_mp;
    memset(m, 0, sizeof(*m));
    m->width = g_emu.cap_w;
    m->height = g_emu.cap_h;
    m->pixelformat = v4l2_fourcc('Q', '0', '8', 'C');   // vrlink: anything but P010/Q10C -> 8-bit Q08C path
    m->field = V4L2_FIELD_NONE;
    m->colorspace = V4L2_COLORSPACE_BT2020;   // what iris reported; vrcompositor ignores it (identity sampler)
    m->quantization = V4L2_QUANTIZATION_FULL_RANGE;
    m->num_planes = 1;
    m->plane_fmt[0].sizeimage = g_emu.cap_sizeimage;
    m->plane_fmt[0].bytesperline = g_emu.cap_bpl;
}

static int emu_ioctl(int fd, unsigned long req, void *arg)
{
    int ret = 0, err = 0;
    pthread_mutex_lock(&g_emu.mu);
    switch (req) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *c = arg;
        memset(c, 0, sizeof(*c));
        snprintf((char *)c->driver, sizeof(c->driver), "slp_pyrowave");
        snprintf((char *)c->card, sizeof(c->card), "Pyrowave decoder (slp)");
        snprintf((char *)c->bus_info, sizeof(c->bus_info), "platform:slp");
        c->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        c->capabilities = c->device_caps | V4L2_CAP_DEVICE_CAPS;
        break;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *d = arg;
        if (d->index != 0) { ret = -1; err = EINVAL; break; }
        if (d->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            d->pixelformat = V4L2_PIX_FMT_HEVC; d->flags = V4L2_FMT_FLAG_COMPRESSED;
            snprintf((char *)d->description, sizeof(d->description), "HEVC (carries SLPW)");
        } else if (d->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            d->pixelformat = v4l2_fourcc('Q', '0', '8', 'C'); d->flags = 0;
            snprintf((char *)d->description, sizeof(d->description), "QC 8-bit UBWC");
        } else { ret = -1; err = EINVAL; }
        break;
    }
    case VIDIOC_G_FMT:
    case VIDIOC_S_FMT:
    case VIDIOC_TRY_FMT: {
        struct v4l2_format *f = arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            // OUTPUT side stays on the hardware (its buffers carry the frames).
            ret = real_ioctl(fd, req, arg);
            err = errno;
            if (ret == 0 && req == VIDIOC_S_FMT) {
                g_emu.out_w = f->fmt.pix_mp.width; g_emu.out_h = f->fmt.pix_mp.height;
                g_emu.out_fourcc = f->fmt.pix_mp.pixelformat;
                if (f->fmt.pix_mp.plane_fmt[0].sizeimage) g_emu.out_sizeimage = f->fmt.pix_mp.plane_fmt[0].sizeimage;
            }
        } else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            emu_fill_cap_fmt(f);
        } else { ret = -1; err = EINVAL; }
        break;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *r = arg;
        if (r->count > EMU_NBUF) r->count = EMU_NBUF;
        r->capabilities = 0x15;   // as iris: MMAP | DMABUF | ORPHANED_BUFS
        if (r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            memset(&g_emu.jobs, 0, sizeof(g_emu.jobs));
            memset(&g_emu.out_done, 0, sizeof(g_emu.out_done));
            ret = real_ioctl(fd, req, arg);   // hardware owns the OUTPUT buffers
            err = errno;
            if (ret == 0) {
                g_emu.out_count = r->count;
                if (!r->count) for (int i = 0; i < EMU_NBUF; ++i) { g_emu.out_addr[i] = NULL; g_emu.out_off[i] = 0; }
            }
        } else if (r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            if (r->memory != V4L2_MEMORY_DMABUF) { ret = -1; err = EINVAL; break; }
            for (int i = 0; i < EMU_NBUF; ++i) {
                if (g_emu.cap_target[i] >= 0 && g_dec) {
                    pthread_mutex_unlock(&g_emu.mu);
                    p_release(g_dec, g_emu.cap_target[i]);
                    pthread_mutex_lock(&g_emu.mu);
                }
                g_emu.cap_target[i] = -1; g_emu.cap_fd[i] = -1; g_emu.cap_queued[i] = 0; g_emu.cap_ino[i] = 0;
            }
            fc_pre_clear_locked();   // v7.19b: new surfaces follow
            memset(&g_emu.cap_done, 0, sizeof(g_emu.cap_done));
            g_emu.cap_count = r->count;
        } else { ret = -1; err = EINVAL; }
        emu_update_ready_locked();
        break;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = arg;
        if (!b->m.planes || b->length < 1) { ret = -1; err = EINVAL; break; }
        b->flags = 0;
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            ret = real_ioctl(fd, req, arg);
            err = errno;
            if (ret == 0 && b->index < EMU_NBUF) g_emu.out_off[b->index] = b->m.planes[0].m.mem_offset;
        } else if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && b->index < g_emu.cap_count) {
            b->length = 1;
            b->m.planes[0].length = g_emu.cap_sizeimage;
            b->m.planes[0].m.mem_offset = 0x40000000;
            b->m.planes[0].bytesused = 0;
        } else { ret = -1; err = EINVAL; }
        break;
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer *b = arg;
        if (!b->m.planes || b->length < 1) { ret = -1; err = EINVAL; break; }
        if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && b->index < g_emu.out_count) {
            emu_item it = {b->index, b->timestamp, b->m.planes[0].bytesused, 0, mono_ms()};
            if (ring_push(&g_emu.jobs, it) != 0) { ret = -1; err = EBUSY; break; }
            b->flags = (b->flags | V4L2_BUF_FLAG_QUEUED) & ~V4L2_BUF_FLAG_DONE;
            pthread_cond_broadcast(&g_emu.cv);
        } else if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && b->index < g_emu.cap_count && b->index < EMU_NBUF) {
            int dfd = b->m.planes[0].m.fd & 0x3fffffff;   // vrlink passes 0x40000000 | fd
            if (g_emu.cap_fd[b->index] != dfd && g_emu.cap_target[b->index] >= 0 && g_dec) {
                int t = g_emu.cap_target[b->index];
                g_emu.cap_target[b->index] = -1;
                pthread_mutex_unlock(&g_emu.mu);
                p_release(g_dec, t);
                pthread_mutex_lock(&g_emu.mu);
            }
            if (g_emu.cap_fd[b->index] != dfd) {
                g_emu.cap_ino[b->index] = fd_inode(dfd);   // v7.19: pairing with vrcompositor's import
                int k = fc_pre_find_locked(g_emu.cap_ino[b->index]);
                if (k >= 0 && !g_fc_pre[k].claimed && g_fc_pre[k].target >= 0 && g_emu.cap_target[b->index] < 0) {
                    g_emu.cap_target[b->index] = g_fc_pre[k].target;   // v7.19b: imported when vrlink exported it
                    g_fc_pre[k].claimed = 1;
                } else if (k >= 0) g_fc_pre[k].claimed = 1;          // still importing: the worker hands it over
                if (g_fc) g_emu.imp_pending = 1;           // full colour: import now, before vrcompositor asks
            }
            g_emu.cap_fd[b->index] = dfd;
            g_emu.cap_queued[b->index] = 1;
            g_emu.cap_qseq[b->index] = ++g_emu.qseq;
            b->flags = (b->flags | V4L2_BUF_FLAG_QUEUED) & ~V4L2_BUF_FLAG_DONE;
            pthread_cond_broadcast(&g_emu.cv);
        } else { ret = -1; err = EINVAL; }
        break;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer *b = arg;
        if (!b->m.planes || b->length < 1) { ret = -1; err = EINVAL; break; }
        int is_out = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        emu_ring *rg = is_out ? &g_emu.out_done : &g_emu.cap_done;
        if (!rg->n) {   // blocking fd: wait a little, then EAGAIN (vrlink only dequeues after epoll says ready)
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_nsec += 100 * 1000000L;
            if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_emu.dq_cv, &g_emu.mu, &dl);
        }
        emu_item it;
        if (ring_pop(rg, &it) != 0) { ret = -1; err = EAGAIN; break; }
        b->index = it.idx;
        b->flags = it.flags;
        b->field = V4L2_FIELD_NONE;
        b->timestamp = it.ts;
        b->sequence = 0;
        b->length = 1;
        b->m.planes[0].bytesused = it.bytes;
        b->m.planes[0].length = is_out ? g_emu.out_sizeimage : g_emu.cap_sizeimage;
        emu_update_ready_locked();
        break;
    }
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF: {
        int on = req == VIDIOC_STREAMON;
        int type = *(int *)arg;
        if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            real_ioctl(fd, req, arg);   // keep the (idle) hardware OUTPUT queue state consistent
            g_emu.out_streaming = on;
            if (!on) { memset(&g_emu.jobs, 0, sizeof(g_emu.jobs)); memset(&g_emu.out_done, 0, sizeof(g_emu.out_done)); }
        } else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            g_emu.cap_streaming = on;
            if (!on) {
                for (int i = 0; i < EMU_NBUF; ++i) g_emu.cap_queued[i] = 0;
                memset(&g_emu.cap_done, 0, sizeof(g_emu.cap_done));
            }
        } else { ret = -1; err = EINVAL; }
        emu_update_ready_locked();
        pthread_cond_broadcast(&g_emu.cv);
        break;
    }
    case VIDIOC_SUBSCRIBE_EVENT: {
        struct v4l2_event_subscription *s = arg;
        if (s->type == V4L2_EVENT_SOURCE_CHANGE) g_emu.subscribed = 1;
        break;
    }
    case VIDIOC_UNSUBSCRIBE_EVENT:
        break;
    case VIDIOC_DQEVENT: {
        struct v4l2_event *e = arg;
        if (!g_emu.ev_pending) { ret = -1; err = ENOENT; break; }
        memset(e, 0, sizeof(*e));
        e->type = V4L2_EVENT_SOURCE_CHANGE;
        e->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
        e->sequence = g_emu.ev_seq++;
        clock_gettime(CLOCK_MONOTONIC, &e->timestamp);
        g_emu.ev_pending = 0;
        emu_update_ready_locked();
        break;
    }
    case VIDIOC_G_CTRL: {
        struct v4l2_control *c = arg;
        if (c->id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE) c->value = 4;
        else { ret = -1; err = EINVAL; }
        break;
    }
    case VIDIOC_S_CTRL:          // display-delay controls: iris rejects them too (EINVAL), vrlink ignores it
    case VIDIOC_S_EXT_CTRLS:
        ret = -1; err = EINVAL;
        break;
    case VIDIOC_G_SELECTION: {
        struct v4l2_selection *s = arg;
        s->r.left = s->r.top = 0;
        s->r.width = g_emu.cap_w;
        s->r.height = g_emu.cap_h;
        break;
    }
    case VIDIOC_DECODER_CMD:
    case VIDIOC_TRY_DECODER_CMD:
        break;
    default: {
        static int unknown_logged;
        if (unknown_logged++ < 20) slog("EMU: unhandled ioctl 0x%lx -> ENOTTY", req);
        ret = -1; err = ENOTTY;
        break;
    }
    }
    pthread_mutex_unlock(&g_emu.mu);
    errno = err;
    return ret;
}
