// Steam Link Pyrowave - Frame-side observe-only layer (phase 0).
// SPDX-License-Identifier: MIT
//
// A Vulkan implicit layer whose only real job is to get our code into the vrlink
// client process without root.  In every other process, and in vrlink unless it is
// armed, it declines to load (vkNegotiateLoaderLayerInterfaceVersion fails and no
// vkGetInstanceProcAddr is exported, so the loader skips it).
//
// When armed inside vrlink it:
//   - behaves as a plain pass-through Vulkan layer (no Vulkan behaviour change),
//   - patches vrlink's own imports of open/ioctl/mmap/munmap/close (GOT entries of the
//     main executable only) to LOG the V4L2 decoder conversation,
//   - writes /tmp/slp_layer/vrlink-<pid>.log.
// It never changes arguments or return values.  Observe only.
//
// Arming / safety:
//   armed      : /tmp/slp_arm exists (tmpfs, cleared by reboot)
//   kill switch: ~/.local/lib/slp/DISABLED exists, or env SLP_LAYER_DISABLE=1 (manifest)
//   uninstall  : uninstall.sh removes the manifest and the library.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define EXPORT __attribute__((visibility("default")))

#if defined(__aarch64__)
#define SLP_R_JUMP_SLOT R_AARCH64_JUMP_SLOT
#define SLP_R_GLOB_DAT  R_AARCH64_GLOB_DAT
#elif defined(__x86_64__)          // host-side testing only
#define SLP_R_JUMP_SLOT R_X86_64_JUMP_SLOT
#define SLP_R_GLOB_DAT  R_X86_64_GLOB_DAT
#endif

// Bind dlsym to the original aarch64 baseline version so the layer loads on any
// glibc >= 2.17 (vrlink itself only requires GLIBC_2.17).  glibc 2.34+ still exports
// dlsym@GLIBC_2.17 as a compat alias of the same function.
#if defined(__aarch64__)
__asm__(".symver dlsym,dlsym@GLIBC_2.17");
#endif

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

static int g_active = -1;   // -1 unknown, 0 inactive, 1 vrlink, 2 vrcompositor (logging and/or v7.19 full colour)
static int g_cfc;           // v7.19: vrcompositor with full colour armed (shared-image swap of the stream views)
static int g_cp_log;        // v7.18 compositor logging (/tmp/slp_arm_compositor)
static char g_exe_leaf[64];

static int file_exists(const char *p)
{
    return access(p, F_OK) == 0;   // access() keeps us on old glibc symbol versions
}

static int compute_active(void)
{
    const char *dis = getenv("SLP_LAYER_DISABLE");
    if (dis && dis[0] == '1') return 0;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *leaf = strrchr(exe, '/');
    leaf = leaf ? leaf + 1 : exe;
    snprintf(g_exe_leaf, sizeof(g_exe_leaf), "%.63s", leaf);
    int mode = 0;
    if (strcmp(leaf, "vrlink") == 0) {
        const char *home = getenv("HOME");
        char autof[512];
        snprintf(autof, sizeof(autof), "%s/.local/lib/slp/AUTO", home ? home : "/home/steamos");
        if (file_exists("/tmp/slp_arm") || file_exists(autof)) mode = 1;   // AUTO: persistent auto-detect mode
    }
    else if (strcmp(leaf, "vrcompositor") == 0) {
        const char *home = getenv("HOME");
        char fcf[512];
        snprintf(fcf, sizeof(fcf), "%s/.local/lib/slp/FULLCOLOUR", home ? home : "/home/steamos");
        g_cp_log = file_exists("/tmp/slp_arm_compositor");
        g_cfc = file_exists("/tmp/slp_fullcolour") || file_exists(fcf);
        if (g_cp_log || g_cfc) mode = 2;
    }
    if (!mode) return 0;
    const char *home = getenv("HOME");
    if (home) {
        char kill[512];
        snprintf(kill, sizeof(kill), "%s/.local/lib/slp/DISABLED", home);
        if (file_exists(kill)) { g_cfc = 0; return 0; }
    }
    return mode;
}

static int is_active(void)
{
    if (g_active < 0) g_active = compute_active();
    return g_active;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static FILE *g_log;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static double g_t0_ms;

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void slog(const char *fmt, ...)
{
    if (!g_log) return;
    pthread_mutex_lock(&g_log_mutex);
    fprintf(g_log, "[%9.3f] [t%d] ", mono_ms() - g_t0_ms, (int)syscall(SYS_gettid));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mutex);
}

static void open_log(void)
{
    mkdir("/tmp/slp_layer", 0700);
    char path[256];
    snprintf(path, sizeof(path), "/tmp/slp_layer/%s-%d.log", g_exe_leaf[0] ? g_exe_leaf : "proc", (int)getpid());
    g_log = fopen(path, "w");
    g_t0_ms = mono_ms();
}

// ---------------------------------------------------------------------------
// V4L2 tracing
// ---------------------------------------------------------------------------

typedef int   (*pfn_open)(const char *, int, ...);
typedef int   (*pfn_ioctl)(int, unsigned long, ...);
typedef void *(*pfn_mmap)(void *, size_t, int, int, int, off_t);
typedef int   (*pfn_munmap)(void *, size_t);
typedef int   (*pfn_close)(int);
typedef int   (*pfn_epoll_ctl)(int, int, int, struct epoll_event *);
typedef int   (*pfn_epoll_wait)(int, struct epoll_event *, int, int);
typedef int   (*pfn_poll)(struct pollfd *, nfds_t, int);
typedef int   (*pfn_fcntl)(int, int, ...);

static pfn_open   real_open;
static pfn_ioctl  real_ioctl;
static pfn_mmap   real_mmap;
static pfn_munmap real_munmap;
static pfn_close  real_close;
static pfn_epoll_ctl  real_epoll_ctl;
static pfn_epoll_wait real_epoll_wait;
static pfn_poll       real_poll;
static pfn_fcntl      real_fcntl;

#define MAX_FDS 8
static atomic_int g_vfd[MAX_FDS];     // tracked /dev/video* fds (0 = empty; stored fd+1)
static char g_vpath[MAX_FDS][256];

static int tracked(int fd)
{
    if (fd < 0) return 0;
    for (int i = 0; i < MAX_FDS; ++i) if (atomic_load(&g_vfd[i]) == fd + 1) return 1;
    return 0;
}
static void track(int fd, const char *path)
{
    for (int i = 0; i < MAX_FDS; ++i) {
        int z = 0;
        if (atomic_compare_exchange_strong(&g_vfd[i], &z, fd + 1)) {
            snprintf(g_vpath[i], sizeof(g_vpath[i]), "%s", path);
            return;
        }
    }
}
static void untrack(int fd)
{
    for (int i = 0; i < MAX_FDS; ++i) {
        int v = fd + 1;
        atomic_compare_exchange_strong(&g_vfd[i], &v, 0);
    }
}

static const char *fourcc(uint32_t f, char out[8])
{
    for (int i = 0; i < 4; ++i) {
        char c = (char)((f >> (8 * i)) & 0xff);
        out[i] = (c >= 32 && c < 127) ? c : '.';
    }
    out[4] = 0;
    return out;
}

static const char *ioc_name(unsigned long r)
{
    switch (r) {
#define N(x) case x: return #x;
    N(VIDIOC_QUERYCAP) N(VIDIOC_ENUM_FMT) N(VIDIOC_G_FMT) N(VIDIOC_S_FMT) N(VIDIOC_TRY_FMT)
    N(VIDIOC_REQBUFS) N(VIDIOC_QUERYBUF) N(VIDIOC_QBUF) N(VIDIOC_DQBUF) N(VIDIOC_EXPBUF)
    N(VIDIOC_STREAMON) N(VIDIOC_STREAMOFF) N(VIDIOC_G_CTRL) N(VIDIOC_S_CTRL)
    N(VIDIOC_G_EXT_CTRLS) N(VIDIOC_S_EXT_CTRLS) N(VIDIOC_QUERYCTRL) N(VIDIOC_QUERY_EXT_CTRL)
    N(VIDIOC_SUBSCRIBE_EVENT) N(VIDIOC_UNSUBSCRIBE_EVENT) N(VIDIOC_DQEVENT)
    N(VIDIOC_DECODER_CMD) N(VIDIOC_TRY_DECODER_CMD) N(VIDIOC_G_SELECTION) N(VIDIOC_S_SELECTION)
    N(VIDIOC_G_PARM) N(VIDIOC_S_PARM) N(VIDIOC_ENUM_FRAMESIZES) N(VIDIOC_CREATE_BUFS)
    N(VIDIOC_PREPARE_BUF) N(VIDIOC_G_CROP) N(VIDIOC_CROPCAP)
#undef N
    default: return NULL;
    }
}

static const char *buf_type(uint32_t t)
{
    switch (t) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE: return "CAP";
    case V4L2_BUF_TYPE_VIDEO_OUTPUT: return "OUT";
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE: return "CAP_MP";
    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE: return "OUT_MP";
    default: return "?";
    }
}
static int is_mplane(uint32_t t)
{
    return t == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}
static int is_output(uint32_t t)
{
    return t == V4L2_BUF_TYPE_VIDEO_OUTPUT || t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
}

// Per-frame decode latency: OUTPUT QBUF timestamp -> CAPTURE DQBUF with the same timestamp.
#define TS_RING 64
static struct { uint64_t ts; double t_q; uint32_t bytes; } g_ts[TS_RING];
static int g_ts_next;
static pthread_mutex_t g_stat_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
    double win;
    uint32_t qout, dqout, qcap, dqcap, matched, eagain;
    double lat_sum, lat_max;
    uint64_t bytes_sum; uint32_t bytes_max;
    int windows;
} g_st;

static atomic_int g_calls_logged;
#define FULL_LOG_CALLS 600

static void stat_roll(double now)
{
    if (g_st.win == 0) { g_st.win = now; return; }
    if (now - g_st.win < 1000.0) return;
    if (g_st.windows < 600)
        slog("[STAT] %.0fs: OUT q %u dq %u | CAP q %u dq %u | decode latency (OUT QBUF->CAP DQBUF) avg %.2f max %.2f ms over %u | "
             "bytes avg %.0f max %u | EAGAIN %u",
             (now - g_t0_ms) / 1000.0, g_st.qout, g_st.dqout, g_st.qcap, g_st.dqcap,
             g_st.matched ? g_st.lat_sum / g_st.matched : 0.0, g_st.lat_max, g_st.matched,
             g_st.qout ? (double)g_st.bytes_sum / g_st.qout : 0.0, g_st.bytes_max, g_st.eagain);
    g_st.windows++;
    double w = g_st.win;
    memset(&g_st, 0, sizeof(g_st));
    g_st.win = w + 1000.0 * (double)(long)((now - w) / 1000.0);
}

