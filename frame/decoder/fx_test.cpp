// Host test of the v7.21 power changes on any Vulkan device (lavapipe):
//   1. pack_rgba10_fx.comp (blur + inset sharpening folded into the pack) against the v7.20 passes
//      (wideblur_apply_r16 in place, sharpen_r16 into yf, pack_rgba10 with the light square): must be bit-identical,
//      output and headset-stability history, with and without history, plus a repeat frame from the history.
//   2. the fast (bilinear) blur pyramid against the exact 6x6 one (max difference in 10-bit steps).
// Usage: fx_test [W]   (atlas W x 4W, W multiple of 16)
#include "volk.h"
#include "wideblur_down_spv.h"
#include "wideblur_apply_r16_spv.h"
#include "sharpen_r16_spv.h"
#include "pack_rgba10_spv.h"
#include "pack_rgba10_fx_spv.h"
#include "wideblur_apply_spv.h"
#include "sharpen_spv.h"
#ifdef OLD_SHADERS   // v7.20 shaders (built from the previous sources) for the refactor regression check
#include "old_apply8_spv.h"
#include "old_apply16_spv.h"
#include "old_sharpen16_spv.h"
#include "old_down_spv.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice gpu;
static VkDevice dev;
static VkQueue q;
static VkCommandPool cpool;
static VkCommandBuffer cb;
static VkFence fence;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(2);
}

struct Img { VkImage i; VkDeviceMemory m; VkImageView v; uint32_t w, h, layers, levels; VkFormat f; VkImageView lv[8]; };

static Img mk(uint32_t w, uint32_t h, VkFormat f, uint32_t layers = 1, uint32_t levels = 1)
{
	Img r = {};
	r.w = w; r.h = h; r.f = f; r.layers = layers; r.levels = levels;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent = {w, h, 1};
	ii.mipLevels = levels; ii.arrayLayers = layers; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateImage(dev, &ii, nullptr, &r.i));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, r.i, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, 0);
	CK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
	CK(vkBindImageMemory(dev, r.i, r.m, 0));
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = r.i;
	vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	vi.format = f;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
	CK(vkCreateImageView(dev, &vi, nullptr, &r.v));
	for (uint32_t l = 0; l < levels && levels > 1; l++)
	{
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, layers};
		CK(vkCreateImageView(dev, &vi, nullptr, &r.lv[l]));
	}
	return r;
}

static void begin()
{
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CK(vkResetCommandBuffer(cb, 0));
	CK(vkBeginCommandBuffer(cb, &bi));
}
static void submit()
{
	CK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	CK(vkQueueSubmit(q, 1, &si, fence));
	CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
	CK(vkResetFences(dev, 1, &fence));
}
static void full_barrier()
{
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
static void to_general(const Img &im)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.i;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im.levels, 0, im.layers};
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

struct Buf { VkBuffer b; VkDeviceMemory m; void *p; };
static Buf mkbuf(size_t n)
{
	Buf r = {};
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = n;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateBuffer(dev, &bi, nullptr, &r.b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, r.b, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
	CK(vkBindBufferMemory(dev, r.b, r.m, 0));
	CK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
	return r;
}
static size_t bpp(VkFormat f) { return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 4 : 2; }
// level 0 of all layers
static void upload(const Img &im, const void *data)
{
	size_t n = size_t(im.w) * im.h * im.layers * bpp(im.f);
	Buf s = mkbuf(n);
	memcpy(s.p, data, n);
	begin();
	VkBufferImageCopy c = {};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers};
	c.imageExtent = {im.w, im.h, 1};
	vkCmdCopyBufferToImage(cb, s.b, im.i, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
	submit();
	vkDestroyBuffer(dev, s.b, nullptr);
	vkFreeMemory(dev, s.m, nullptr);
}
static std::vector<uint8_t> download(const Img &im, uint32_t level = 0)
{
	uint32_t w = std::max(1u, im.w >> level), h = std::max(1u, im.h >> level);
	size_t n = size_t(w) * h * im.layers * bpp(im.f);
	Buf s = mkbuf(n);
	begin();
	full_barrier();
	VkBufferImageCopy c = {};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, im.layers};
	c.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(cb, im.i, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &c);
	submit();
	std::vector<uint8_t> out(n);
	memcpy(out.data(), s.p, n);
	vkDestroyBuffer(dev, s.b, nullptr);
	vkFreeMemory(dev, s.m, nullptr);
	return out;
}
static void copy_img(const Img &a, const Img &b)
{
	begin();
	full_barrier();
	VkImageCopy c = {};
	c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, a.layers};
	c.extent = {a.w, a.h, 1};
	vkCmdCopyImage(cb, a.i, VK_IMAGE_LAYOUT_GENERAL, b.i, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
	submit();
}

