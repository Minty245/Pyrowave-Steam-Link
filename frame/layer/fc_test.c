// Host test of the v7.19 full-colour handover (socket protocol, peer check, inode pairing, target states).
// Builds the real layer source with stubbed decoder calls: memfds stand in for the dma-bufs.
// gcc -O1 -g -I$VK_INC fc_test.c -o slp_fc_test -ldl -lpthread && SLP_FC_TEST=1 ./slp_fc_test
#include "slp_layer.c"
#include <sys/mman.h>
#include <assert.h>

static int t_out[64];
static int t_rgba_fd[64];
static int stub_full_colour(void *d) { (void)d; return 1; }
static int stub_rgba_export(void *d, int t, slpd_rgba_l *o, char *e, size_t el)
{
    (void)d; (void)e; (void)el;
    if (t < 0 || t >= 64 || t_rgba_fd[t] < 0) return -1;
    memset(o, 0, sizeof(*o));
    o->fd = dup(t_rgba_fd[t]);
    o->width = 768; o->height = 3072; o->vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    o->row_pitch = 3072; o->size = 768ull * 3072 * 4;
    return 0;
}
static void stub_set_out(void *d, int t, int o) { (void)d; if (t >= 0 && t < 64) t_out[t] = o; }
static int stub_get_out(void *d, int t) { (void)d; return t >= 0 && t < 64 ? t_out[t] : 0; }
static int stub_chroma(const void *f, size_t n) { (void)f; (void)n; return 0; }


// ---- fake vrcompositor device: the next layer's functions, recording what our compositor glue asks for ----
static VkImageCreateInfo s_ici; static VkImageDrmFormatModifierExplicitCreateInfoEXT s_mod; static VkSubresourceLayout s_lay;
static int s_imgs, s_import_fd = -1, s_dedicated_ok, s_destroyed, s_freed, s_views; static VkImageViewCreateInfo s_vci;
static VkResult VKAPI_CALL f_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *o)
{
    (void)d; (void)a;
    s_ici = *ci;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) {
            s_mod = *(const VkImageDrmFormatModifierExplicitCreateInfoEXT *)p; s_lay = s_mod.pPlaneLayouts[0];
        }
    *o = (VkImage)(uintptr_t)(0x5000 + ++s_imgs);
    return VK_SUCCESS;
}
static void VKAPI_CALL f_GetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *r)
{ (void)d; (void)i; r->size = 768ull * 3072 * 4; r->alignment = 4096; r->memoryTypeBits = 0x6; }
static VkResult VKAPI_CALL f_GetMemoryFdPropertiesKHR(VkDevice d, VkExternalMemoryHandleTypeFlagBits t, int fd, VkMemoryFdPropertiesKHR *p)
{ (void)d; (void)t; (void)fd; p->memoryTypeBits = 0x4; return VK_SUCCESS; }
static VkResult VKAPI_CALL f_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *o)
{
    (void)d; (void)a;
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) { s_import_fd = ((const VkImportMemoryFdInfoKHR *)p)->fd; }
        if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            s_dedicated_ok = ((const VkMemoryDedicatedAllocateInfo *)p)->image == (VkImage)(uintptr_t)(0x5000 + s_imgs) && ai->memoryTypeIndex == 2;
    }
    if (s_import_fd >= 0) close(s_import_fd);   // the import owns it
    *o = (VkDeviceMemory)(uintptr_t)0x7000;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL f_BindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize off) { (void)d; (void)i; (void)m; (void)off; return VK_SUCCESS; }