static void trace_buf(const char *what, int fd, const struct v4l2_buffer *b, int ret, int err, int full)
{
    uint32_t bytes = b->bytesused, len = b->length;
    int fdm = -1;
    if (is_mplane(b->type) && b->m.planes && b->length > 0 && b->length <= VIDEO_MAX_PLANES) {
        bytes = b->m.planes[0].bytesused;
        len = b->m.planes[0].length;
        if (b->memory == V4L2_MEMORY_DMABUF) fdm = b->m.planes[0].m.fd;
    } else if (b->memory == V4L2_MEMORY_DMABUF) {
        fdm = b->m.fd;
    }
    uint64_t ts = (uint64_t)b->timestamp.tv_sec * 1000000ull + (uint64_t)b->timestamp.tv_usec;
    double now = mono_ms();
    int dq = what[0] == 'D';
    pthread_mutex_lock(&g_stat_mutex);
    stat_roll(now);
    if (ret < 0) {
        if (err == EAGAIN) g_st.eagain++;
    } else if (is_output(b->type)) {
        if (dq) g_st.dqout++;
        else {
            g_st.qout++;
            g_st.bytes_sum += bytes;
            if (bytes > g_st.bytes_max) g_st.bytes_max = bytes;
            g_ts[g_ts_next].ts = ts; g_ts[g_ts_next].t_q = now; g_ts[g_ts_next].bytes = bytes;
            g_ts_next = (g_ts_next + 1) % TS_RING;
        }
    } else {
        if (dq) {
            g_st.dqcap++;
            for (int i = 0; i < TS_RING; ++i)
                if (g_ts[i].t_q > 0 && g_ts[i].ts == ts) {
                    double l = now - g_ts[i].t_q;
                    g_st.matched++; g_st.lat_sum += l;
                    if (l > g_st.lat_max) g_st.lat_max = l;
                    g_ts[i].t_q = 0;
                    break;
                }
        } else g_st.qcap++;
    }
    pthread_mutex_unlock(&g_stat_mutex);
    if (full)
        slog("%s fd=%d %s idx=%u mem=%u bytesused=%u len=%u flags=0x%x field=%u ts=%llu.%06llu seq=%u dmabuf_fd=%d planes=%u -> %d%s%s",
             what, fd, buf_type(b->type), b->index, b->memory, bytes, len, b->flags, b->field,
             (unsigned long long)b->timestamp.tv_sec, (unsigned long long)b->timestamp.tv_usec, b->sequence, fdm,
             is_mplane(b->type) ? b->length : 0, ret, ret < 0 ? " errno=" : "", ret < 0 ? strerror(err) : "");
}

static void trace_fmt(const char *what, int fd, const struct v4l2_format *f, int ret, int err)
{
    char cc[8];
    if (is_mplane(f->type)) {
        const struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
        slog("%s fd=%d %s %ux%u %s field=%u colorspace=%u ycbcr=%u quant=%u xfer=%u planes=%u "
             "[p0 size=%u bpl=%u] [p1 size=%u bpl=%u] -> %d%s%s",
             what, fd, buf_type(f->type), p->width, p->height, fourcc(p->pixelformat, cc), p->field, p->colorspace,
             p->ycbcr_enc, p->quantization, p->xfer_func, p->num_planes, p->plane_fmt[0].sizeimage,
             p->plane_fmt[0].bytesperline, p->plane_fmt[1].sizeimage, p->plane_fmt[1].bytesperline, ret,
             ret < 0 ? " errno=" : "", ret < 0 ? strerror(err) : "");
    } else {
        const struct v4l2_pix_format *p = &f->fmt.pix;
        slog("%s fd=%d %s %ux%u %s field=%u sizeimage=%u bpl=%u colorspace=%u -> %d%s%s", what, fd, buf_type(f->type),
             p->width, p->height, fourcc(p->pixelformat, cc), p->field, p->sizeimage, p->bytesperline, p->colorspace, ret,
             ret < 0 ? " errno=" : "", ret < 0 ? strerror(err) : "");
    }
}

static void trace_ioctl(int fd, unsigned long req, void *arg, int ret, int err)
{
    int full = atomic_fetch_add(&g_calls_logged, 1) < FULL_LOG_CALLS;
    const char *nm = ioc_name(req);
    // Per-frame calls are always counted; detailed lines only for the first calls,
    // plus every failure that is not a plain EAGAIN.
    int fail = ret < 0 && err != EAGAIN;
    switch (req) {
    case VIDIOC_QBUF: case VIDIOC_DQBUF:
        if (arg) trace_buf(req == VIDIOC_QBUF ? "QBUF" : "DQBUF", fd, (const struct v4l2_buffer *)arg, ret, err, full || fail);
        return;
    default: break;
    }
    if (!full && !fail) return;
    if (!arg) { slog("%s(0x%lx) fd=%d arg=NULL -> %d", nm ? nm : "ioctl", req, fd, ret); return; }
    char cc[8];
    switch (req) {
    case VIDIOC_QUERYCAP: {
        const struct v4l2_capability *c = arg;
        slog("QUERYCAP fd=%d driver=%.16s card=%.32s caps=0x%08x device_caps=0x%08x -> %d", fd, c->driver, c->card,
             c->capabilities, c->device_caps, ret);
        break;
    }
    case VIDIOC_ENUM_FMT: {
        const struct v4l2_fmtdesc *d = arg;
        slog("ENUM_FMT fd=%d %s idx=%u %s flags=0x%x desc=%.32s -> %d", fd, buf_type(d->type), d->index,
             fourcc(d->pixelformat, cc), d->flags, d->description, ret);
        break;
    }
    case VIDIOC_G_FMT: case VIDIOC_S_FMT: case VIDIOC_TRY_FMT:
        trace_fmt(nm, fd, arg, ret, err);
        break;
    case VIDIOC_REQBUFS: {
        const struct v4l2_requestbuffers *r = arg;
        slog("REQBUFS fd=%d %s count=%u memory=%u caps=0x%x -> %d", fd, buf_type(r->type), r->count, r->memory,
             r->capabilities, ret);
        break;
    }
    case VIDIOC_QUERYBUF: {
        const struct v4l2_buffer *b = arg;
        if (is_mplane(b->type) && b->m.planes && b->length && b->length <= VIDEO_MAX_PLANES)
            slog("QUERYBUF fd=%d %s idx=%u mem=%u planes=%u p0 len=%u off=0x%x p1 len=%u off=0x%x -> %d", fd,
                 buf_type(b->type), b->index, b->memory, b->length, b->m.planes[0].length, b->m.planes[0].m.mem_offset,
                 b->length > 1 ? b->m.planes[1].length : 0, b->length > 1 ? b->m.planes[1].m.mem_offset : 0, ret);
        else
            slog("QUERYBUF fd=%d %s idx=%u mem=%u len=%u off=0x%x -> %d", fd, buf_type(b->type), b->index, b->memory,
                 b->length, b->m.offset, ret);
        break;
    }
    case VIDIOC_EXPBUF: {
        const struct v4l2_exportbuffer *e = arg;
        slog("EXPBUF fd=%d %s idx=%u plane=%u flags=0x%x -> fd %d (ret %d)", fd, buf_type(e->type), e->index, e->plane,
             e->flags, e->fd, ret);
        break;
    }
    case VIDIOC_STREAMON: case VIDIOC_STREAMOFF:
        slog("%s fd=%d %s -> %d", nm, fd, buf_type(*(const uint32_t *)arg), ret);
        break;
    case VIDIOC_S_CTRL: case VIDIOC_G_CTRL: {
        const struct v4l2_control *c = arg;
        slog("%s fd=%d id=0x%08x value=%d -> %d%s%s", nm, fd, c->id, c->value, ret, ret < 0 ? " errno=" : "",
             ret < 0 ? strerror(err) : "");
        break;
    }
    case VIDIOC_S_EXT_CTRLS: case VIDIOC_G_EXT_CTRLS: {
        const struct v4l2_ext_controls *c = arg;
        slog("%s fd=%d which=0x%x count=%u first id=0x%08x value=%d -> %d%s%s", nm, fd, c->which, c->count,
             c->count && c->controls ? c->controls[0].id : 0, c->count && c->controls ? c->controls[0].value : 0, ret,
             ret < 0 ? " errno=" : "", ret < 0 ? strerror(err) : "");
        break;
    }
    case VIDIOC_SUBSCRIBE_EVENT: case VIDIOC_UNSUBSCRIBE_EVENT: {
        const struct v4l2_event_subscription *s = arg;
        slog("%s fd=%d type=%u id=%u flags=0x%x -> %d", nm, fd, s->type, s->id, s->flags, ret);
        break;
    }
    case VIDIOC_DQEVENT: {
        const struct v4l2_event *e = arg;
        slog("DQEVENT fd=%d type=%u changes=0x%x pending=%u seq=%u -> %d", fd, e->type,
             e->type == V4L2_EVENT_SOURCE_CHANGE ? e->u.src_change.changes : 0, e->pending, e->sequence, ret);
        break;
    }
    case VIDIOC_DECODER_CMD: case VIDIOC_TRY_DECODER_CMD: {
        const struct v4l2_decoder_cmd *c = arg;
        slog("%s fd=%d cmd=%u flags=0x%x -> %d", nm, fd, c->cmd, c->flags, ret);
        break;
    }
    case VIDIOC_G_SELECTION: case VIDIOC_S_SELECTION: {
        const struct v4l2_selection *s = arg;
        slog("%s fd=%d %s target=0x%x rect=%d,%d %ux%u -> %d", nm, fd, buf_type(s->type), s->target, s->r.left, s->r.top,
             s->r.width, s->r.height, ret);
        break;
    }
    default:
        slog("%s(0x%lx) fd=%d -> %d%s%s", nm ? nm : "ioctl", req, fd, ret, ret < 0 ? " errno=" : "",
             ret < 0 ? strerror(err) : "");
        break;
    }
}

// ---------------------------------------------------------------------------
// Hooks (installed into vrlink's GOT)
// ---------------------------------------------------------------------------

#include "slp_emu.inc.c"

static char g_decoder_path[64] = "/dev/video22";

static int hook_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = real_open(path, flags, mode);
    int err = errno;
    if (path && strncmp(path, "/dev/video", 10) == 0) {
        if (fd >= 0) track(fd, path);
        slog("open(%s, 0x%x) -> %d%s%s", path, flags, fd, fd < 0 ? " errno=" : "", fd < 0 ? strerror(err) : "");
        if (fd >= 0 && strcmp(path, g_decoder_path) == 0 && emu_armed() && g_emu.fd < 0) {
            emu_cand_reset(fd);
            slog("EMU: watching decoder fd %d: hardware until the first frame; SLPW -> Pyrowave takeover, else stock HEVC", fd);
        }
    }
    errno = err;
    return fd;
}

static void dump_epoll_regs(const char *why);
static void log_fd_flags(int fd, const char *why);
static int g_own_fd;            // v7.20, below
static void own_unmap(void);

static int hook_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int ret, err;
    if (emu_is_fd(fd)) {
        ret = emu_ioctl(fd, req, arg);
        err = errno;
        trace_ioctl(fd, req, arg, ret, err);
        errno = err;
        return ret;
    }
    if (emu_is_cand(fd)) {
        if (req == VIDIOC_QBUF && emu_cand_qbuf_is_slpw(arg) && emu_takeover(fd) == 0) {
            ret = emu_ioctl(fd, req, arg);   // the first frame goes to Pyrowave, not the hardware
            err = errno;
            trace_ioctl(fd, req, arg, ret, err);
            errno = err;
            return ret;
        }
        if (req == VIDIOC_QBUF) {
            struct v4l2_buffer *b = arg;
            if (b && b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
                slog("EMU: first frame on decoder fd %d is not SLPW -> stock hardware decode for this session", fd);
                g_cand.fd = -1;
                if (fd == g_own_fd) own_unmap();   // v7.20: our mappings would keep the hardware's buffers busy
            }
        }
    }
    ret = real_ioctl(fd, req, arg);
    err = errno;
    if (emu_is_cand(fd)) emu_cand_after_ioctl(req, arg, ret);
    if (tracked(fd)) {
        trace_ioctl(fd, req, arg, ret, err);
        static atomic_int probes;
        if ((req == VIDIOC_STREAMON || req == VIDIOC_DQEVENT || req == VIDIOC_REQBUFS) && atomic_fetch_add(&probes, 1) < 8) {
            dump_epoll_regs(ioc_name(req));
            log_fd_flags(fd, ioc_name(req));
        }
    }
    errno = err;
    return ret;
}

static void *hook_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (emu_is_fd(fd)) {
        void *q = emu_mmap(addr, len, prot, flags, fd, off);
        int e = errno;
        slog("EMU: mmap(decoder fd %d, len %zu, off 0x%llx) -> %p", fd, len, (unsigned long long)off, q);
        errno = e;
        return q;
    }
    void *p = real_mmap(addr, len, prot, flags, fd, off);
    int err = errno;
    if (emu_is_cand(fd)) emu_cand_note_mmap(off, p, len);
    if (tracked(fd)) slog("mmap fd=%d len=%zu prot=0x%x flags=0x%x off=0x%llx -> %p", fd, len, prot, flags,
                          (unsigned long long)off, p);
    errno = err;
    return p;
}