struct Pipe { VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline p; VkDescriptorPool pool; };
static Pipe mkpipe(const uint32_t *code, size_t size, std::vector<VkDescriptorType> types, uint32_t pcsize)
{
	Pipe r = {};
	std::vector<VkDescriptorSetLayoutBinding> b(types.size());
	for (size_t i = 0; i < types.size(); i++)
		b[i] = {uint32_t(i), types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dl.bindingCount = uint32_t(b.size());
	dl.pBindings = b.data();
	CK(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &r.dsl));
	VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pcsize};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &r.dsl;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pr;
	CK(vkCreatePipelineLayout(dev, &pli, nullptr, &r.pl));
	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = size;
	smi.pCode = code;
	VkShaderModule sm;
	CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
	VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cp.stage.module = sm;
	cp.stage.pName = "main";
	cp.layout = r.pl;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, &r.p));
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
	                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 16;
	dpi.poolSizeCount = 3;
	dpi.pPoolSizes = ps;
	CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &r.pool));
	return r;
}
struct Bind { VkSampler s; VkImageView v; };
static VkDescriptorSet mkset(const Pipe &p, std::vector<VkDescriptorType> types, std::vector<Bind> binds)
{
	VkDescriptorSet s;
	VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	da.descriptorPool = p.pool;
	da.descriptorSetCount = 1;
	da.pSetLayouts = &p.dsl;
	CK(vkAllocateDescriptorSets(dev, &da, &s));
	std::vector<VkDescriptorImageInfo> ii(types.size());
	std::vector<VkWriteDescriptorSet> w(types.size());
	for (size_t i = 0; i < types.size(); i++)
	{
		ii[i] = {binds[i].s, binds[i].v, VK_IMAGE_LAYOUT_GENERAL};
		w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w[i].dstSet = s;
		w[i].dstBinding = uint32_t(i);
		w[i].descriptorCount = 1;
		w[i].descriptorType = types[i];
		w[i].pImageInfo = &ii[i];
	}
	vkUpdateDescriptorSets(dev, uint32_t(w.size()), w.data(), 0, nullptr);
	return s;
}
static void dispatch(const Pipe &p, VkDescriptorSet s, const void *pc, uint32_t pcs, uint32_t x, uint32_t y, uint32_t z)
{
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &s, 0, nullptr);
	vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcs, pc);
	vkCmdDispatch(cb, x, y, z);
	full_barrier();
}

