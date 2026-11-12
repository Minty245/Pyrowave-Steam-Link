// Host test of v7.20 decoder-fd adoption: a stub driver answers G_FMT / QUERYBUF, a memfd stands in for the
// decoder's OUTPUT buffers. gcc -O1 -g -std=gnu11 -I$VK_INC adopt_test.c -o adopt_test -ldl -lpthread
#include "slp_layer.c"
#include <sys/mman.h>
static int s_fd;
static int stub_ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (fd != s_fd) { errno = EBADF; return -1; }
    if (req == VIDIOC_G_FMT) {
        struct v4l2_format *f = arg;
        f->fmt.pix_mp.width = 1536; f->fmt.pix_mp.height = 6144; f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
        f->fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
        return 0;
    }
    if (req == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *b = arg;
        if (b->index >= 16) { errno = EINVAL; return -1; }
        b->m.planes[0].m.mem_offset = b->index * 4096; b->m.planes[0].length = 4096;
        return 0;
    }
    if (req == VIDIOC_QBUF) return 0;
    errno = ENOTTY; return -1;
}
static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)
int main(void)
{
    setenv("HOME", "/nonexistent", 1);
    s_fd = memfd_create("dec", 0);
    if (ftruncate(s_fd, 16 * 4096) != 0) return 2;
    real_ioctl = (pfn_ioctl)stub_ioctl;
    real_close = close; real_mmap = mmap;
    FILE *a = fopen("/tmp/slp_decode", "w"); if (a) fclose(a);   // arm the stand-in for this test
    track(s_fd, "/dev/video22");
    snprintf(g_decoder_path, sizeof(g_decoder_path), "/dev/video22");
    atomic_store(&g_dec_epfd, 8); atomic_store(&g_dec_data, 0x2f);
    emu_adopt_existing(s_fd);
    CHECK(g_cand.fd == s_fd && g_cand.out_count == 16 && g_cand.out_w == 1536 && g_cand.epfd == 8 && g_cand.epdata == 0x2f,
          "adopted: fd, 16 buffers, format, epoll registration");
    CHECK(g_own_fd == s_fd && g_own_map[15] != NULL, "OUTPUT buffers mapped by the layer");
    // vrlink writes a non-SLPW frame into buffer 3 through its own mapping: same memory
    uint8_t *vr = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, 0);
    memcpy(vr + 3 * 4096, "\x53\x4c\x50\x57", 4);   // "SLPW"
    struct v4l2_plane pl[1] = {{0}};
    pl[0].bytesused = 300;
    struct v4l2_buffer b; memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.index = 3; b.m.planes = pl; b.length = 1;
    CHECK(emu_cand_qbuf_is_slpw(&b) == 1, "SLPW frame recognised through the layer's own mapping");
    memcpy(vr + 3 * 4096, "\x00\x00\x00\x01", 4);   // HEVC start code
    CHECK(emu_cand_qbuf_is_slpw(&b) == 0, "HEVC frame not taken");
    hook_ioctl(s_fd, VIDIOC_QBUF, &b);               // first frame HEVC -> stock decode, our mappings released
    CHECK(g_cand.fd == -1 && g_own_fd == -1 && g_own_map[0] == NULL, "HEVC session: candidate dropped, layer mappings released");
    // close path
    emu_adopt_existing(s_fd);
    CHECK(g_own_fd == s_fd, "re-adopted");
    hook_close(dup(s_fd));                           // another fd: untouched
    CHECK(g_own_fd == s_fd, "close of another fd leaves the mappings");
    int keep = dup(s_fd);
    hook_close(s_fd);
    CHECK(g_own_fd == -1 && g_own_map[0] == NULL && g_cand.fd == -1, "close of the decoder fd releases the mappings");
    close(keep);
    unlink("/tmp/slp_decode");
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