static int hook_munmap(void *addr, size_t len)
{
    emu_note_munmap(addr);
    return real_munmap(addr, len);
}

// ---- v5: how vrlink waits on the decoder (epoll / poll / fd flags) ----
static atomic_int g_dec_epfd = -1;          // epoll set that contains the decoder fd
static atomic_ullong g_dec_data;             // epoll_event.data registered for the decoder fd
static atomic_int g_epw_logged, g_poll_logged, g_epw_calls;

// Tiny parsers: sscanf/atoi would bind to glibc 2.38 __isoc23_* symbols with current headers.
static const char *parse_num(const char *p, int base, unsigned long long *out)
{
    while (*p == ' ' || *p == '\t') ++p;
    unsigned long long v = 0; int any = 0;
    for (;; ++p) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = v * (unsigned)base + (unsigned)d; any = 1;
    }
    *out = v;
    return any ? p : NULL;
}

static void dump_epoll_regs(const char *why)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char lp[64], t[64];
        snprintf(lp, sizeof(lp), "/proc/self/fd/%.40s", e->d_name);
        ssize_t n = readlink(lp, t, sizeof(t) - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (strcmp(t, "anon_inode:[eventpoll]") != 0) continue;
        char ip[64];
        snprintf(ip, sizeof(ip), "/proc/self/fdinfo/%.40s", e->d_name);
        FILE *f = fopen(ip, "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long tfd = 0, ev = 0, data = 0, epfd_u = 0;
            const char *q;
            if (strncmp(line, "tfd:", 4) != 0 || !(q = parse_num(line + 4, 10, &tfd))) continue;
            q = strstr(q, "events:");
            if (q) q = parse_num(q + 7, 16, &ev);
            if (q) { q = strstr(q, "data:"); if (q) parse_num(q + 5, 16, &data); }
            if (tracked((int)tfd)) {
                parse_num(e->d_name, 10, &epfd_u);
                int epfd = (int)epfd_u;
                atomic_store(&g_dec_epfd, epfd);
                atomic_store(&g_dec_data, data);
                slog("epoll registration (%s): epfd=%d decoder fd=%d events=0x%llx data=0x%llx", why, epfd, (int)tfd, ev, data);
            }
        }
        fclose(f);
    }
    closedir(d);
}

static void log_fd_flags(int fd, const char *why)
{
    int fl = real_fcntl ? real_fcntl(fd, F_GETFL) : fcntl(fd, F_GETFL);
    slog("decoder fd %d flags (%s): 0x%x%s", fd, why, fl, (fl & O_NONBLOCK) ? " O_NONBLOCK" : " (blocking)");
}

static int hook_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    if (emu_is_fd(fd)) return emu_epoll_ctl(epfd, op, fd, ev);
    if (emu_is_cand(fd)) {
        if (op == EPOLL_CTL_DEL) g_cand.epfd = -1;
        else if (ev) { g_cand.epfd = epfd; g_cand.epdata = ev->data.u64; }
    }
    int r = real_epoll_ctl(epfd, op, fd, ev);
    int err = errno;
    if (tracked(fd)) {
        slog("epoll_ctl(epfd=%d, op=%d, decoder fd=%d, events=0x%x, data=0x%llx) -> %d", epfd, op, fd,
             ev ? ev->events : 0, ev ? (unsigned long long)ev->data.u64 : 0ull, r);
        if (r == 0 && op != EPOLL_CTL_DEL && ev) { atomic_store(&g_dec_epfd, epfd); atomic_store(&g_dec_data, ev->data.u64); }
    }
    errno = err;
    return r;
}

static int hook_epoll_wait(int epfd, struct epoll_event *evs, int max, int timeout)
{
    int r = real_epoll_wait(epfd, evs, max, timeout);
    if (g_emu.fd >= 0 && epfd == g_emu.epfd) {
        // Decoder entries carry our eventfd's readiness; translate to V4L2 semantics (and drop stale wake-ups).
        while (r > 0) {
            int n = emu_filter_events(evs, r);
            if (n > 0 || timeout == 0) { r = n; break; }
            r = real_epoll_wait(epfd, evs, max, timeout);   // nothing really ready: wait again
        }
    }
    int err = errno;
    if (epfd == atomic_load(&g_dec_epfd)) {
        atomic_fetch_add(&g_epw_calls, 1);
        unsigned long long dd = atomic_load(&g_dec_data);
        for (int i = 0; i < r; ++i)
            if (evs[i].data.u64 == dd && atomic_fetch_add(&g_epw_logged, 1) < 300)
                slog("epoll_wait(epfd=%d, max=%d, timeout=%d) -> %d; decoder events=0x%x", epfd, max, timeout, r, evs[i].events);
        if (r == 0 && atomic_load(&g_epw_logged) < 300 && atomic_fetch_add(&g_epw_logged, 1) < 300)
            slog("epoll_wait(epfd=%d, timeout=%d) -> 0 (timeout)", epfd, timeout);
    }
    errno = err;
    return r;
}

static int hook_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    int r = real_poll(fds, n, timeout);
    int err = errno;
    for (nfds_t i = 0; i < n; ++i)
        if (tracked(fds[i].fd) && atomic_fetch_add(&g_poll_logged, 1) < 300) {
            char buf[256]; int pos = 0;
            for (nfds_t k = 0; k < n && pos < 200; ++k)
                pos += snprintf(buf + pos, sizeof(buf) - pos, " [fd %d ev 0x%x rev 0x%x]", fds[k].fd, fds[k].events, fds[k].revents);
            slog("poll(n=%lu, timeout=%d) -> %d:%s", (unsigned long)n, timeout, r, buf);
            break;
        }
    errno = err;
    return r;
}

static int hook_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long a = va_arg(ap, long);
    va_end(ap);
    int r = real_fcntl(fd, cmd, a);
    int err = errno;
    if (tracked(fd) && cmd == F_SETFL) slog("fcntl(decoder fd %d, F_SETFL, 0x%lx) -> %d", fd, a, r);
    errno = err;
    return r;
}

// ---- v7.20: adopting a decoder fd that vrlink opened before this layer was loaded ----
// vrlink sometimes opens /dev/video22 (and even starts streaming) before its first vkCreateInstance loads us. v7.19
// only watched decoders opened later, so the first Pyrowave frames of such a session went to the HEVC hardware,
// which then wedged ("connected, no video" until SteamVR was restarted). The setup we missed is read back from the
// driver (G_FMT, QUERYBUF) and the OUTPUT buffers are mapped by us; these mappings are ours to undo.
static void *g_own_map[EMU_NBUF];
static size_t g_own_len[EMU_NBUF];
static int g_own_fd = -1;
static int g_preopen_fd = -1;
static void own_unmap(void)
{
    for (int i = 0; i < EMU_NBUF; ++i)
        if (g_own_map[i]) { munmap(g_own_map[i], g_own_len[i]); g_own_map[i] = NULL; g_own_len[i] = 0; }
    g_own_fd = -1;
}
static void emu_adopt_existing(int fd)
{
    if (fd < 0 || !emu_armed() || g_emu.fd >= 0 || g_cand.fd >= 0 || !real_ioctl) return;
    emu_cand_reset(fd);
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (real_ioctl(fd, VIDIOC_G_FMT, &f) == 0) {
        g_cand.out_w = f.fmt.pix_mp.width; g_cand.out_h = f.fmt.pix_mp.height; g_cand.fourcc = f.fmt.pix_mp.pixelformat;
        if (f.fmt.pix_mp.plane_fmt[0].sizeimage) g_cand.sizeimage = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    }
    int n = 0, mapped = 0;
    for (int i = 0; i < EMU_NBUF; ++i) {
        struct v4l2_buffer b;
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (uint32_t)i;
        b.m.planes = pl;
        b.length = VIDEO_MAX_PLANES;
        if (real_ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) break;
        n++;
        g_cand.out_off[i] = pl[0].m.mem_offset;
        void *p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)pl[0].m.mem_offset);
        if (p != MAP_FAILED) {
            g_own_map[i] = p;
            g_own_len[i] = pl[0].length;
            emu_cand_note_mmap((off_t)pl[0].m.mem_offset, p, pl[0].length);
            mapped++;
        }
    }
    g_own_fd = mapped ? fd : -1;
    g_cand.out_count = (uint32_t)n;
    g_cand.out_streaming = n > 0;   // vrlink streams right after setting its buffers up; a later STREAMON/OFF corrects it
    g_cand.subscribed = 1;          // vrlink always subscribes to SOURCE_CHANGE
    int epfd = atomic_load(&g_dec_epfd);
    if (epfd >= 0) { g_cand.epfd = epfd; g_cand.epdata = atomic_load(&g_dec_data); }
    slog("EMU: adopted decoder fd %d (opened before the layer loaded): OUTPUT %ux%u, %d buffer(s), %d mapped by the layer, "
         "epoll %d; SLPW -> Pyrowave takeover, else stock HEVC", fd, g_cand.out_w, g_cand.out_h, n, mapped, g_cand.epfd);
}

static int hook_close(int fd)
{
    if (emu_is_fd(fd)) { emu_close(fd); untrack(fd); if (fd == g_own_fd) own_unmap(); return real_close(fd); }
    if (fd == g_own_fd) own_unmap();
    if (emu_is_cand(fd)) g_cand.fd = -1;
    if (tracked(fd)) { slog("close(%d) [video]", fd); untrack(fd); }
    return real_close(fd);
}

// ---------------------------------------------------------------------------
// GOT patching of the main executable (vrlink) only
// ---------------------------------------------------------------------------

struct hook_def { const char *name; void *hook; void **real; };
static struct hook_def g_hooks[] = {
    {"open",   (void *)hook_open,   (void **)&real_open},
    {"ioctl",  (void *)hook_ioctl,  (void **)&real_ioctl},
    {"mmap",   (void *)hook_mmap,   (void **)&real_mmap},
    {"munmap", (void *)hook_munmap, (void **)&real_munmap},
    {"close",  (void *)hook_close,  (void **)&real_close},
    {"epoll_ctl",  (void *)hook_epoll_ctl,  (void **)&real_epoll_ctl},
    {"epoll_wait", (void *)hook_epoll_wait, (void **)&real_epoll_wait},
    {"poll",       (void *)hook_poll,       (void **)&real_poll},
    {"fcntl",      (void *)hook_fcntl,      (void **)&real_fcntl},
};
#define NHOOKS (sizeof(g_hooks) / sizeof(g_hooks[0]))

static int patch_main_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    int *patched = data;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;   // first entry = main program
    ElfW(Addr) base = info->dlpi_addr;
    ElfW(Dyn) *dyn = NULL;
    ElfW(Addr) relro_lo = 0, relro_hi = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_DYNAMIC) dyn = (ElfW(Dyn) *)(base + ph->p_vaddr);
        if (ph->p_type == PT_GNU_RELRO) { relro_lo = base + ph->p_vaddr; relro_hi = relro_lo + ph->p_memsz; }
    }
    if (!dyn) return 1;
    ElfW(Sym) *symtab = NULL; const char *strtab = NULL;
    ElfW(Rela) *jmprel = NULL, *rela = NULL; size_t pltsz = 0, relasz = 0;
    for (ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
        ElfW(Addr) v = d->d_un.d_ptr;
        if (v && v < base) v += base;          // in case the loader did not relocate d_ptr
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = (ElfW(Sym) *)v; break;
        case DT_STRTAB: strtab = (const char *)v; break;
        case DT_JMPREL: jmprel = (ElfW(Rela) *)v; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELA: rela = (ElfW(Rela) *)v; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab) return 1;
    long pg = sysconf(_SC_PAGESIZE);
    for (int pass = 0; pass < 2; ++pass) {
        ElfW(Rela) *r = pass ? rela : jmprel;
        size_t n = (pass ? relasz : pltsz) / sizeof(ElfW(Rela));
        for (size_t i = 0; r && i < n; ++i) {
            uint32_t type = ELF64_R_TYPE(r[i].r_info);
            if (type != SLP_R_JUMP_SLOT && type != SLP_R_GLOB_DAT) continue;
            const char *nm = strtab + symtab[ELF64_R_SYM(r[i].r_info)].st_name;
            for (size_t h = 0; h < NHOOKS; ++h) {
                if (strcmp(nm, g_hooks[h].name) != 0) continue;
                void **slot = (void **)(base + r[i].r_offset);
                if (*slot == g_hooks[h].hook) break;
                void *page = (void *)((uintptr_t)slot & ~(uintptr_t)(pg - 1));
                int in_relro = (uintptr_t)slot >= relro_lo && (uintptr_t)slot < relro_hi;
                if (mprotect(page, (size_t)pg, PROT_READ | PROT_WRITE) != 0) {
                    slog("mprotect failed for %s: %s", nm, strerror(errno));
                    break;
                }
                // The real function comes from dlsym, not from the slot: with lazy binding the
                // slot may still point at the PLT resolver stub.
                __atomic_store_n(slot, g_hooks[h].hook, __ATOMIC_SEQ_CST);
                if (in_relro) mprotect(page, (size_t)pg, PROT_READ);
                (*patched)++;
                slog("patched vrlink GOT: %s (%s, %s)", nm, type == SLP_R_JUMP_SLOT ? "JUMP_SLOT" : "GLOB_DAT",
                     in_relro ? "relro" : "rw");
                break;
            }
        }
    }
    return 1;
}