int main(int argc, char **argv)
{
	const uint32_t W = argc > 1 ? uint32_t(atoi(argv[1])) : 256, H = 4 * W, T = W;
	CK(volkInitialize());
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CK(vkCreateInstance(&ici, nullptr, &inst));
	volkLoadInstance(inst);
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &gpu);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	CK(vkCreateDevice(gpu, &dci, nullptr, &dev));
	volkLoadDevice(dev);
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	CK(vkCreateCommandPool(dev, &cpi, nullptr, &cpool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = cpool;
	cai.commandBufferCount = 1;
	CK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CK(vkCreateFence(dev, &fci, nullptr, &fence));

	uint32_t levels = 0;
	for (uint32_t s = T / 2; s >= 8 && levels < 6; s /= 2)
		levels++;
	Img Y = mk(W, H, VK_FORMAT_R16_UNORM), YA = mk(W, H, VK_FORMAT_R16_UNORM), Yf = mk(W, H, VK_FORMAT_R16_UNORM);
	Img Cb = mk(W / 2, H / 2, VK_FORMAT_R16_UNORM), Cr = mk(W / 2, H / 2, VK_FORMAT_R16_UNORM);
	Img OA = mk(W, H, VK_FORMAT_A2B10G10R10_UNORM_PACK32), OB = mk(W, H, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
	Img M = mk(T / 2, T / 2, VK_FORMAT_R16_SFLOAT, 2, levels), MF = mk(T / 2, T / 2, VK_FORMAT_R16_SFLOAT, 2, levels);
	Img HA = mk(T, T, VK_FORMAT_R16_SFLOAT, 2), HB = mk(T, T, VK_FORMAT_R16_SFLOAT, 2), H0 = mk(T, T, VK_FORMAT_R16_SFLOAT, 2);
	begin();
	for (const Img *im : {&Y, &YA, &Yf, &Cb, &Cr, &OA, &OB, &M, &MF, &HA, &HB, &H0})
		to_general(*im);
	submit();
	// picture: smooth gradients + texture + hard 16-px block steps (what the blur and sharpening act on)
	std::vector<uint16_t> y(size_t(W) * H), c1(size_t(W / 2) * (H / 2)), c2(c1.size());
	srand(7);
	for (uint32_t j = 0; j < H; j++)
		for (uint32_t i = 0; i < W; i++)
		{
			double v = 0.4 + 0.25 * sin(i * 0.05 + j * 0.013) + 0.08 * sin(i * 0.9) * cos(j * 0.7) + 0.04 * ((i / 16 + j / 16) & 1) +
			           0.02 * (rand() / double(RAND_MAX) - 0.5);
			y[size_t(j) * W + i] = uint16_t(std::lround(std::min(1.0, std::max(0.0, v)) * 1023.0) * 65535 / 1023);
		}
	for (size_t i = 0; i < c1.size(); i++)
	{
		c1[i] = uint16_t(20000 + (i * 37) % 20000);
		c2[i] = uint16_t(30000 + (i * 53) % 15000);
	}
	std::vector<uint16_t> h0(size_t(T) * T * 2);
	for (size_t i = 0; i < h0.size(); i++)   // R16F history: plausible values near the picture
	{
		float f = float(y[(size_t(T) + i % (size_t(T) * T)) % y.size()]) / 65535.0f + 0.01f * float((i * 7) % 5) - 0.02f;
		uint32_t b;
		memcpy(&b, &f, 4);
		// float -> half (values in 0..1, normal range)
		uint32_t e = (b >> 23) & 255, mnt = b & 0x7fffff;
		uint16_t hh = (e < 113) ? 0 : uint16_t(((e - 112) << 10) | (mnt >> 13));
		h0[i] = hh;
	}
	upload(Y, y.data());
	upload(Cb, c1.data());
	upload(Cr, c2.data());
	upload(H0, h0.data());

	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = float(levels - 1);
	VkSampler smp, smp0, smpk;
	CK(vkCreateSampler(dev, &si, nullptr, &smp));
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.maxLod = 0.0f;
	CK(vkCreateSampler(dev, &si, nullptr, &smp0));
	CK(vkCreateSampler(dev, &si, nullptr, &smpk));

	const VkDescriptorType S = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ST = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	                       C = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	std::vector<VkDescriptorType> td = {S, S, ST, C, C}, ta = {ST, C, ST}, ts = {S, ST}, tp = {S, S, C, C, ST},
	                              tx = {S, S, C, C, ST, C, ST};
	Pipe pd = mkpipe(slp_wideblur_down_spv, sizeof(slp_wideblur_down_spv), td, 12);
	Pipe pa = mkpipe(slp_wideblur_apply_r16_spv, sizeof(slp_wideblur_apply_r16_spv), ta, 96);
	Pipe ps = mkpipe(slp_sharpen_r16_spv, sizeof(slp_sharpen_r16_spv), ts, 36);
	Pipe pp = mkpipe(slp_pack_rgba10_spv, sizeof(slp_pack_rgba10_spv), tp, 24);
	Pipe px = mkpipe(slp_pack_rgba10_fx_spv, sizeof(slp_pack_rgba10_fx_spv), tx, 128);

	auto pyramid = [&](Img &mip, int fast) {
		begin();
		for (uint32_t l = 0; l < levels; l++)
		{
			VkDescriptorSet s = mkset(pd, td, {{0, Y.v}, {0, mip.v}, {0, mip.lv[l]}, {smp0, Y.v}, {smp, mip.v}});
			int pc[3] = {int(l), int(T), fast};
			uint32_t sz = (T / 2) >> l;
			dispatch(pd, s, pc, 12, (sz + 7) / 8, (sz + 7) / 8, 2);
		}
		submit();
	};
	pyramid(M, 0);
	pyramid(MF, 1);
	int ok = 1;
	{
		double md = 0;
		for (uint32_t l = 0; l < levels; l++)
		{
			auto a = download(M, l), b = download(MF, l);
			for (size_t i = 0; i < a.size(); i += 2)
			{
				auto h2f = [](const uint8_t *p) {
					uint16_t hh = uint16_t(p[0] | (p[1] << 8));
					int e = (hh >> 10) & 31, m = hh & 1023;
					return e == 0 ? ldexp(double(m), -24) : ldexp(1.0 + m / 1024.0, e - 15);
				};
				md = std::max(md, fabs(h2f(&a[i]) - h2f(&b[i])));
			}
		}
		printf("fast pyramid vs exact: max difference %.4f of full scale (%.2f 10-bit steps) over %u levels -> %s\n", md,
		       md * 1023.0, levels, md * 1023.0 <= 0.75 ? "OK" : "TOO LARGE");
		ok &= md * 1023.0 <= 0.75;
	}

	// settings as Balanced (sharpen 0.5, r0 0.35, r1 0.8, blur 0.3 px, contrast 3, stability 0.6 / 0.6, ramp 0.2)
	const float rp[4] = {0.2f, 15.0f, 55.0f, 0.2f}, pj0[4] = {0.32f, 0.58f, -0.33f, 0.40f}, pj1[4] = {0.32f, 0.42f, -0.33f, 0.40f},
	            cen[4] = {0.58f, 0.40f, 0.42f, 0.40f};
	const float sharpen = 0.5f, r0 = 0.35f, r1 = 0.8f, blur = 0.3f, contrast = 3.0f, temporal = 0.6f, tclamp = 0.6f;
	const float max_lod = float(levels) - 1.5f;
	const float half = std::min(1.0f, r1) * 0.5f * float(T) + 2.0f;
	const uint32_t cc = T / 2, h8 = std::min(cc, (uint32_t(std::ceil(half)) + 7u) & ~7u), sq0 = cc - h8, sq1 = std::min(T, cc + h8);

	auto run = [&](float dither, float hist_ok, bool repeat_b, bool blur_on, bool light, std::vector<uint8_t> *oa,
	               std::vector<uint8_t> *ob, std::vector<uint8_t> *ha, std::vector<uint8_t> *hb) {
		// path A (v7.20): YA = Y; apply in place on YA (history HA); sharpen YA -> Yf (squares); pack(YA, Yf) -> OA
		copy_img(Y, YA);
		begin();
		if (blur_on)
		{
			VkDescriptorSet s = mkset(pa, ta, {{0, YA.v}, {smp, M.v}, {0, HA.v}});
			struct { float lod, contrast, dither, tile, max_lod, temporal, tclamp, hist_ok; float rp[4], pj0[4], pj1[4], cen[4]; } apc = {
			    blur, contrast, dither, float(T), max_lod, temporal, tclamp, hist_ok, {}, {}, {}, {}};
			memcpy(apc.rp, rp, 16); memcpy(apc.pj0, pj0, 16); memcpy(apc.pj1, pj1, 16); memcpy(apc.cen, cen, 16);
			dispatch(pa, s, &apc, 96, (T + 15) / 16, (T + 15) / 16, 2);
		}
		if (light)
		{
			VkDescriptorSet s = mkset(ps, ts, {{0, YA.v}, {0, Yf.v}});
			for (uint32_t t4 = 0; t4 < 4; t4 += 2)
			{
				float pc[9] = {float(sq0), 0.0f, sharpen, r0, r1, dither, float(T), float(t4 * T + sq0), 0.0f};
				dispatch(ps, s, pc, 36, (sq1 - sq0 + 7) / 8, (sq1 - sq0 + 7) / 8, 1);
			}
		}
		{
			VkDescriptorSet s = mkset(pp, tp, {{0, YA.v}, {0, Yf.v}, {smpk, Cb.v}, {smpk, Cr.v}, {0, OA.v}});
			float kpc[6] = {dither, float(T), light ? 1.0f : 0.0f, float(sq0), float(sq1), 0.0f};
			dispatch(pp, s, kpc, 24, (W + 7) / 8, (H + 7) / 8, 1);
		}
		// path B (v7.21): pack_fx(Y) -> OB (history HB)
		{
			VkDescriptorSet s = mkset(px, tx, {{0, Y.v}, {0, Yf.v}, {smpk, Cb.v}, {smpk, Cr.v}, {0, OB.v}, {smp, M.v}, {0, HB.v}});
			struct { float a[4], b[4], c[4], d[4], rp[4], pj0[4], pj1[4], cen[4]; } xpc = {
			    {dither, float(T), light ? 1.0f : 0.0f, 0.0f}, {float(sq0), float(sq1), sharpen, r0},
			    {r1, blur, contrast, max_lod}, {temporal, tclamp, hist_ok, float((blur_on ? 1 : 0) | (repeat_b ? 2 : 0))}, {}, {}, {}, {}};
			memcpy(xpc.rp, rp, 16); memcpy(xpc.pj0, pj0, 16); memcpy(xpc.pj1, pj1, 16); memcpy(xpc.cen, cen, 16);
			dispatch(px, s, &xpc, 128, (W + 7) / 8, (H + 7) / 8, 1);
		}
		submit();
		if (oa) *oa = download(OA);
		if (ob) *ob = download(OB);
		if (ha) *ha = download(HA);
		if (hb) *hb = download(HB);
	};
	auto cmp = [&](const char *name, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
		size_t nd = 0;
		for (size_t i = 0; i < a.size(); i++)
			nd += a[i] != b[i];
		printf("  %-58s %s (%zu bytes differ of %zu)\n", name, nd ? "MISMATCH" : "IDENTICAL", nd, a.size());
		ok &= nd == 0;
	};
	struct Case { const char *name; float dither, hist_ok; bool blur, light; } cases[] = {
	    {"Balanced, no history yet", 1.0f, 0.0f, true, true},
	    {"Balanced, with history", 1.0f, 1.0f, true, true},
	    {"Balanced, with history, dither off", 0.0f, 1.0f, true, true},
	    {"blur only (no sharpening)", 1.0f, 1.0f, true, false},
	    {"sharpening only (no blur)", 1.0f, 0.0f, false, true}};
	printf("merged pack (v7.21) vs separate passes (v7.20), %ux%u:\n", W, H);
	for (auto &c : cases)
	{
		copy_img(H0, HA);
		copy_img(H0, HB);
		std::vector<uint8_t> oa, ob, ha, hb;
		run(c.dither, c.hist_ok, false, c.blur, c.light, &oa, &ob, &ha, &hb);
		char nm[128];
		snprintf(nm, sizeof(nm), "%s: picture", c.name);
		cmp(nm, oa, ob);
		if (c.blur)
		{
			snprintf(nm, sizeof(nm), "%s: history", c.name);
			cmp(nm, ha, hb);
		}
		if (c.blur && c.hist_ok > 0.5f)
		{
			// repeat frame: B re-presents the wide views from its history (= the result just shown)
			std::vector<uint8_t> ob2, hb2;
			run(c.dither, c.hist_ok, true, c.blur, c.light, nullptr, &ob2, nullptr, &hb2);
			// the history is R16F, so the re-presented wide views can round to the neighbouring 10-bit code
			int md[3] = {0, 0, 0};
			size_t nd = 0;
			for (size_t i = 0; i < ob.size(); i += 4)
			{
				uint32_t a, b;
				memcpy(&a, &ob[i], 4);
				memcpy(&b, &ob2[i], 4);
				if (a == b) continue;
				nd++;
				for (int k = 0; k < 3; k++)
					md[k] = std::max(md[k], abs(int((a >> (10 * k)) & 1023) - int((b >> (10 * k)) & 1023)));
			}
			bool rok = md[1] <= 1 && md[0] == 0 && md[2] == 0;
			printf("  %-58s %s (%zu px differ, max Y %d Cb %d Cr %d codes)\n", (std::string(c.name) + ": repeat frame vs last picture").c_str(),
			       rok ? "OK (<= 1 code)" : "MISMATCH", nd, md[1], md[2], md[0]);
			ok &= rok;
			snprintf(nm, sizeof(nm), "%s: repeat leaves the history", c.name);
			cmp(nm, hb, hb2);
		}
	}
#ifdef OLD_SHADERS
	{
		printf("v7.21 shader refactor vs the v7.20 shaders (same inputs):\n");
		Img YB = mk(W, H, VK_FORMAT_R16_UNORM), Yf2 = mk(W, H, VK_FORMAT_R16_UNORM);
		Img Y8 = mk(W, H, VK_FORMAT_R8_UNORM), Y8A = mk(W, H, VK_FORMAT_R8_UNORM), Y8B = mk(W, H, VK_FORMAT_R8_UNORM);
		Img MO = mk(T / 2, T / 2, VK_FORMAT_R16_SFLOAT, 2, levels);
		begin();
		for (const Img *im : {&YB, &Yf2, &Y8, &Y8A, &Y8B, &MO})
			to_general(*im);
		submit();
		std::vector<uint8_t> y8(y.size());
		for (size_t i = 0; i < y.size(); i++)
			y8[i] = uint8_t(y[i] >> 8);
		upload(Y8, y8.data());
		// pyramid: old shader vs new with fast = 0
		Pipe po = mkpipe(slp_old_down_spv, sizeof(slp_old_down_spv), {S, S, ST}, 8);
		begin();
		for (uint32_t l = 0; l < levels; l++)
		{
			VkDescriptorSet s = mkset(po, {S, S, ST}, {{0, Y.v}, {0, MO.v}, {0, MO.lv[l]}});
			int pc[2] = {int(l), int(T)};
			uint32_t sz = (T / 2) >> l;
			dispatch(po, s, pc, 8, (sz + 7) / 8, (sz + 7) / 8, 2);
		}
		submit();
		for (uint32_t l = 0; l < levels; l++)
		{
			char nm[64];
			snprintf(nm, sizeof(nm), "pyramid level %u (exact path)", l);
			cmp(nm, download(MO, l), download(M, l));
		}
		struct { float lod, contrast, dither, tile, max_lod, temporal, tclamp, hist_ok; float rp[4], pj0[4], pj1[4], cen[4]; } apc = {
		    blur, contrast, 1.0f, float(T), max_lod, temporal, tclamp, 1.0f, {}, {}, {}, {}};
		memcpy(apc.rp, rp, 16); memcpy(apc.pj0, pj0, 16); memcpy(apc.pj1, pj1, 16); memcpy(apc.cen, cen, 16);
		auto apply_cmp = [&](const char *name, const uint32_t *oc, size_t os, const uint32_t *nc, size_t ns, const Img &src, const Img &a, const Img &b) {
			Pipe pO = mkpipe(oc, os, ta, 96), pN = mkpipe(nc, ns, ta, 96);
			copy_img(src, a);
			copy_img(src, b);
			copy_img(H0, HA);
			copy_img(H0, HB);
			begin();
			dispatch(pO, mkset(pO, ta, {{0, a.v}, {smp, M.v}, {0, HA.v}}), &apc, 96, (T + 15) / 16, (T + 15) / 16, 2);
			dispatch(pN, mkset(pN, ta, {{0, b.v}, {smp, M.v}, {0, HB.v}}), &apc, 96, (T + 15) / 16, (T + 15) / 16, 2);
			submit();
			char nm[96];
			snprintf(nm, sizeof(nm), "%s: picture", name);
			cmp(nm, download(a), download(b));
			snprintf(nm, sizeof(nm), "%s: history", name);
			cmp(nm, download(HA), download(HB));
		};
		apply_cmp("wide blur 16-bit", slp_old_apply16_spv, sizeof(slp_old_apply16_spv), slp_wideblur_apply_r16_spv,
		          sizeof(slp_wideblur_apply_r16_spv), Y, YA, YB);
		apply_cmp("wide blur 8-bit", slp_old_apply8_spv, sizeof(slp_old_apply8_spv), slp_wideblur_apply_spv,
		          sizeof(slp_wideblur_apply_spv), Y8, Y8A, Y8B);
		Pipe sO = mkpipe(slp_old_sharpen16_spv, sizeof(slp_old_sharpen16_spv), ts, 36);
		copy_img(Y, Yf);
		copy_img(Y, Yf2);
		begin();
		for (uint32_t t4 = 0; t4 < 4; t4 += 2)
		{
			float pc[9] = {float(sq0), 0.0f, sharpen, r0, r1, 1.0f, float(T), float(t4 * T + sq0), 0.0f};
			dispatch(sO, mkset(sO, ts, {{0, Y.v}, {0, Yf.v}}), pc, 36, (sq1 - sq0 + 7) / 8, (sq1 - sq0 + 7) / 8, 1);
			dispatch(ps, mkset(ps, ts, {{0, Y.v}, {0, Yf2.v}}), pc, 36, (sq1 - sq0 + 7) / 8, (sq1 - sq0 + 7) / 8, 1);
		}
		submit();
		cmp("inset sharpening 16-bit", download(Yf), download(Yf2));
	}
#endif
	printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