static VkResult VKAPI_CALL f_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *o)
{ (void)d; (void)a; s_vci = *ci; s_views++; *o = (VkImageView)(uintptr_t)(0x9000 + s_views); return VK_SUCCESS; }
static void VKAPI_CALL f_DestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)a; if (i) s_destroyed++; }
static void VKAPI_CALL f_FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)a; if (m) s_freed++; }
static VkImageMemoryBarrier s_b1[8]; static uint32_t s_nb1; static VkImageMemoryBarrier2 s_b2[8]; static uint32_t s_nb2;
static void VKAPI_CALL f_Barrier(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags f, uint32_t nm,
                                 const VkMemoryBarrier *pm, uint32_t nb, const VkBufferMemoryBarrier *pb, uint32_t ni, const VkImageMemoryBarrier *pi)
{ (void)c; (void)a; (void)b; (void)f; (void)nm; (void)pm; (void)nb; (void)pb; s_nb1 = ni; memcpy(s_b1, pi, sizeof(*pi) * (ni < 8 ? ni : 8)); }
static void VKAPI_CALL f_Barrier2(VkCommandBuffer c, const VkDependencyInfo *di)
{ (void)c; s_nb2 = di->imageMemoryBarrierCount; memcpy(s_b2, di->pImageMemoryBarriers, sizeof(VkImageMemoryBarrier2) * (s_nb2 < 8 ? s_nb2 : 8)); }
static PFN_vkVoidFunction VKAPI_CALL f_gdpa(VkDevice d, const char *n)
{
    (void)d;
#define F(x) if (!strcmp(n, "vk" #x)) return (PFN_vkVoidFunction)f_##x;
    F(CreateImage) F(GetImageMemoryRequirements) F(GetMemoryFdPropertiesKHR) F(AllocateMemory) F(BindImageMemory)
    F(CreateImageView) F(DestroyImage) F(FreeMemory)
    if (!strcmp(n, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)f_Barrier;
    if (!strcmp(n, "vkCmdPipelineBarrier2")) return (PFN_vkVoidFunction)f_Barrier2;
    return NULL;
}

static int fails;
static void *fake_pre_worker(void *arg)
{
    // like the worker's v7.19b pre-import loop: imports exported-but-not-queued surfaces when asked
    (void)arg;
    pthread_mutex_lock(&g_emu.mu);
    while (!g_emu.imp_pending) { pthread_mutex_unlock(&g_emu.mu); usleep(2000); pthread_mutex_lock(&g_emu.mu); }
    g_emu.imp_pending = 0;
    for (int k = 0; k < FC_PRE; ++k)
        if (g_fc_pre[k].ino && g_fc_pre[k].target < 0 && fd_inode(g_fc_pre[k].fd) == g_fc_pre[k].ino) g_fc_pre[k].target = 11;
    pthread_cond_broadcast(&g_fc_cv);
    pthread_mutex_unlock(&g_emu.mu);
    return NULL;
}
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void *fake_worker(void *arg)
{
    // imports surface 5 when asked (like the real worker's imp_pending handling), after 60 ms
    (void)arg;
    usleep(60 * 1000);
    pthread_mutex_lock(&g_emu.mu);
    while (!g_emu.imp_pending) { pthread_mutex_unlock(&g_emu.mu); usleep(2000); pthread_mutex_lock(&g_emu.mu); }
    g_emu.imp_pending = 0;
    g_emu.cap_target[5] = 9;
    pthread_cond_broadcast(&g_fc_cv);
    pthread_mutex_unlock(&g_emu.mu);
    return NULL;
}

int main(void)
{
    if (!getenv("SLP_FC_TEST")) { printf("run with SLP_FC_TEST=1\n"); return 2; }
    g_dec = (void *)1;
    g_fc = 1;
    p_full_colour = stub_full_colour; p_rgba_export = stub_rgba_export; p_set_target_output = stub_set_out;
    p_target_output = stub_get_out; p_frame_chroma = stub_chroma;
    for (int i = 0; i < 64; ++i) t_rgba_fd[i] = -1;
    for (int i = 0; i < EMU_NBUF; ++i) { g_emu.cap_fd[i] = -1; g_emu.cap_target[i] = -1; }
    int surfA = memfd_create("surfA", 0), surfB = memfd_create("surfB", 0), surfX = memfd_create("unknown", 0);
    t_rgba_fd[7] = memfd_create("rgba7", 0);
    t_rgba_fd[9] = memfd_create("rgba9", 0);
    g_emu.cap_fd[3] = surfA; g_emu.cap_ino[3] = fd_inode(surfA); g_emu.cap_target[3] = 7;
    g_emu.cap_fd[5] = surfB; g_emu.cap_ino[5] = fd_inode(surfB); g_emu.cap_target[5] = -1;   // not imported yet
    printf("fc_test: inodes A %llu B %llu X %llu rgba7 %llu\n", (unsigned long long)fd_inode(surfA),
           (unsigned long long)fd_inode(surfB), (unsigned long long)fd_inode(surfX), (unsigned long long)fd_inode(t_rgba_fd[7]));
    CHECK(fd_inode(surfA) && fd_inode(surfA) != fd_inode(surfB), "fd_inode via raw fstat syscall");
    fc_server_start();
    usleep(100 * 1000);

    fc_rep rep;
    int fd = -1;
    pthread_mutex_lock(&g_cfc_sock_mu);
    int st = cfc_request_locked(FC_GET, fd_inode(surfA), &rep, &fd);
    CHECK(st == FC_OK && fd >= 0, "GET known surface -> OK + fd (status %d, fd %d)", st, fd);
    CHECK(fd >= 0 && fd_inode(fd) == fd_inode(t_rgba_fd[7]), "received fd is the surface's shared image");
    CHECK(rep.width == 768 && rep.height == 3072 && rep.row_pitch == 3072 && rep.vk_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32,
          "layout %ux%u pitch %llu fmt %u", rep.width, rep.height, (unsigned long long)rep.row_pitch, rep.vk_format);
    CHECK(t_out[7] == 2, "target 7 -> both after GET (%d)", t_out[7]);
    if (fd >= 0) close(fd);
    st = cfc_request_locked(FC_ACK, fd_inode(surfA), &rep, NULL);
    CHECK(st == FC_OK && t_out[7] == 1, "ACK -> RGBA only (%d)", t_out[7]);
    st = cfc_request_locked(FC_NACK, fd_inode(surfA), &rep, NULL);
    CHECK(st == FC_OK && t_out[7] == 0, "NACK -> NV12 only (%d)", t_out[7]);

    double t0 = mono_ms();
    st = cfc_request_locked(FC_GET, fd_inode(surfX), &rep, &fd);
    double t1 = mono_ms();
    CHECK(st == FC_UNKNOWN && fd < 0 && t1 - t0 > 200 && t1 - t0 < 400, "unknown surface -> UNKNOWN after the first wait (%.0f ms)", t1 - t0);
    st = cfc_request_locked(FC_GET, fd_inode(surfX), &rep, &fd);
    CHECK(st == FC_UNKNOWN && mono_ms() - t1 < 60, "second unknown answers fast (%.0f ms)", mono_ms() - t1);

    pthread_t w;
    pthread_create(&w, NULL, fake_worker, NULL);
    t0 = mono_ms();
    st = cfc_request_locked(FC_GET, fd_inode(surfB), &rep, &fd);   // known: waits for the worker's import
    CHECK(st == FC_OK && mono_ms() - t0 < 200, "known surface being imported -> waits for the import (%d, %.0f ms)", st, mono_ms() - t0);
    if (fd >= 0) close(fd);
    pthread_join(w, NULL);
    st = cfc_request_locked(FC_GET, fd_inode(surfB), &rep, &fd);
    CHECK(st == FC_OK && fd >= 0 && fd_inode(fd) == fd_inode(t_rgba_fd[9]) && t_out[9] == 2,
          "GET again -> OK, its own image, both (%d)", t_out[9]);
    if (fd >= 0) close(fd);
    g_emu.cap_target[5] = -1;   // not imported, and no worker: gives up after 500 ms
    t0 = mono_ms();
    st = cfc_request_locked(FC_GET, fd_inode(surfB), &rep, &fd);
    CHECK(st == FC_NOT_READY && mono_ms() - t0 > 450 && mono_ms() - t0 < 590, "never imported -> NOT_READY after 500 ms (%.0f ms)", mono_ms() - t0);
    g_emu.cap_target[5] = 9;
    // disconnect while target 9 is in "both": back to NV12
    close(g_cfc_sock);
    g_cfc_sock = -1;
    pthread_mutex_unlock(&g_cfc_sock_mu);
    usleep(100 * 1000);
    CHECK(t_out[9] == 0, "disconnect without ACK -> NV12 (%d)", t_out[9]);


    // ---- v7.19b: surface exported by vrlink, asked for by vrcompositor BEFORE vrlink queues it ----
    pthread_mutex_lock(&g_cfc_sock_mu);
    pthread_mutex_unlock(&g_cfc_sock_mu);
    {
        for (int k = 0; k < FC_PRE; ++k) { g_fc_pre[k].fd = -1; g_fc_pre[k].target = -1; }
        int surfC = memfd_create("surfC", 0);
        t_rgba_fd[11] = memfd_create("rgba11", 0);
        fc_note_export(surfC);
        pthread_t pw;
        pthread_create(&pw, NULL, fake_pre_worker, NULL);
        double t0 = mono_ms();
        int st = cfc_request(FC_GET, fd_inode(surfC), &rep, &fd);
        pthread_join(pw, NULL);
        CHECK(st == FC_OK && fd >= 0 && fd_inode(fd) == fd_inode(t_rgba_fd[11]) && t_out[11] == 2,
              "exported-not-queued surface: GET waits for the early import -> OK (%d, %.0f ms)", st, mono_ms() - t0);
        if (fd >= 0) close(fd);
        st = cfc_request(FC_ACK, fd_inode(surfC), &rep, NULL);
        CHECK(st == FC_OK && t_out[11] == 1, "ACK on a pre-imported surface -> RGBA only (%d)", t_out[11]);
        t_out[11] = 0;
        pthread_mutex_lock(&g_emu.mu);
        int k = fc_pre_find_locked(fd_inode(surfC));
        g_fc_pre[k].claimed = 1;   // as QBUF would after taking the target over (release then belongs to the CAPTURE slot)
        fc_pre_clear_locked();
        CHECK(fc_pre_find_locked(fd_inode(surfC)) < 0, "pre table cleared");
        pthread_mutex_unlock(&g_emu.mu);
    }

    // ---- compositor glue against the fake device (stub driver), real server ----
    printf("compositor glue:\n");
    g_fc = 1;
    g_cfc = 1;
    g_active = 2;
    VkDevice dev = (VkDevice)(uintptr_t)0x1234;
    g_dev[0].dev = dev;
    g_dev[0].next_gdpa = f_gdpa;
    t_out[7] = 0;
    VkImage surf = (VkImage)(uintptr_t)0x4444, other = (VkImage)(uintptr_t)0x4445;
    track_image(surf, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 768, 3072);
    // the AllocateMemory hook path: dedicated dma-buf import of the tracked surface
    {
        PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)slp_GetDeviceProcAddr(dev, "vkAllocateMemory");
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        imp.fd = dup(surfA);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.pNext = &imp;
        ded.image = surf;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &ded;
        VkDeviceMemory m;
        s_import_fd = -1;
        am(dev, &ai, NULL, &m);
        CHECK(g_cfc_tab[0].surf == surf && g_cfc_tab[0].ino == fd_inode(surfA) && g_cfc_tab[0].w == 768, "import of the surface noted (inode %llu)",
              (unsigned long long)g_cfc_tab[0].ino);
    }
    PFN_vkCreateImageView cv = (PFN_vkCreateImageView)slp_GetDeviceProcAddr(dev, "vkCreateImageView");
    VkSamplerYcbcrConversionInfo conv = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    conv.conversion = (VkSamplerYcbcrConversion)(uintptr_t)0x77;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &conv;
    vi.image = surf;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    s_import_fd = -1;
    VkResult vr = cv(dev, &vi, NULL, &view);
    usleep(50 * 1000);
    CHECK(vr == VK_SUCCESS && s_imgs == 1 && s_ici.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && s_ici.extent.width == 768 &&
          s_ici.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && s_mod.drmFormatModifier == 0 && s_lay.rowPitch == 3072,
          "shared image created: A2B10G10R10 768x3072, LINEAR explicit, pitch %llu", (unsigned long long)s_lay.rowPitch);
    CHECK(s_import_fd >= 0 && s_dedicated_ok, "memory imported from the received fd, dedicated, right memory type");
    CHECK(s_views == 1 && s_vci.pNext == NULL && s_vci.image == (VkImage)(uintptr_t)0x5001 && s_vci.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
          view == (VkImageView)(uintptr_t)0x9001, "view of OUR image returned, no YCbCr conversion");
    CHECK(t_out[7] == 1, "vrlink side: target 7 RGBA only after the ACK (%d)", t_out[7]);
    // second view of the same surface: no second import
    cv(dev, &vi, NULL, &view);
    CHECK(s_imgs == 1 && s_views == 2 && s_vci.image == (VkImage)(uintptr_t)0x5001, "second view reuses the imported image");
    // a plane view (aspect PLANE_0) is not swapped
    VkImageViewCreateInfo pv = vi;
    pv.pNext = NULL;
    pv.format = VK_FORMAT_R8_UNORM;
    pv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    cv(dev, &pv, NULL, &view);
    CHECK(s_views == 3 && s_vci.image == surf && s_vci.format == VK_FORMAT_R8_UNORM, "plane view passes through untouched");
    // barriers: mirrored onto our image (same layouts / families), others untouched
    PFN_vkCmdPipelineBarrier pb = (PFN_vkCmdPipelineBarrier)slp_GetDeviceProcAddr(dev, "vkCmdPipelineBarrier");
    PFN_vkCmdPipelineBarrier2 pb2 = (PFN_vkCmdPipelineBarrier2)slp_GetDeviceProcAddr(dev, "vkCmdPipelineBarrier2");
    VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    b[0].image = other; b[1].image = surf;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT; b[1].dstQueueFamilyIndex = 0;
    b[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
    pb((VkCommandBuffer)0, 0, 0, 0, 0, NULL, 0, NULL, 2, b);
    CHECK(s_nb1 == 3 && s_b1[0].image == other && s_b1[1].image == surf && s_b1[2].image == (VkImage)(uintptr_t)0x5001 &&
          s_b1[2].newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && s_b1[2].srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT &&
          s_b1[2].subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT, "vkCmdPipelineBarrier mirrored (%u barriers)", s_nb1);
    pb((VkCommandBuffer)0, 0, 0, 0, 0, NULL, 0, NULL, 1, b);
    CHECK(s_nb1 == 1, "barrier on other images only: passed through");
    VkImageMemoryBarrier2 c2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    c2.image = surf; c2.newLayout = VK_IMAGE_LAYOUT_GENERAL; c2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &c2;
    pb2((VkCommandBuffer)0, &di);
    CHECK(s_nb2 == 2 && s_b2[1].image == (VkImage)(uintptr_t)0x5001 && s_b2[1].newLayout == VK_IMAGE_LAYOUT_GENERAL, "vkCmdPipelineBarrier2 mirrored");
    // destroy: our image + memory freed, vrlink back to NV12, surface untracked
    PFN_vkDestroyImage dimg = (PFN_vkDestroyImage)slp_GetDeviceProcAddr(dev, "vkDestroyImage");
    dimg(dev, surf, NULL);
    usleep(50 * 1000);
    CHECK(s_destroyed == 2 && s_freed == 1 && t_out[7] == 0 && !image_tracked(surf) && atomic_load(&g_cfc_live) == 0,
          "DestroyImage: ours freed, NACK -> NV12 (%d), untracked", t_out[7]);
    pb((VkCommandBuffer)0, 0, 0, 0, 0, NULL, 0, NULL, 2, b);
    CHECK(s_nb1 == 2, "after destroy: barriers pass through");
    // failure path: vrlink does not know the surface -> original view
    track_image(surf, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 768, 3072);
    cfc_note_import(dev, surf, fd_inode(surfX));
    int before = s_views;
    cv(dev, &vi, NULL, &view);
    CHECK(s_views == before + 1 && s_vci.image == surf && s_vci.pNext == &conv, "unknown surface: vrcompositor's own NV12 view");
    // reconnect works; full colour off -> OFF
    pthread_mutex_lock(&g_cfc_sock_mu);
    g_fc = 0;
    st = cfc_request_locked(FC_GET, fd_inode(surfA), &rep, &fd);
    CHECK(st == FC_OFF && fd < 0, "reconnect + full colour off -> OFF (%d)", st);
    pthread_mutex_unlock(&g_cfc_sock_mu);
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