static void log_existing_video_fds(void)
{
    char p[64], t[256];
    for (int fd = 0; fd < 1024; ++fd) {
        snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(p, t, sizeof(t) - 1);
        if (n <= 0) continue;
        t[n] = 0;
        if (strncmp(t, "/dev/video", 10) == 0) {
            track(fd, t);
            snprintf(g_decoder_path, sizeof(g_decoder_path), "%.63s", t);
            slog("NOTE: %s was already open as fd %d before the layer loaded (setup calls missed)", t, fd);
            g_preopen_fd = fd;
            log_fd_flags(fd, "layer load");
            dump_epoll_regs("layer load");
        }
    }
}

static void install_hooks(void)
{
    static int done;
    if (done) return;
    done = 1;
    open_log();
    char exe[256] = {0};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) < 0) exe[0] = 0;
    slog("slp layer v7.22 loaded into pid %d (%s), mode %d; decoder stand-in %s", (int)getpid(), exe, g_active,
         emu_armed() ? "ARMED (auto-detect: Pyrowave streams decoded by Pyrowave, HEVC streams by the hardware)" : "not armed (observe-only)");
    if (g_active != 1) {
        slog("compositor mode: %s%s, no GOT patching", g_cfc ? "FULL COLOUR (shared-image swap of the stream views)" : "",
             g_cp_log ? (g_cfc ? " + Vulkan logging" : "Vulkan logging only") : "");
        return;
    }
    for (size_t h = 0; h < NHOOKS; ++h) {
        // SLP_TEST_FAKE_DRIVER=1 (host tests only): resolve through the global scope so an LD_PRELOADed
        // fake V4L2 driver is used; on the Frame RTLD_NEXT is always libc.
        static int test_global = -1;
        if (test_global < 0) { const char *e = getenv("SLP_TEST_FAKE_DRIVER"); test_global = e && e[0] == '1'; }
        *g_hooks[h].real = test_global ? NULL : dlsym(RTLD_NEXT, g_hooks[h].name);
        if (!*g_hooks[h].real) *g_hooks[h].real = dlsym(RTLD_DEFAULT, g_hooks[h].name);
        if (!*g_hooks[h].real) { slog("dlsym(%s) failed; not patching anything", g_hooks[h].name); return; }
    }
    log_existing_video_fds();
    int patched = 0;
    dl_iterate_phdr(patch_main_cb, &patched);
    slog("GOT patch done: %d slot(s)", patched);
    if (g_preopen_fd >= 0 && patched) emu_adopt_existing(g_preopen_fd);   // v7.20
}

// Runs at dlopen time (inside vkCreateInstance).  Does nothing unless armed in vrlink.
__attribute__((constructor)) static void slp_ctor(void)
{
    if (is_active()) install_hooks();
}

// ---------------------------------------------------------------------------
// Minimal pass-through Vulkan layer
// ---------------------------------------------------------------------------

#define MAX_OBJ 16
static struct { VkInstance inst; PFN_vkGetInstanceProcAddr next_gipa; } g_inst[MAX_OBJ];
static struct { VkDevice dev; PFN_vkGetDeviceProcAddr next_gdpa; } g_dev[MAX_OBJ];
static pthread_mutex_t g_obj_mutex = PTHREAD_MUTEX_INITIALIZER;

static PFN_vkGetInstanceProcAddr inst_gipa(VkInstance i)
{
    PFN_vkGetInstanceProcAddr r = NULL;
    pthread_mutex_lock(&g_obj_mutex);
    for (int k = 0; k < MAX_OBJ; ++k) if (g_inst[k].inst == i) { r = g_inst[k].next_gipa; break; }
    if (!r) for (int k = 0; k < MAX_OBJ; ++k) if (g_inst[k].inst) { r = g_inst[k].next_gipa; break; }
    pthread_mutex_unlock(&g_obj_mutex);
    return r;
}
static PFN_vkGetDeviceProcAddr dev_gdpa(VkDevice d)
{
    PFN_vkGetDeviceProcAddr r = NULL;
    pthread_mutex_lock(&g_obj_mutex);
    for (int k = 0; k < MAX_OBJ; ++k) if (g_dev[k].dev == d) { r = g_dev[k].next_gdpa; break; }
    pthread_mutex_unlock(&g_obj_mutex);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkInstance *out)
{
    VkLayerInstanceCreateInfo *li = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerInstanceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_obj_mutex);
        for (int k = 0; k < MAX_OBJ; ++k) if (!g_inst[k].inst) { g_inst[k].inst = *out; g_inst[k].next_gipa = next; break; }
        pthread_mutex_unlock(&g_obj_mutex);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL slp_DestroyInstance(VkInstance i, const VkAllocationCallbacks *a)
{
    PFN_vkGetInstanceProcAddr next = inst_gipa(i);
    PFN_vkDestroyInstance d = next ? (PFN_vkDestroyInstance)next(i, "vkDestroyInstance") : NULL;
    pthread_mutex_lock(&g_obj_mutex);
    for (int k = 0; k < MAX_OBJ; ++k) if (g_inst[k].inst == i) { g_inst[k].inst = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_obj_mutex);
    if (d) d(i, a);
}


// One-shot capability report for the formats/modifiers the Pyrowave decoder may write.
static void log_format_caps(VkPhysicalDevice pd, PFN_vkGetInstanceProcAddr next_gipa)
{
    static int done;
    if (done) return;
    done = 1;
    VkInstance inst = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_obj_mutex);
    for (int k = 0; k < MAX_OBJ; ++k) if (g_inst[k].inst) { inst = g_inst[k].inst; break; }
    pthread_mutex_unlock(&g_obj_mutex);
    if (!inst) return;
    PFN_vkGetPhysicalDeviceFormatProperties2 fp2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)next_gipa(inst, "vkGetPhysicalDeviceFormatProperties2");
    PFN_vkGetPhysicalDeviceImageFormatProperties2 ifp2 =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)next_gipa(inst, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!fp2 || !ifp2) { slog("caps: physical-device query functions unavailable"); return; }
    static const struct { VkFormat f; const char *n; } fmts[] = {
        {VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, "G10X6_B10X6R10X6_2PLANE_420 (P010-like)"},
        {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "G8_B8R8_2PLANE_420 (NV12)"},
        {VK_FORMAT_R10X6_UNORM_PACK16, "R10X6"}, {VK_FORMAT_R10X6G10X6_UNORM_2PACK16, "R10X6G10X6"},
        {VK_FORMAT_R16_UNORM, "R16"}, {VK_FORMAT_R16G16_UNORM, "R16G16"}, {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
    };
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); ++i) {
        VkDrmFormatModifierPropertiesListEXT ml = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &ml};
        fp2(pd, fmts[i].f, &fp);
        slog("caps %s (fmt %d): optimal feats 0x%x linear 0x%x; %u DRM modifiers", fmts[i].n, fmts[i].f,
             fp.formatProperties.optimalTilingFeatures, fp.formatProperties.linearTilingFeatures, ml.drmFormatModifierCount);
        VkDrmFormatModifierPropertiesEXT mp[16];
        if (ml.drmFormatModifierCount > 16) ml.drmFormatModifierCount = 16;
        ml.pDrmFormatModifierProperties = mp;
        fp2(pd, fmts[i].f, &fp);
        for (uint32_t m = 0; m < ml.drmFormatModifierCount; ++m) {
            slog("    modifier 0x%016llx planes=%u feats=0x%x", (unsigned long long)mp[m].drmFormatModifier,
                 mp[m].drmFormatModifierPlaneCount, mp[m].drmFormatModifierTilingFeatures);
            static const struct { VkImageUsageFlags u; const char *n; } us[] = {
                {VK_IMAGE_USAGE_SAMPLED_BIT, "SAMPLED"}, {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "TRANSFER_DST"},
                {VK_IMAGE_USAGE_STORAGE_BIT, "STORAGE"}, {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "COLOR_ATT"}};
            char line[256]; int pos = 0;
            for (int f = 0; f < 2; ++f) for (size_t u = 0; u < 4; ++u) {
                VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
                mi.drmFormatModifier = mp[m].drmFormatModifier; mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &mi};
                fi.format = fmts[i].f; fi.type = VK_IMAGE_TYPE_2D; fi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                fi.usage = us[u].u;
                fi.flags = f ? (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) : 0;
                VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
                VkResult rr = ifp2(pd, &fi, &ip);
                pos += snprintf(line + pos, sizeof(line) - pos, "%s%s=%s ", f ? "mut:" : "", us[u].n, rr == VK_SUCCESS ? "ok" : "no");
            }
            slog("      %s", line);
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkDevice *out)
{
    VkLayerDeviceCreateInfo *li = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerDeviceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(pd, ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_obj_mutex);
        for (int k = 0; k < MAX_OBJ; ++k) if (!g_dev[k].dev) { g_dev[k].dev = *out; g_dev[k].next_gdpa = next_gdpa; break; }
        pthread_mutex_unlock(&g_obj_mutex);
        slog("vkCreateDevice ok (device %p)", (void *)*out);
        if (g_active == 1) log_format_caps(pd, next_gipa);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL slp_DestroyDevice(VkDevice d, const VkAllocationCallbacks *a)
{
    PFN_vkGetDeviceProcAddr next = dev_gdpa(d);
    PFN_vkDestroyDevice f = next ? (PFN_vkDestroyDevice)next(d, "vkDestroyDevice") : NULL;
    pthread_mutex_lock(&g_obj_mutex);
    for (int k = 0; k < MAX_OBJ; ++k) if (g_dev[k].dev == d) { g_dev[k].dev = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_obj_mutex);
    if (f) f(d, a);
}

// ---------------------------------------------------------------------------
// v1: observe how vrlink's Vulkan side creates / imports the decode surfaces.
// Logged only for "interesting" objects: external memory, DRM modifiers, YCbCr
// formats, or the 1344-wide stream size.  All calls pass straight through.
// ---------------------------------------------------------------------------

#define NEXT_DEV(dev, T, name) ((T)(dev_gdpa(dev) ? dev_gdpa(dev)(dev, name) : NULL))
static atomic_int g_vk_logged;
#define VK_LOG_CAP 400

static int vk_log_ok(void) { return atomic_fetch_add(&g_vk_logged, 1) < VK_LOG_CAP; }

static int is_ycbcr_format(VkFormat f)
{
    return (f >= VK_FORMAT_G8B8G8R8_422_UNORM && f <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
           (f >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM && f <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
}

#define CP_MAX 256
static uint64_t g_cp_samplers[CP_MAX], g_cp_views[CP_MAX], g_cp_dsl[CP_MAX], g_cp_pl[CP_MAX], g_cp_tmpl[CP_MAX];
static pthread_mutex_t g_cp_mutex = PTHREAD_MUTEX_INITIALIZER;
static void cp_track(uint64_t *arr, int n, uint64_t h)
{
    pthread_mutex_lock(&g_cp_mutex);
    for (int i = 0; i < n; ++i) if (!arr[i] || arr[i] == h) { arr[i] = h; break; }
    pthread_mutex_unlock(&g_cp_mutex);
}
static int cp_has(const uint64_t *arr, int n, uint64_t h)
{
    if (!h) return 0;
    int r = 0;
    pthread_mutex_lock(&g_cp_mutex);
    for (int i = 0; i < n && arr[i]; ++i) if (arr[i] == h) { r = 1; break; }
    pthread_mutex_unlock(&g_cp_mutex);
    return r;
}

#define MAX_TRACKED_IMG 64
static VkImage g_timg[MAX_TRACKED_IMG];
static VkFormat g_timg_fmt[MAX_TRACKED_IMG];
static uint32_t g_timg_w[MAX_TRACKED_IMG], g_timg_h[MAX_TRACKED_IMG];   // v7.19
static VkDeviceMemory g_surf_mem[MAX_TRACKED_IMG];   // v7.19b: vrlink's exportable stream surface memories
static pthread_mutex_t g_img_mutex = PTHREAD_MUTEX_INITIALIZER;
static void track_image(VkImage im, VkFormat f, uint32_t w, uint32_t h)
{
    pthread_mutex_lock(&g_img_mutex);
    // v7.19b: when full, the oldest entry is reused (vrlink never tells us about destroyed images)
    static unsigned next_slot;
    int k = -1;
    for (int i = 0; i < MAX_TRACKED_IMG && k < 0; ++i) if (!g_timg[i]) k = i;
    if (k < 0) k = (int)(next_slot++ % MAX_TRACKED_IMG);
    g_timg[k] = im; g_timg_fmt[k] = f; g_timg_w[k] = w; g_timg_h[k] = h;
    pthread_mutex_unlock(&g_img_mutex);
}
static void untrack_image(VkImage im)
{
    pthread_mutex_lock(&g_img_mutex);
    for (int i = 0; i < MAX_TRACKED_IMG; ++i) if (g_timg[i] == im) { g_timg[i] = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_img_mutex);
}
static int image_tracked(VkImage im)
{
    int r = 0;
    pthread_mutex_lock(&g_img_mutex);
    for (int i = 0; i < MAX_TRACKED_IMG; ++i) if (g_timg[i] == im) { r = 1; break; }
    pthread_mutex_unlock(&g_img_mutex);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateImage(VkDevice d, const VkImageCreateInfo *ci,
                                                       const VkAllocationCallbacks *a, VkImage *out)
{
    PFN_vkCreateImage next = NEXT_DEV(d, PFN_vkCreateImage, "vkCreateImage");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (!ci) return r;
    uint32_t ext_types = 0; int nmods = -1; uint64_t mod0 = 0, explicit_mod = 0; int has_explicit = 0;
    uint32_t nplanes = 0; VkSubresourceLayout pl[4]; uint32_t nviewfmts = 0; VkFormat vf0 = 0;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        switch (p->sType) {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            ext_types = ((const VkExternalMemoryImageCreateInfo *)p)->handleTypes; break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: {
            const VkImageDrmFormatModifierListCreateInfoEXT *m = (const void *)p;
            nmods = (int)m->drmFormatModifierCount; mod0 = nmods > 0 ? m->pDrmFormatModifiers[0] : 0; break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: {
            const VkImageDrmFormatModifierExplicitCreateInfoEXT *m = (const void *)p;
            has_explicit = 1; explicit_mod = m->drmFormatModifier; nplanes = m->drmFormatModifierPlaneCount;
            for (uint32_t i = 0; i < nplanes && i < 4; ++i) pl[i] = m->pPlaneLayouts[i];
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *m = (const void *)p;
            nviewfmts = m->viewFormatCount; vf0 = nviewfmts ? m->pViewFormats[0] : 0; break;
        }
        default: break;
        }
    }
    int interesting = ext_types || nmods >= 0 || has_explicit || is_ycbcr_format(ci->format) || ci->extent.width == 1344 ||
                      ci->extent.width == g_cl_want_w;
    // Compositor mode: only stream-sized surfaces (the decode images are 1344x5376), so the log cap is not
    // spent on overlay / UI / camera textures.
    // v7.18: any stream size (the stream is 4 tiles tall: width x 4*width, e.g. 1152x4608), YCbCr surfaces only
    if (g_active == 2) interesting = ci->extent.width >= 512 && ci->extent.height == 4 * ci->extent.width &&
                                     is_ycbcr_format(ci->format);
    if (interesting && r == VK_SUCCESS && out) track_image(*out, ci->format, ci->extent.width, ci->extent.height);
    if (g_active == 1 && r == VK_SUCCESS && has_explicit && nplanes == 2 &&
        ci->format == (EMU_BITS == 8 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) &&
        ci->extent.width == g_cl_want_w) {
        g_cl_mod = explicit_mod;
        g_cl_off[0] = pl[0].offset; g_cl_off[1] = pl[1].offset;
        g_cl_pitch[0] = pl[0].rowPitch; g_cl_pitch[1] = pl[1].rowPitch;
        g_cl_seen = 1;
    }
    if (interesting && vk_log_ok()) {
        slog("vkCreateImage %ux%u fmt=%d tiling=%d usage=0x%x flags=0x%x mips=%u layers=%u ext_handles=0x%x "
             "modlist=%d(first 0x%llx) explicit_mod=%s0x%llx planes=%u viewfmts=%u(first %d) -> %d image=%p",
             ci->extent.width, ci->extent.height, ci->format, ci->tiling, ci->usage, ci->flags, ci->mipLevels,
             ci->arrayLayers, ext_types, nmods, (unsigned long long)mod0, has_explicit ? "" : "(none)",
             (unsigned long long)explicit_mod, nplanes, nviewfmts, vf0, r, out ? (void *)*out : NULL);
        for (uint32_t i = 0; i < nplanes && i < 4; ++i)
            slog("    plane %u: offset=%llu size=%llu rowPitch=%llu", i, (unsigned long long)pl[i].offset,
                 (unsigned long long)pl[i].size, (unsigned long long)pl[i].rowPitch);
    }
    return r;
}

static void cfc_note_import(VkDevice d, VkImage surf, uint64_t ino);   // v7.19, below
static VKAPI_ATTR VkResult VKAPI_CALL slp_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai,
                                                          const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
    PFN_vkAllocateMemory next = NEXT_DEV(d, PFN_vkAllocateMemory, "vkAllocateMemory");
    // Read the import fd BEFORE the call: a successful import transfers ownership.
    int imp_fd = -1; uint32_t imp_type = 0, exp_types = 0; VkImage ded_img = VK_NULL_HANDLE;
    for (const VkBaseInStructure *p = ai ? ai->pNext : NULL; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            imp_fd = ((const VkImportMemoryFdInfoKHR *)p)->fd; imp_type = ((const VkImportMemoryFdInfoKHR *)p)->handleType;
        } else if (p->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            exp_types = ((const VkExportMemoryAllocateInfo *)p)->handleTypes;
        } else if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            ded_img = ((const VkMemoryDedicatedAllocateInfo *)p)->image;
        }
    }
    // v7.19 full colour: the inode of an imported stream surface, read before the import consumes the fd
    uint64_t cfc_ino = (g_active == 2 && g_cfc && imp_fd >= 0 && imp_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
                        ded_img && image_tracked(ded_img)) ? fd_inode(imp_fd) : 0;
    VkResult r = next ? next(d, ai, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (cfc_ino && r == VK_SUCCESS) cfc_note_import(d, ded_img, cfc_ino);
    // v7.19b: vrlink's own exportable stream surfaces (dedicated, width x 4*width NV12): remembered so their dma-buf
    // can be registered for full colour when vrlink exports it
    if (g_active == 1 && r == VK_SUCCESS && out && (exp_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) && ded_img) {
        int ok = 0;
        pthread_mutex_lock(&g_img_mutex);
        for (int i = 0; i < MAX_TRACKED_IMG; ++i)
            if (g_timg[i] == ded_img && g_timg_fmt[i] == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && g_timg_w[i] >= 256 &&
                g_timg_h[i] == 4 * g_timg_w[i]) ok = 1;
        static unsigned next_mem;
        if (ok) g_surf_mem[next_mem++ % MAX_TRACKED_IMG] = *out;   // ring: old sessions' entries are overwritten
        pthread_mutex_unlock(&g_img_mutex);
    }
    if (g_active == 2 && !(ded_img && image_tracked(ded_img))) return r;
    if ((imp_type || exp_types) && ai && vk_log_ok())
        slog("vkAllocateMemory size=%llu type=%u import(fd=%d type=0x%x) export=0x%x dedicated_image=%p -> %d mem=%p",
             (unsigned long long)ai->allocationSize, ai->memoryTypeIndex, imp_fd, imp_type, exp_types, (void *)ded_img, r,
             out ? (void *)*out : NULL);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_GetMemoryFdKHR(VkDevice d, const VkMemoryGetFdInfoKHR *gi, int *fd)
{
    PFN_vkGetMemoryFdKHR next = NEXT_DEV(d, PFN_vkGetMemoryFdKHR, "vkGetMemoryFdKHR");
    VkResult r = next ? next(d, gi, fd) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active == 1 && g_fc && r == VK_SUCCESS && gi && fd && *fd >= 0 &&
        gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        int ok = 0;
        pthread_mutex_lock(&g_img_mutex);
        for (int i = 0; i < MAX_TRACKED_IMG; ++i) if (g_surf_mem[i] == gi->memory && gi->memory) ok = 1;
        pthread_mutex_unlock(&g_img_mutex);
        if (ok) fc_note_export(*fd);   // v7.19b: register for full colour before vrcompositor asks
    }
    if (g_active != 2 && vk_log_ok())
        slog("vkGetMemoryFdKHR mem=%p type=0x%x -> %d fd=%d", gi ? (void *)gi->memory : NULL, gi ? gi->handleType : 0, r,
             fd ? *fd : -1);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_GetImageDrmFormatModifierPropertiesEXT(VkDevice d, VkImage img,
                                                                                  VkImageDrmFormatModifierPropertiesEXT *pr)
{
    PFN_vkGetImageDrmFormatModifierPropertiesEXT next =
        NEXT_DEV(d, PFN_vkGetImageDrmFormatModifierPropertiesEXT, "vkGetImageDrmFormatModifierPropertiesEXT");
    VkResult r = next ? next(d, img, pr) : VK_ERROR_INITIALIZATION_FAILED;
    if (vk_log_ok()) slog("vkGetImageDrmFormatModifierPropertiesEXT image=%p -> %d modifier=0x%llx", (void *)img, r,
                          pr ? (unsigned long long)pr->drmFormatModifier : 0ull);
    return r;
}

static void log_ycbcr(const char *fn, const VkSamplerYcbcrConversionCreateInfo *ci, VkResult r)
{
    if (!ci || !vk_log_ok()) return;
    slog("%s fmt=%d model=%d range=%d swizzle=%d,%d,%d,%d xoff=%d yoff=%d filter=%d forceExplicit=%u -> %d", fn, ci->format,
         ci->ycbcrModel, ci->ycbcrRange, ci->components.r, ci->components.g, ci->components.b, ci->components.a,
         ci->xChromaOffset, ci->yChromaOffset, ci->chromaFilter, ci->forceExplicitReconstruction, r);
}
static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateSamplerYcbcrConversion(VkDevice d, const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                        const VkAllocationCallbacks *a,
                                                                        VkSamplerYcbcrConversion *out)
{
    PFN_vkCreateSamplerYcbcrConversion next = NEXT_DEV(d, PFN_vkCreateSamplerYcbcrConversion, "vkCreateSamplerYcbcrConversion");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    log_ycbcr("vkCreateSamplerYcbcrConversion", ci, r);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateSamplerYcbcrConversionKHR(VkDevice d,
                                                                           const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                           const VkAllocationCallbacks *a,
                                                                           VkSamplerYcbcrConversion *out)
{
    PFN_vkCreateSamplerYcbcrConversionKHR next =
        NEXT_DEV(d, PFN_vkCreateSamplerYcbcrConversionKHR, "vkCreateSamplerYcbcrConversionKHR");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    log_ycbcr("vkCreateSamplerYcbcrConversionKHR", ci, r);
    return r;
}

// ---------------------------------------------------------------------------
// v7.19 full colour, vrcompositor side (armed by ~/.local/lib/slp/FULLCOLOUR or /tmp/slp_fullcolour).
// For each stream surface vrcompositor imports (dedicated dma-buf import of a width x 4*width NV12 image), we ask
// vrlink's layer for the shared 10-bit RGBA image of that surface (R = Cr, G = Y, B = Cb), import it on
// vrcompositor's device, and hand out a plain view of it where vrcompositor creates its YCbCr view of the surface.
// The shader reads the same (Cr, Y, Cb) the identity conversion returned, so its colour matrix is unchanged; the
// chroma is now full resolution when the stream is 4:4:4. Barriers vrcompositor records on the surface are mirrored
// onto our image (same layouts, same foreign-queue acquire). Any failure keeps vrcompositor's own NV12 view.
// ---------------------------------------------------------------------------
#define CFC_MAX 32
typedef struct {
    VkDevice dev;
    VkImage surf, img;
    VkDeviceMemory mem;
    uint64_t ino;
    uint32_t w, h;
    int tried, acked;
} cfc_entry;
static cfc_entry g_cfc_tab[CFC_MAX];
static atomic_int g_cfc_live;         // acked entries (barrier fast path)
static pthread_mutex_t g_cfc_mu = PTHREAD_MUTEX_INITIALIZER;        // the table only (held briefly; barriers use it)
static pthread_mutex_t g_cfc_sock_mu = PTHREAD_MUTEX_INITIALIZER;   // the socket (requests can take a while)
static int g_cfc_sock = -1;
static PFN_vkCmdPipelineBarrier g_next_barrier;
static PFN_vkCmdPipelineBarrier2 g_next_barrier2, g_next_barrier2khr;

static int cfc_connect_locked(void)
{
    if (g_cfc_sock >= 0) return g_cfc_sock;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    socklen_t al;
    fc_sock_name(&a, &al);
    if (connect(s, (struct sockaddr *)&a, al) != 0) {
        static int logged;
        if (logged++ < 3) slog("FULLCOLOUR: vrlink's handover socket is not there (%s): is full colour armed for vrlink too, and a "
                               "stream running? NV12 kept", strerror(errno));
        close(s);
        return -1;
    }
    struct timeval tv = {0, 600 * 1000};   // vrlink waits at most 500 ms itself
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    g_cfc_sock = s;
    slog("FULLCOLOUR: connected to vrlink");
    return s;
}

// One request / reply (caller holds g_cfc_sock_mu). fd_out receives the image fd for a GET (or -1). Returns the reply
// status, or -100 on a transport error (the socket is closed and reopened on the next request).
static int cfc_request_locked(uint32_t op, uint64_t ino, fc_rep *rep, int *fd_out)
{
    if (fd_out) *fd_out = -1;
    int s = cfc_connect_locked();
    if (s < 0) return -100;
    fc_req q = {FC_MAGIC, op, ino};
    if (send(s, &q, sizeof(q), MSG_NOSIGNAL) != (ssize_t)sizeof(q)) goto broken;
    struct iovec iov = {rep, sizeof(*rep)};
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u;
    memset(&u, 0, sizeof(u));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = u.buf;
    mh.msg_controllen = sizeof(u.buf);
    ssize_t r = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
    int fd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len >= CMSG_LEN(sizeof(int)))
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    if (r != (ssize_t)sizeof(*rep) || rep->magic != FC_MAGIC) { if (fd >= 0) close(fd); goto broken; }
    if (fd_out) *fd_out = fd;
    else if (fd >= 0) close(fd);
    return rep->status;
broken:
    slog("FULLCOLOUR: handover socket failed (%s); reconnecting next time", strerror(errno));
    close(s);
    g_cfc_sock = -1;
    return -100;
}
static int cfc_request(uint32_t op, uint64_t ino, fc_rep *rep, int *fd_out)
{
    pthread_mutex_lock(&g_cfc_sock_mu);
    int st = cfc_request_locked(op, ino, rep, fd_out);
    pthread_mutex_unlock(&g_cfc_sock_mu);
    return st;
}

static cfc_entry *cfc_find_locked(VkImage surf)
{
    for (int i = 0; i < CFC_MAX; ++i) if (g_cfc_tab[i].surf == surf && surf) return &g_cfc_tab[i];
    return NULL;
}

// vkAllocateMemory imported a stream surface (dedicated): remember its dma-buf inode.
static void cfc_note_import(VkDevice d, VkImage surf, uint64_t ino)
{
    if (!ino) return;
    uint32_t w = 0, h = 0;
    pthread_mutex_lock(&g_img_mutex);
    for (int i = 0; i < MAX_TRACKED_IMG; ++i) if (g_timg[i] == surf) { w = g_timg_w[i]; h = g_timg_h[i]; break; }
    pthread_mutex_unlock(&g_img_mutex);
    pthread_mutex_lock(&g_cfc_mu);
    cfc_entry *e = cfc_find_locked(surf);
    for (int i = 0; !e && i < CFC_MAX; ++i) if (!g_cfc_tab[i].surf) e = &g_cfc_tab[i];
    if (e) {
        memset(e, 0, sizeof(*e));
        e->dev = d; e->surf = surf; e->ino = ino; e->w = w; e->h = h;
    }
    pthread_mutex_unlock(&g_cfc_mu);
    if (!e) slog("FULLCOLOUR: surface table full; surface %p keeps NV12", (void *)surf);
}

static void cfc_free(VkDevice d, VkImage img, VkDeviceMemory mem)
{
    if (img) {
        PFN_vkDestroyImage di = NEXT_DEV(d, PFN_vkDestroyImage, "vkDestroyImage");
        if (di) di(d, img, NULL);
    }
    if (mem) {
        PFN_vkFreeMemory fm = NEXT_DEV(d, PFN_vkFreeMemory, "vkFreeMemory");
        if (fm) fm(d, mem, NULL);
    }
}

// GET + import of the shared image into vrcompositor's device (no table lock held: this talks to vrlink).
static int cfc_import(VkDevice d, VkImage surf, uint64_t ino, uint32_t sw, uint32_t sh, VkImage *img_out, VkDeviceMemory *mem_out)
{
    *img_out = VK_NULL_HANDLE;
    *mem_out = VK_NULL_HANDLE;
    fc_rep rep;
    int fd = -1;
    double t0 = mono_ms();
    int st = cfc_request(FC_GET, ino, &rep, &fd);
    if (st != FC_OK || fd < 0) {
        static int logged;
        if (logged++ < 20)
            slog("FULLCOLOUR: surface %p: no shared image (%s, %.1f ms); NV12 kept", (void *)surf,
                 st == FC_UNKNOWN ? "vrlink does not know this surface" : st == FC_NOT_READY ? "not ready in time"
                 : st == FC_OFF ? "full colour off in vrlink" : st == FC_EXPORT_FAILED ? "export failed" : "no connection",
                 mono_ms() - t0);
        if (fd >= 0) close(fd);
        return 0;
    }
    const char *why = NULL;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    PFN_vkCreateImage ci_ = NEXT_DEV(d, PFN_vkCreateImage, "vkCreateImage");
    PFN_vkGetImageMemoryRequirements gmr = NEXT_DEV(d, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    PFN_vkGetMemoryFdPropertiesKHR gfp = NEXT_DEV(d, PFN_vkGetMemoryFdPropertiesKHR, "vkGetMemoryFdPropertiesKHR");
    PFN_vkAllocateMemory am = NEXT_DEV(d, PFN_vkAllocateMemory, "vkAllocateMemory");
    PFN_vkBindImageMemory bim = NEXT_DEV(d, PFN_vkBindImageMemory, "vkBindImageMemory");
    VkResult r = VK_SUCCESS;
    if (!ci_ || !gmr || !gfp || !am || !bim) why = "device functions missing";
    else if (rep.vk_format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 || (sw && (rep.width != sw || rep.height != sh)))
        why = "size / format does not match the surface";
    if (!why) {
        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkSubresourceLayout lay;
        memset(&lay, 0, sizeof(lay));
        lay.offset = rep.offset;
        lay.rowPitch = rep.row_pitch;
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        mod.pNext = &ext;
        mod.drmFormatModifier = rep.modifier;
        mod.drmFormatModifierPlaneCount = 1;
        mod.pPlaneLayouts = &lay;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.pNext = &mod;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        ii.extent.width = rep.width; ii.extent.height = rep.height; ii.extent.depth = 1;
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((r = ci_(d, &ii, NULL, &img)) != VK_SUCCESS) { img = VK_NULL_HANDLE; why = "vkCreateImage"; }
    }
    if (!why) {
        VkMemoryRequirements mr;
        gmr(d, img, &mr);
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        gfp(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
        uint32_t bits = mr.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
        uint32_t type = 0;
        while (type < 32 && !(bits & (1u << type))) type++;
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        imp.fd = fd;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.pNext = &imp;
        ded.image = img;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &ded;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (type >= 32) why = "no memory type";
        else if (rep.size && mr.size > rep.size) why = "image needs more memory than the shared buffer";
        else if ((r = am(d, &ai, NULL, &mem)) != VK_SUCCESS) { mem = VK_NULL_HANDLE; why = "vkAllocateMemory(import)"; }
        else fd = -1;   // consumed by the import
        if (!why && (r = bim(d, img, mem, 0)) != VK_SUCCESS) why = "vkBindImageMemory";
    }
    if (fd >= 0) close(fd);
    if (why) {
        slog("FULLCOLOUR: surface %p: importing the shared image failed (%s, %d); NV12 kept", (void *)surf, why, (int)r);
        cfc_free(d, img, mem);
        cfc_request(FC_NACK, ino, &rep, NULL);
        return 0;
    }
    static int logged_ok;
    if (logged_ok++ < 20)
        slog("FULLCOLOUR: surface %p: shared %ux%u 10-bit image imported (row pitch %llu) in %.1f ms", (void *)surf, rep.width,
             rep.height, (unsigned long long)rep.row_pitch, mono_ms() - t0);
    *img_out = img;
    *mem_out = mem;
    return 1;
}

// vkCreateImageView of a stream surface: returns 1 and a view of our image when swapped (then *res is its result).
static int cfc_view(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out, VkResult *res)
{
    if (!ci || !out || ci->viewType != VK_IMAGE_VIEW_TYPE_2D || ci->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
        ci->subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
        return 0;   // only the full (YCbCr-converted) view of an NV12 surface is replaced
    // 1. first view of this surface: fetch + import the shared image (outside the table lock)
    pthread_mutex_lock(&g_cfc_mu);
    cfc_entry *e = cfc_find_locked(ci->image);
    int need = e && !e->img && !e->tried;
    VkDevice ed = e ? e->dev : VK_NULL_HANDLE;
    uint64_t ino = e ? e->ino : 0;
    uint32_t sw = e ? e->w : 0, sh = e ? e->h : 0;
    if (need) e->tried = 1;
    pthread_mutex_unlock(&g_cfc_mu);
    if (!e) return 0;
    if (need) {
        VkImage img;
        VkDeviceMemory mem;
        if (cfc_import(ed, ci->image, ino, sw, sh, &img, &mem)) {
            pthread_mutex_lock(&g_cfc_mu);
            e = cfc_find_locked(ci->image);
            if (e && e->ino == ino && !e->img) { e->img = img; e->mem = mem; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE; }
            pthread_mutex_unlock(&g_cfc_mu);
            cfc_free(ed, img, mem);   // only if the surface went away meanwhile
        }
    }
    // 2. the view of our image
    pthread_mutex_lock(&g_cfc_mu);
    e = cfc_find_locked(ci->image);
    VkImage img = e && e->ino == ino ? e->img : VK_NULL_HANDLE;
    int ok = 0, send_ack = 0, send_nack = 0;
    if (img) {
        PFN_vkCreateImageView cv = NEXT_DEV(d, PFN_vkCreateImageView, "vkCreateImageView");
        VkImageViewCreateInfo v = *ci;
        v.pNext = NULL;   // no YCbCr conversion, no usage subset
        v.flags = 0;
        v.image = img;
        v.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        v.subresourceRange.baseMipLevel = 0; v.subresourceRange.levelCount = 1;
        v.subresourceRange.baseArrayLayer = 0; v.subresourceRange.layerCount = 1;
        VkResult r = cv ? cv(d, &v, a, out) : VK_ERROR_INITIALIZATION_FAILED;
        if (r == VK_SUCCESS) {
            ok = 1;
            *res = r;
            if (!e->acked) { e->acked = 1; atomic_fetch_add(&g_cfc_live, 1); send_ack = 1; }
        } else {
            slog("FULLCOLOUR: surface %p: view of the shared image failed (%d); NV12 kept", (void *)e->surf, (int)r);
            if (!e->acked) {   // no view of ours handed out yet: drop the image
                cfc_free(e->dev, e->img, e->mem);
                e->img = VK_NULL_HANDLE;
                e->mem = VK_NULL_HANDLE;
                send_nack = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_cfc_mu);
    fc_rep rep;
    if (send_ack) {
        cfc_request(FC_ACK, ino, &rep, NULL);
        static int logged;
        if (logged++ < 20) slog("FULLCOLOUR: surface %p: vrcompositor's view now shows the shared 10-bit image (view %p)",
                                (void *)ci->image, (void *)*out);
    }
    if (send_nack) cfc_request(FC_NACK, ino, &rep, NULL);
    return ok;
}

static VKAPI_ATTR void VKAPI_CALL slp_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a)
{
    if (g_cfc && img) {
        pthread_mutex_lock(&g_cfc_mu);
        cfc_entry *e = cfc_find_locked(img);
        cfc_entry copy;
        memset(&copy, 0, sizeof(copy));
        if (e) {
            copy = *e;
            if (e->acked) atomic_fetch_sub(&g_cfc_live, 1);
            memset(e, 0, sizeof(*e));
        }
        pthread_mutex_unlock(&g_cfc_mu);
        if (copy.surf) {
            // vrcompositor destroys its views first, so ours (of our image) are gone too
            cfc_free(copy.dev, copy.img, copy.mem);
            fc_rep rep;
            if (copy.img || copy.acked) cfc_request(FC_NACK, copy.ino, &rep, NULL);   // vrlink: back to NV12
        }
    }
    if (img) untrack_image(img);
    PFN_vkDestroyImage next = NEXT_DEV(d, PFN_vkDestroyImage, "vkDestroyImage");
    if (next) next(d, img, a);
}

static VkImage cfc_mirror_locked(VkImage surf)
{
    cfc_entry *e = cfc_find_locked(surf);
    return e && e->acked ? e->img : VK_NULL_HANDLE;
}

static VKAPI_ATTR void VKAPI_CALL slp_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                                         VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *pm,
                                                         uint32_t nb, const VkBufferMemoryBarrier *pb, uint32_t ni,
                                                         const VkImageMemoryBarrier *pi)
{
    if (atomic_load(&g_cfc_live) > 0 && ni && pi) {
        VkImageMemoryBarrier tmp[64];
        uint32_t n = 0;
        pthread_mutex_lock(&g_cfc_mu);
        for (uint32_t i = 0; i < ni && ni + n < 64; ++i) {
            VkImage m = cfc_mirror_locked(pi[i].image);
            if (m) {
                tmp[ni + n] = pi[i];
                tmp[ni + n].image = m;
                tmp[ni + n].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                n++;
            }
        }
        pthread_mutex_unlock(&g_cfc_mu);
        if (n) {
            memcpy(tmp, pi, sizeof(*pi) * ni);
            g_next_barrier(cb, ss, ds, df, nm, pm, nb, pb, ni + n, tmp);
            return;
        }
    }
    g_next_barrier(cb, ss, ds, df, nm, pm, nb, pb, ni, pi);
}

static void cfc_barrier2(PFN_vkCmdPipelineBarrier2 next, VkCommandBuffer cb, const VkDependencyInfo *di)
{
    if (atomic_load(&g_cfc_live) > 0 && di && di->imageMemoryBarrierCount && di->pImageMemoryBarriers) {
        const uint32_t ni = di->imageMemoryBarrierCount;
        VkImageMemoryBarrier2 tmp[64];
        uint32_t n = 0;
        pthread_mutex_lock(&g_cfc_mu);
        for (uint32_t i = 0; i < ni && ni + n < 64; ++i) {
            VkImage m = cfc_mirror_locked(di->pImageMemoryBarriers[i].image);
            if (m) {
                tmp[ni + n] = di->pImageMemoryBarriers[i];
                tmp[ni + n].pNext = NULL;
                tmp[ni + n].image = m;
                tmp[ni + n].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                n++;
            }
        }
        pthread_mutex_unlock(&g_cfc_mu);
        if (n) {
            memcpy(tmp, di->pImageMemoryBarriers, sizeof(*tmp) * ni);
            VkDependencyInfo d2 = *di;
            d2.imageMemoryBarrierCount = ni + n;
            d2.pImageMemoryBarriers = tmp;
            next(cb, &d2);
            return;
        }
    }
    next(cb, di);
}
static VKAPI_ATTR void VKAPI_CALL slp_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{
    cfc_barrier2(g_next_barrier2, cb, di);
}
static VKAPI_ATTR void VKAPI_CALL slp_CmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo *di)
{
    cfc_barrier2(g_next_barrier2khr, cb, di);
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci,
                                                           const VkAllocationCallbacks *a, VkImageView *out)
{
    PFN_vkCreateImageView next = NEXT_DEV(d, PFN_vkCreateImageView, "vkCreateImageView");
    VkResult r;
    if (g_active == 2 && g_cfc && ci && image_tracked(ci->image) && cfc_view(d, ci, a, out, &r)) {
        if (vk_log_ok())
            slog("vkCreateImageView image=%p (stream surface) -> full-colour view %p", (void *)ci->image, out ? (void *)*out : NULL);
        return r;
    }
    r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ci && (image_tracked(ci->image) || is_ycbcr_format(ci->format))) {
        VkSamplerYcbcrConversion conv = VK_NULL_HANDLE; uint32_t vusage = 0;
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
            if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) conv = ((const VkSamplerYcbcrConversionInfo *)p)->conversion;
            if (p->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) vusage = ((const VkImageViewUsageCreateInfo *)p)->usage;
        }
        if (g_active == 2 && r == VK_SUCCESS && out && image_tracked(ci->image)) cp_track(g_cp_views, CP_MAX, (uint64_t)*out);
        if (vk_log_ok())
            slog("vkCreateImageView image=%p tracked=%d viewType=%d fmt=%d aspect=0x%x mips=%u layers=%u ycbcrConv=%p viewUsage=0x%x -> %d",
                 (void *)ci->image, image_tracked(ci->image), ci->viewType, ci->format, ci->subresourceRange.aspectMask,
                 ci->subresourceRange.levelCount, ci->subresourceRange.layerCount, (void *)conv, vusage, r);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateSampler(VkDevice d, const VkSamplerCreateInfo *ci,
                                                         const VkAllocationCallbacks *a, VkSampler *out)
{
    PFN_vkCreateSampler next = NEXT_DEV(d, PFN_vkCreateSampler, "vkCreateSampler");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    for (const VkBaseInStructure *p = ci ? ci->pNext : NULL; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            if (g_active == 2 && r == VK_SUCCESS && out) cp_track(g_cp_samplers, CP_MAX, (uint64_t)*out);
            if (vk_log_ok())
                slog("vkCreateSampler with ycbcrConv=%p mag=%d min=%d addr=%d -> %d sampler=%p", (void *)((const VkSamplerYcbcrConversionInfo *)p)->conversion,
                     ci->magFilter, ci->minFilter, ci->addressModeU, r, out ? (void *)*out : NULL);
        }
    return r;
}

// ---------------------------------------------------------------------------
// v7.18 (4:4:4 phase 1, compositor mode only, read-only): how vrcompositor samples the stream surfaces.
// Descriptor set layouts with an immutable YCbCr sampler, the pipeline layouts / pipelines that use them (their
// SPIR-V is saved to /tmp/slp_layer/spv/ to read the colour matrix), and descriptor writes of stream views.
// Everything passes straight through.
// ---------------------------------------------------------------------------
// shader module copies (SPIR-V), so pipelines that sample the stream can be dumped
#define CP_MOD_MAX 512
static struct { uint64_t h; uint32_t *code; size_t size; } g_cp_mods[CP_MOD_MAX];
static size_t g_cp_mod_bytes;
static atomic_int g_cp_dumps, g_cp_writes;

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                                              const VkAllocationCallbacks *a, VkShaderModule *out)
{
    PFN_vkCreateShaderModule next = NEXT_DEV(d, PFN_vkCreateShaderModule, "vkCreateShaderModule");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active == 2 && r == VK_SUCCESS && out && ci && ci->codeSize && ci->pCode) {
        pthread_mutex_lock(&g_cp_mutex);
        if (g_cp_mod_bytes + ci->codeSize <= (8u << 20))
            for (int i = 0; i < CP_MOD_MAX; ++i) if (!g_cp_mods[i].h) {
                g_cp_mods[i].code = malloc(ci->codeSize);
                if (g_cp_mods[i].code) {
                    memcpy(g_cp_mods[i].code, ci->pCode, ci->codeSize);
                    g_cp_mods[i].size = ci->codeSize; g_cp_mods[i].h = (uint64_t)*out; g_cp_mod_bytes += ci->codeSize;
                }
                break;
            }
        pthread_mutex_unlock(&g_cp_mutex);
    }
    return r;
}

static void cp_dump_module(uint64_t mod, const char *tag, int stage)
{
    if (atomic_fetch_add(&g_cp_dumps, 1) >= 64) return;
    pthread_mutex_lock(&g_cp_mutex);
    for (int i = 0; i < CP_MOD_MAX; ++i) if (g_cp_mods[i].h == mod) {
        mkdir("/tmp/slp_layer", 0755); mkdir("/tmp/slp_layer/spv", 0755);
        char path[256];
        snprintf(path, sizeof(path), "/tmp/slp_layer/spv/%s_stage%x_%016llx.spv", tag, stage, (unsigned long long)mod);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_cp_mods[i].code, 1, g_cp_mods[i].size, f); fclose(f); }
        slog("    saved %s (%zu bytes)", path, g_cp_mods[i].size);
        break;
    }
    pthread_mutex_unlock(&g_cp_mutex);
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                     const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    PFN_vkCreateDescriptorSetLayout next = NEXT_DEV(d, PFN_vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active != 2 || r != VK_SUCCESS || !ci || !out) return r;
    int hit = 0;
    for (uint32_t i = 0; i < ci->bindingCount; ++i)
        if (ci->pBindings[i].pImmutableSamplers)
            for (uint32_t k = 0; k < ci->pBindings[i].descriptorCount; ++k)
                if (cp_has(g_cp_samplers, CP_MAX, (uint64_t)ci->pBindings[i].pImmutableSamplers[k])) hit = 1;
    if (!hit) return r;
    cp_track(g_cp_dsl, CP_MAX, (uint64_t)*out);
    if (vk_log_ok()) {
        slog("vkCreateDescriptorSetLayout with a YCbCr immutable sampler: flags=0x%x bindings=%u -> dsl=%p", ci->flags,
             ci->bindingCount, (void *)*out);
        for (uint32_t i = 0; i < ci->bindingCount && i < 16; ++i)
            slog("    binding %u type=%d count=%u stages=0x%x immutable=%s", ci->pBindings[i].binding, ci->pBindings[i].descriptorType,
                 ci->pBindings[i].descriptorCount, ci->pBindings[i].stageFlags, ci->pBindings[i].pImmutableSamplers ? "yes" : "no");
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipelineLayout *out)
{
    PFN_vkCreatePipelineLayout next = NEXT_DEV(d, PFN_vkCreatePipelineLayout, "vkCreatePipelineLayout");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active != 2 || r != VK_SUCCESS || !ci || !out) return r;
    for (uint32_t i = 0; i < ci->setLayoutCount; ++i)
        if (cp_has(g_cp_dsl, CP_MAX, (uint64_t)ci->pSetLayouts[i])) {
            cp_track(g_cp_pl, CP_MAX, (uint64_t)*out);
            if (vk_log_ok())
                slog("vkCreatePipelineLayout sets=%u (YCbCr set at index %u) push ranges=%u -> layout=%p", ci->setLayoutCount, i,
                     ci->pushConstantRangeCount, (void *)*out);
            break;
        }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                                   const VkGraphicsPipelineCreateInfo *ci,
                                                                   const VkAllocationCallbacks *a, VkPipeline *out)
{
    PFN_vkCreateGraphicsPipelines next = NEXT_DEV(d, PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    VkResult r = next ? next(d, c, n, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active != 2 || !ci) return r;
    for (uint32_t p = 0; p < n; ++p)
        if (cp_has(g_cp_pl, CP_MAX, (uint64_t)ci[p].layout) && vk_log_ok()) {
            slog("vkCreateGraphicsPipelines #%u with the YCbCr layout: stages=%u -> %d pipeline=%p", p, ci[p].stageCount, r,
                 out ? (void *)out[p] : NULL);
            for (uint32_t s2 = 0; s2 < ci[p].stageCount; ++s2) {
                const VkPipelineShaderStageCreateInfo *st = &ci[p].pStages[s2];
                slog("    stage 0x%x entry=%s module=%p spec_consts=%u", st->stage, st->pName ? st->pName : "?", (void *)st->module,
                     st->pSpecializationInfo ? st->pSpecializationInfo->mapEntryCount : 0);
                cp_dump_module((uint64_t)st->module, "gfx", st->stage);
            }
        }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                                  const VkComputePipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    PFN_vkCreateComputePipelines next = NEXT_DEV(d, PFN_vkCreateComputePipelines, "vkCreateComputePipelines");
    VkResult r = next ? next(d, c, n, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active != 2 || !ci) return r;
    for (uint32_t p = 0; p < n; ++p)
        if (cp_has(g_cp_pl, CP_MAX, (uint64_t)ci[p].layout) && vk_log_ok()) {
            slog("vkCreateComputePipelines #%u with the YCbCr layout: entry=%s module=%p -> %d", p,
                 ci[p].stage.pName ? ci[p].stage.pName : "?", (void *)ci[p].stage.module, r);
            cp_dump_module((uint64_t)ci[p].stage.module, "comp", ci[p].stage.stage);
        }
    return r;
}

static void cp_log_writes(const char *fn, uint32_t n, const VkWriteDescriptorSet *w)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (!w[i].pImageInfo) continue;
        for (uint32_t k = 0; k < w[i].descriptorCount; ++k)
            if (cp_has(g_cp_views, CP_MAX, (uint64_t)w[i].pImageInfo[k].imageView)) {
                if (atomic_fetch_add(&g_cp_writes, 1) < 48 && vk_log_ok())
                    slog("%s: stream view %p -> set=%p binding=%u element=%u type=%d layout=%d sampler=%p", fn,
                         (void *)w[i].pImageInfo[k].imageView, (void *)w[i].dstSet, w[i].dstBinding, w[i].dstArrayElement + k,
                         w[i].descriptorType, w[i].pImageInfo[k].imageLayout, (void *)w[i].pImageInfo[k].sampler);
                break;
            }
    }
}

static VKAPI_ATTR void VKAPI_CALL slp_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w,
                                                           uint32_t nc, const VkCopyDescriptorSet *c)
{
    PFN_vkUpdateDescriptorSets next = NEXT_DEV(d, PFN_vkUpdateDescriptorSets, "vkUpdateDescriptorSets");
    if (g_active == 2 && w) cp_log_writes("vkUpdateDescriptorSets", nw, w);
    if (next) next(d, nw, w, nc, c);
}

static VKAPI_ATTR VkResult VKAPI_CALL slp_CreateDescriptorUpdateTemplate(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                                          const VkAllocationCallbacks *a,
                                                                          VkDescriptorUpdateTemplate *out)
{
    PFN_vkCreateDescriptorUpdateTemplate next = NEXT_DEV(d, PFN_vkCreateDescriptorUpdateTemplate, "vkCreateDescriptorUpdateTemplate");
    VkResult r = next ? next(d, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (g_active == 2 && r == VK_SUCCESS && ci && out &&
        (cp_has(g_cp_dsl, CP_MAX, (uint64_t)ci->descriptorSetLayout) || cp_has(g_cp_pl, CP_MAX, (uint64_t)ci->pipelineLayout))) {
        cp_track(g_cp_tmpl, CP_MAX, (uint64_t)*out);
        if (vk_log_ok()) {
            slog("vkCreateDescriptorUpdateTemplate for the YCbCr layout: type=%d entries=%u set=%u -> %p", ci->templateType,
                 ci->descriptorUpdateEntryCount, ci->set, (void *)*out);
            for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount && i < 16; ++i)
                slog("    entry binding=%u element=%u count=%u type=%d offset=%zu stride=%zu", ci->pDescriptorUpdateEntries[i].dstBinding,
                     ci->pDescriptorUpdateEntries[i].dstArrayElement, ci->pDescriptorUpdateEntries[i].descriptorCount,
                     ci->pDescriptorUpdateEntries[i].descriptorType, ci->pDescriptorUpdateEntries[i].offset,
                     ci->pDescriptorUpdateEntries[i].stride);
        }
    }
    return r;
}

static atomic_int g_cp_tmpl_calls;
static VKAPI_ATTR void VKAPI_CALL slp_UpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet set,
                                                                      VkDescriptorUpdateTemplate t, const void *data)
{
    PFN_vkUpdateDescriptorSetWithTemplate next = NEXT_DEV(d, PFN_vkUpdateDescriptorSetWithTemplate, "vkUpdateDescriptorSetWithTemplate");
    if (g_active == 2 && cp_has(g_cp_tmpl, CP_MAX, (uint64_t)t) && atomic_fetch_add(&g_cp_tmpl_calls, 1) < 8 && vk_log_ok())
        slog("vkUpdateDescriptorSetWithTemplate with the YCbCr template %p -> set=%p", (void *)t, (void *)set);
    if (next) next(d, set, t, data);
}

// Device-level names this layer intercepts (others pass through untouched).
static PFN_vkVoidFunction slp_device_hook(const char *name)
{
    if (!strcmp(name, "vkCreateImageView")) return (PFN_vkVoidFunction)slp_CreateImageView;
    if (!strcmp(name, "vkCreateSampler")) return (PFN_vkVoidFunction)slp_CreateSampler;
    if (!strcmp(name, "vkCreateImage")) return (PFN_vkVoidFunction)slp_CreateImage;
    if (!strcmp(name, "vkAllocateMemory")) return (PFN_vkVoidFunction)slp_AllocateMemory;
    if (!strcmp(name, "vkGetMemoryFdKHR")) return (PFN_vkVoidFunction)slp_GetMemoryFdKHR;
    if (!strcmp(name, "vkGetImageDrmFormatModifierPropertiesEXT"))
        return (PFN_vkVoidFunction)slp_GetImageDrmFormatModifierPropertiesEXT;
    if (!strcmp(name, "vkCreateSamplerYcbcrConversion")) return (PFN_vkVoidFunction)slp_CreateSamplerYcbcrConversion;
    if (!strcmp(name, "vkCreateSamplerYcbcrConversionKHR")) return (PFN_vkVoidFunction)slp_CreateSamplerYcbcrConversionKHR;
    if (g_active == 2 && g_cfc) {   // v7.19 full colour
        if (!strcmp(name, "vkDestroyImage")) return (PFN_vkVoidFunction)slp_DestroyImage;
        if (!strcmp(name, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)slp_CmdPipelineBarrier;
        if (!strcmp(name, "vkCmdPipelineBarrier2")) return (PFN_vkVoidFunction)slp_CmdPipelineBarrier2;
        if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)slp_CmdPipelineBarrier2KHR;
    }
    if (g_active == 2 && g_cp_log) {   // v7.18: compositor-only, read-only logging for 4:4:4 phase 1
        if (!strcmp(name, "vkCreateShaderModule")) return (PFN_vkVoidFunction)slp_CreateShaderModule;
        if (!strcmp(name, "vkCreateDescriptorSetLayout")) return (PFN_vkVoidFunction)slp_CreateDescriptorSetLayout;
        if (!strcmp(name, "vkCreatePipelineLayout")) return (PFN_vkVoidFunction)slp_CreatePipelineLayout;
        if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)slp_CreateGraphicsPipelines;
        if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)slp_CreateComputePipelines;
        if (!strcmp(name, "vkUpdateDescriptorSets")) return (PFN_vkVoidFunction)slp_UpdateDescriptorSets;
        if (!strcmp(name, "vkCreateDescriptorUpdateTemplate") || !strcmp(name, "vkCreateDescriptorUpdateTemplateKHR"))
            return (PFN_vkVoidFunction)slp_CreateDescriptorUpdateTemplate;
        if (!strcmp(name, "vkUpdateDescriptorSetWithTemplate") || !strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR"))
            return (PFN_vkVoidFunction)slp_UpdateDescriptorSetWithTemplate;
        if ((!strcmp(name, "vkCmdPushDescriptorSetKHR") || !strcmp(name, "vkGetDescriptorEXT")) && vk_log_ok())
            slog("note: vrcompositor looked up %s (push descriptors / descriptor buffers; not hooked)", name);
    }
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL slp_GetDeviceProcAddr(VkDevice d, const char *name);

// v7.19: the next layer's command-buffer functions we wrap (the driver's entry points do not depend on the device)
static void slp_note_down(const char *name, PFN_vkVoidFunction down)
{
    if (!down) return;
    if (!strcmp(name, "vkCmdPipelineBarrier")) g_next_barrier = (PFN_vkCmdPipelineBarrier)down;
    else if (!strcmp(name, "vkCmdPipelineBarrier2")) g_next_barrier2 = (PFN_vkCmdPipelineBarrier2)down;
    else if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) g_next_barrier2khr = (PFN_vkCmdPipelineBarrier2)down;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL slp_GetInstanceProcAddr(VkInstance i, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)slp_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)slp_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)slp_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)slp_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)slp_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)slp_DestroyDevice;
    PFN_vkGetInstanceProcAddr next = inst_gipa(i);
    PFN_vkVoidFunction down = next ? next(i, name) : NULL;
    if (down) { PFN_vkVoidFunction h = slp_device_hook(name); if (h) { slp_note_down(name, down); return h; } }
    return down;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL slp_GetDeviceProcAddr(VkDevice d, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)slp_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)slp_DestroyDevice;
    PFN_vkGetDeviceProcAddr next = dev_gdpa(d);
    PFN_vkVoidFunction down = next ? next(d, name) : NULL;
    // Only wrap functions the driver actually provides (keeps NULL for unsupported ones).
    if (down) { PFN_vkVoidFunction h = slp_device_hook(name); if (h) { slp_note_down(name, down); return h; } }
    return down;
}

// The only exported symbol.  Returning an error makes the loader skip this layer
// (it then looks for an exported vkGetInstanceProcAddr, finds none, and moves on).
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p)
{
    if (!is_active() || g_internal) return VK_ERROR_INITIALIZATION_FAILED;   // never inside our own decoder's instance
    if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || p->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    p->loaderLayerInterfaceVersion = 2;
    p->pfnGetInstanceProcAddr = slp_GetInstanceProcAddr;
    p->pfnGetDeviceProcAddr = slp_GetDeviceProcAddr;
    p->pfnGetPhysicalDeviceProcAddr = NULL;
    install_hooks();   // normally already done by the constructor
    return VK_SUCCESS;
}
