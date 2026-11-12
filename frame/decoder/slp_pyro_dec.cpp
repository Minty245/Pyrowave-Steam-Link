// Steam Link Pyrowave - Frame-side GPU decoder core (see slp_pyro_dec.hpp).
// SPDX-License-Identifier: MIT
#include "slp_pyro_dec.hpp"

#include "volk.h"
#include "pyrowave.h"
#include "chroma_pack_spv.h"
#include "sample_readback_spv.h"
#include "to8bit_spv.h"
#include "finish8_spv.h"
#include "chroma_pack8_spv.h"
#include "postfx_spv.h"
#include "seam_spv.h"
#include "sharpen_spv.h"
#include "wideblur_down_spv.h"
#include "wideblur_apply_spv.h"
#include "seam_r16_spv.h"
#include "sharpen_r16_spv.h"
#include "postfx_r16_spv.h"
#include "wideblur_apply_r16_spv.h"
#include "pack_rgba10_spv.h"
#include "pack_rgba10_fx_spv.h"
#include <atomic>
#include <cmath>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace slp
{
bool parse_frame(const uint8_t *data, size_t size, FrameHeader *hdr, const uint32_t **sizes, const uint8_t **payload)
{
	if (!data || size < sizeof(FrameHeader))
		return false;
	FrameHeader h;
	memcpy(&h, data, sizeof(h));
	if (h.magic != kFrameMagic || h.version != kFrameVersion || h.header_size != sizeof(FrameHeader))
		return false;
	if (h.num_packets > 4096 || (h.num_packets == 0 && h.payload_bytes != 0))
		return false; // num_packets == 0: "repeat" frame (PC had no new picture)
	size_t table = size_t(h.num_packets) * 4;
	if (size < sizeof(FrameHeader) + table)
		return false;
	const uint32_t *sz = reinterpret_cast<const uint32_t *>(data + sizeof(FrameHeader));
	uint64_t total = 0;
	for (uint32_t i = 0; i < h.num_packets; i++)
		total += sz[i];
	if (total != h.payload_bytes || sizeof(FrameHeader) + table + total > size)
		return false;
	*hdr = h;
	*sizes = sz;
	*payload = data + sizeof(FrameHeader) + table;
	return true;
}

namespace
{
const char *vkr(VkResult r)
{
	switch (r)
	{
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "INVALID_EXTERNAL_HANDLE";
	case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT: return "INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT";
	case VK_TIMEOUT: return "TIMEOUT";
	default: return "VK_ERROR";
	}
}

std::atomic<bool> g_default_rgba_compressed{false};   // v7.21 (Decoder::set_default_rgba_compressed)

bool fail(std::string *err, const char *what, VkResult r = VK_SUCCESS)
{
	if (err)
	{
		char buf[256];
		if (r != VK_SUCCESS)
			snprintf(buf, sizeof(buf), "%s failed: %s (%d)", what, vkr(r), int(r));
		else
			snprintf(buf, sizeof(buf), "%s failed", what);
		*err = buf;
	}
	return false;
}

struct Plane
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0;
};

struct Target
{
	bool used = false;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	bool readable = false;
	// Alias modes: the same dma-buf seen as two single-plane UBWC images (R8 luma, R8G8 chroma) that the
	// GPU writes directly (storage), so no copy into the NV12 image is needed.
	bool alias_ok = false;
	VkDeviceMemory amem = VK_NULL_HANDLE;
	VkImage ay = VK_NULL_HANDLE, ac = VK_NULL_HANDLE;
	VkImage ys = VK_NULL_HANDLE; // mode 5: NV12 view of the dma-buf with STORAGE, luma written through its PLANE_0
	int ydirect = 0;             // 0: luma copied, 1: R8 alias (ay), 2: NV12 plane 0 (ys)
	VkImageView ayv = VK_NULL_HANDLE, acv = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;
	// v7.19 full colour: the shared 10-bit RGBA image (LINEAR dma-buf) and its pack sets (0: 4:2:0, 1: 4:4:4)
	VkImage rimg = VK_NULL_HANDLE;
	VkDeviceMemory rmem = VK_NULL_HANDLE;
	VkImageView rview = VK_NULL_HANDLE;
	VkDescriptorSet rset[2] = {};
	VkDescriptorSet xset[2] = {};  // v7.21: sets of the pack with the luma filters folded in (pack_rgba10_fx)
	uint64_t rmod = 0;             // v7.21: DRM modifier of the shared image (0 = LINEAR)
	bool rexport = false;          // memory exportable as dma-buf
	bool rinit = false;            // cleared once
	uint64_t roffset = 0, rpitch = 0, rsize = 0;
};
} // namespace

struct Decoder::Impl
{
	// Vulkan objects (all ours).
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memprops = {};
	VkPhysicalDeviceProperties props = {};
	bool has_foreign = false;
	bool preserve_layout = false;

	// Create infos kept alive for Pyrowave.
	VkApplicationInfo app = {};
	VkInstanceCreateInfo ici = {};
	VkPhysicalDeviceFeatures2 f2 = {};
	VkPhysicalDeviceVulkan11Features f11 = {};
	VkPhysicalDeviceVulkan12Features f12 = {};
	VkPhysicalDeviceVulkan13Features f13 = {};
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	std::vector<const char *> dev_exts;
	std::vector<std::string> dev_ext_store;
	VkDeviceCreateInfo dci = {};
	pyrowave_device_create_queue_info pq = {};

	pyrowave_device pdev = nullptr;
	pyrowave_decoder pdec = nullptr;
	pyrowave_decoder pdec4 = nullptr;   // v7.19 full colour: 4:4:4 frames

	int width = 0, height = 0;
	int bits = 10;           // 10: P010 client surface (copy into UBWC P010 is broken on the Frame's Turnip); 8: NV12
	float dither = 1.0f;
	int mode = 0;            // OutputMode
	bool direct8 = false;    // 8-bit: Pyrowave decodes straight into R8 planes (no 16-bit finish pass, no dither)
	bool alias = false;      // write (some of) the client surface directly through alias images
	bool pyro_dither = false; // Pyrowave fork: dithered 8-bit final iDWT pass available
	std::string alias_note;  // last alias import result (self-test / log)
	Plane y, cb, cr, cbcr;   // 16-bit decode output (+ packed CbCr for 10-bit)
	Plane y8, c8;            // 8-bit output (NV12 mode)
	// v7.19 full colour (mode 6): filters run on the 16-bit luma, 4:4:4 chroma planes, pack into the shared images
	bool rgba = false;
	Plane cb4, cr4;
	int last_chroma = 0;     // chroma of the planes' current contents (repeat frames)
	VkDescriptorSet dset444 = VK_NULL_HANDLE;   // finish8 with the 4:4:4 chroma planes
	VkDescriptorPool pk_pool = VK_NULL_HANDLE;
	VkDescriptorSetLayout pk_dsl = VK_NULL_HANDLE;
	VkPipelineLayout pk_pl = VK_NULL_HANDLE;
	VkShaderModule pk_sh = VK_NULL_HANDLE;
	VkPipeline pk_pipe = VK_NULL_HANDLE;
	VkSampler pk_smp = VK_NULL_HANDLE;
	bool pk_ok = false;
	// v7.21: pack with the wide-view blur and the inset sharpening folded in (RGBA-only frames)
	VkDescriptorSetLayout pkx_dsl = VK_NULL_HANDLE;
	VkPipelineLayout pkx_pl = VK_NULL_HANDLE;
	VkShaderModule pkx_sh = VK_NULL_HANDLE;
	VkPipeline pkx_pipe = VK_NULL_HANDLE;
	bool pkx_ok = false;
	std::atomic<bool> merge_on{true};
	bool rgba_compressed = false;   // v7.21: try a UBWC (compressed) shared image; LINEAR otherwise
	// v7.21: per-pass GPU time, averaged for the layer's 1 s stats (pass_report)
	static constexpr int kPasses = 6;
	std::mutex ps_mu;
	double ps_sum[kPasses] = {};
	double ps_total = 0;
	int ps_n = 0;
	bool has_modifiers = false;
	static constexpr int kMaxOut = 64;
	std::atomic<int> tout[kMaxOut] = {};
	std::mutex tmu;          // v7.19: targets vs rgba_export from another thread (vrlink's full-colour socket)
	// v7.5 luma post-filter (deblock / dering + inset sharpening), 8-bit modes with a luma copy only
	Plane yf;
	VkDescriptorSetLayout pf_dsl = VK_NULL_HANDLE;
	VkPipelineLayout pf_pl = VK_NULL_HANDLE;
	VkPipeline pf_pipe = VK_NULL_HANDLE;
	VkDescriptorSet pf_dset = VK_NULL_HANDLE;
	VkShaderModule pf_shader = VK_NULL_HANDLE;
	VkShaderModule sh_shader = VK_NULL_HANDLE;   // v7.12: light inset-only sharpen (shares pf_pl / pf_dset)
	VkPipeline sh_pipe = VK_NULL_HANDLE;
	std::atomic<bool> sh_on{true};
	// v7.13: contrast-preserving blur of the wide views after decoding (in place on out_y())
	std::atomic<float> wb_px{0.0f}, wb_contrast{1.0f};
	// v7.17: headset stability (temporal) in the wide-blur pass: history of the two wide views (R16F, T x T x 2)
	std::atomic<float> wb_temporal{0.0f}, wb_tclamp{1.0f};
	VkImage wb_hist = VK_NULL_HANDLE;
	VkDeviceMemory wb_hist_mem = VK_NULL_HANDLE;
	VkImageView wb_hist_view = VK_NULL_HANDLE;
	bool wb_hist_inited = false, wb_hist_ok = false;
	bool wb_pyr_ok = false;   // v7.21: the pyramid holds the last decoded frame (merged repeats read it)
	// v7.14: shared edge ramp from the PC (SLPR): ramp, start, end, overshoot, then per eye cx, cy, ax, bx, ay, by
	std::mutex ramp_mu;
	float ramp[16] = {0.0f, 15.0f, 55.0f, 0.0f, 0.5f, 0.5f, 0.32f, 0.58f, -0.33f, 0.40f,
	                  0.5f, 0.5f, 0.32f, 0.42f, -0.33f, 0.40f};
	// push-constant layout used by seam.comp and wideblur_apply.comp: rp, pj0, pj1, cen
	void ramp_pc(float out[16])
	{
		std::lock_guard<std::mutex> lk(ramp_mu);
		const float *r = ramp;
		const float v[16] = {r[0], r[1], r[2], r[3], r[6], r[7], r[8], r[9], r[12], r[13], r[14], r[15],
		                     r[4], r[5], r[10], r[11]};
		memcpy(out, v, sizeof(v));
	}
	VkImage wb_img = VK_NULL_HANDLE;
	VkDeviceMemory wb_mem = VK_NULL_HANDLE;
	VkImageView wb_all = VK_NULL_HANDLE, wb_lvl[6] = {};
	uint32_t wb_levels = 0;
	VkSampler wb_smp = VK_NULL_HANDLE;
	VkSampler wb_smp0 = VK_NULL_HANDLE;   // v7.21: bilinear, no mips (the fast pyramid's level-0 reads)
	std::atomic<bool> wb_fast{true};      // v7.21: 3 x 3 bilinear taps per pyramid output instead of 6 x 6 fetches
	VkDescriptorPool wb_pool = VK_NULL_HANDLE;
	VkDescriptorSetLayout wb_dsl_d = VK_NULL_HANDLE, wb_dsl_a = VK_NULL_HANDLE;
	VkPipelineLayout wb_pl_d = VK_NULL_HANDLE, wb_pl_a = VK_NULL_HANDLE;
	VkShaderModule wb_sh_d = VK_NULL_HANDLE, wb_sh_a = VK_NULL_HANDLE;
	VkPipeline wb_pipe_d = VK_NULL_HANDLE, wb_pipe_a = VK_NULL_HANDLE;
	VkDescriptorSet wb_set_d[6] = {}, wb_set_a = VK_NULL_HANDLE;
	bool wb_ok = false;
	std::atomic<float> pf_deblock{0.0f}, pf_sharpen{0.0f}, pf_r0{0.35f}, pf_r1{0.8f};   // pf_deblock = wide views
	std::atomic<float> pf_deblock_inset{0.0f}, pf_wide_step{2.0f};
	std::atomic<int> pf_mode{0};
	std::atomic<int> pf_coarse{2};      // v7.10: seam grids: 0 = 16 px only, 1 = +32, 2 = +32 +64
	std::atomic<int> pf_bpp_scale{1};   // v7.10: 1 = deblock strength follows bits/pixel, 0 = always full strength   // v7.9: 0 = seam deblock (in place, keeps texture), 1 = smoothing deblock (v7.8)
	VkDescriptorSetLayout sm_dsl = VK_NULL_HANDLE;
	VkPipelineLayout sm_pl = VK_NULL_HANDLE;
	VkPipeline sm_pipe = VK_NULL_HANDLE;
	VkDescriptorSet sm_dset = VK_NULL_HANDLE;
	VkShaderModule sm_shader = VK_NULL_HANDLE;
	bool sm_ok = false;
	float pf_bpp_factor = 1.0f;
	bool pf_ok = false;
	Plane &out_y() { return bits == 8 && !direct8 && !rgba ? y8 : y; }
	Plane &out_c() { return bits == 8 ? c8 : cbcr; }
	VkFormat surface_format() const
	{
		return bits == 8 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
	}

	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkDescriptorSet dset = VK_NULL_HANDLE;
	VkShaderModule shader = VK_NULL_HANDLE;

	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkQueryPool qpool = VK_NULL_HANDLE;
	bool timestamps = false;

	std::vector<Target> targets;

	// Pipelined decode: up to kSlots frames in flight (submit on one thread, wait on another).
	static constexpr unsigned kSlots = 2;
	struct Slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkQueryPool qpool = VK_NULL_HANDLE;
		bool busy = false;
		bool complete = false, direct = false, luma_direct = false;
		int output = 0;
		bool c444 = false;
		std::chrono::steady_clock::time_point t0;
	};
	Slot slots[kSlots];
	std::mutex smu;
	std::condition_variable scv;
	uint64_t next_ticket = 0;

	uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) const
	{
		for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
				return i;
		return UINT32_MAX;
	}

	bool create_plane(Plane &p, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, std::string *err)
	{
		p.format = fmt;
		p.w = w;
		p.h = h;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fmt;
		ii.extent = {w, h, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = usage;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkResult r = vkCreateImage(device, &ii, nullptr, &p.image);
		if (r)
			return fail(err, "vkCreateImage(plane)", r);
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(device, p.image, &mr);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (ai.memoryTypeIndex == UINT32_MAX)
			return fail(err, "plane memory type");
		if ((r = vkAllocateMemory(device, &ai, nullptr, &p.mem)))
			return fail(err, "vkAllocateMemory(plane)", r);
		if ((r = vkBindImageMemory(device, p.image, p.mem, 0)))
			return fail(err, "vkBindImageMemory(plane)", r);
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = p.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if ((r = vkCreateImageView(device, &vi, nullptr, &p.view)))
			return fail(err, "vkCreateImageView(plane)", r);
		return true;
	}

	// v7.13: resources for the wide-view blur. Optional: false just leaves the blur off.
	bool create_wideblur(uint32_t T)
	{
		const uint32_t half = T / 2;
		wb_levels = 0;
		for (uint32_t s = half; s >= 8 && wb_levels < 6; s /= 2)   // v7.14: 6 levels (ramped blur up to ~11 px)
			wb_levels++;
		if (wb_levels < 4)
			return false;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_R16_SFLOAT;
		ii.extent = {half, half, 1};
		ii.mipLevels = wb_levels;
		ii.arrayLayers = 2;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (vkCreateImage(device, &ii, nullptr, &wb_img) != VK_SUCCESS)
			return false;
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(device, wb_img, &mr);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &ai, nullptr, &wb_mem) != VK_SUCCESS ||
		    vkBindImageMemory(device, wb_img, wb_mem, 0) != VK_SUCCESS)
			return false;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = wb_img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		vi.format = VK_FORMAT_R16_SFLOAT;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, wb_levels, 0, 2};
		if (vkCreateImageView(device, &vi, nullptr, &wb_all) != VK_SUCCESS)
			return false;
		{   // v7.17: history for headset stability
			VkImageCreateInfo hi = ii;
			hi.extent = {T, T, 1};
			hi.mipLevels = 1;
			hi.usage = VK_IMAGE_USAGE_STORAGE_BIT;
			VkMemoryRequirements hr;
			if (vkCreateImage(device, &hi, nullptr, &wb_hist) != VK_SUCCESS)
				return false;
			vkGetImageMemoryRequirements(device, wb_hist, &hr);
			VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			hai.allocationSize = hr.size;
			hai.memoryTypeIndex = find_mem(hr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (hai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &hai, nullptr, &wb_hist_mem) != VK_SUCCESS ||
			    vkBindImageMemory(device, wb_hist, wb_hist_mem, 0) != VK_SUCCESS)
				return false;
			VkImageViewCreateInfo hv = vi;
			hv.image = wb_hist;
			hv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
			if (vkCreateImageView(device, &hv, nullptr, &wb_hist_view) != VK_SUCCESS)
				return false;
			wb_hist_inited = wb_hist_ok = false;
		}
		for (uint32_t l = 0; l < wb_levels; l++)
		{
			vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 2};
			if (vkCreateImageView(device, &vi, nullptr, &wb_lvl[l]) != VK_SUCCESS)
				return false;
		}
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.maxLod = float(wb_levels - 1);
		if (vkCreateSampler(device, &si, nullptr, &wb_smp) != VK_SUCCESS)
			return false;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.maxLod = 0.0f;
		if (vkCreateSampler(device, &si, nullptr, &wb_smp0) != VK_SUCCESS)
			return false;
		VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12},
		                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
		                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 13}};   // v7.21: + 2 per level
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = 7;
		dpi.poolSizeCount = 3;
		dpi.pPoolSizes = ps;
		if (vkCreateDescriptorPool(device, &dpi, nullptr, &wb_pool) != VK_SUCCESS)
			return false;
		VkDescriptorSetLayoutBinding bd[5] = {
		    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},    // v7.21
		    {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};   // v7.21
		VkDescriptorSetLayoutBinding ba[3] = {
		    {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};   // v7.17 history
		VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dl.bindingCount = 5;
		dl.pBindings = bd;
		if (vkCreateDescriptorSetLayout(device, &dl, nullptr, &wb_dsl_d) != VK_SUCCESS)
			return false;
		dl.bindingCount = 3;   // v7.17: + history
		dl.pBindings = ba;
		if (vkCreateDescriptorSetLayout(device, &dl, nullptr, &wb_dsl_a) != VK_SUCCESS)
			return false;
		auto make = [&](VkDescriptorSetLayout dsl, uint32_t pcsize, const uint32_t *code, size_t codesize,
		                VkPipelineLayout *pl, VkShaderModule *sm, VkPipeline *pipe) {
			VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pcsize};
			VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pli.setLayoutCount = 1;
			pli.pSetLayouts = &dsl;
			pli.pushConstantRangeCount = 1;
			pli.pPushConstantRanges = &pr;
			if (vkCreatePipelineLayout(device, &pli, nullptr, pl) != VK_SUCCESS)
				return false;
			VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			smi.codeSize = codesize;
			smi.pCode = code;
			if (vkCreateShaderModule(device, &smi, nullptr, sm) != VK_SUCCESS)
				return false;
			VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			cp.stage.module = *sm;
			cp.stage.pName = "main";
			cp.layout = *pl;
			return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, pipe) == VK_SUCCESS;
		};
		if (!make(wb_dsl_d, 12, slp_wideblur_down_spv, sizeof(slp_wideblur_down_spv), &wb_pl_d, &wb_sh_d, &wb_pipe_d) ||
		    !make(wb_dsl_a, 96, rgba ? slp_wideblur_apply_r16_spv : slp_wideblur_apply_spv,
		          rgba ? sizeof(slp_wideblur_apply_r16_spv) : sizeof(slp_wideblur_apply_spv), &wb_pl_a, &wb_sh_a, &wb_pipe_a))
			return false;
		VkDescriptorSet sets[7] = {};
		VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		da.descriptorPool = wb_pool;
		da.descriptorSetCount = wb_levels + 1;
		VkDescriptorSetLayout lay2[7];
		for (uint32_t i = 0; i < wb_levels; i++)
			lay2[i] = wb_dsl_d;
		lay2[wb_levels] = wb_dsl_a;
		da.pSetLayouts = lay2;
		if (vkAllocateDescriptorSets(device, &da, sets) != VK_SUCCESS)
			return false;
		const Plane &src = out_y();
		for (uint32_t l = 0; l < wb_levels; l++)
		{
			wb_set_d[l] = sets[l];
			VkDescriptorImageInfo ii3[5] = {{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_GENERAL},
			                                {VK_NULL_HANDLE, wb_all, VK_IMAGE_LAYOUT_GENERAL},
			                                {VK_NULL_HANDLE, wb_lvl[l], VK_IMAGE_LAYOUT_GENERAL},
			                                {wb_smp0, src.view, VK_IMAGE_LAYOUT_GENERAL},
			                                {wb_smp, wb_all, VK_IMAGE_LAYOUT_GENERAL}};
			VkWriteDescriptorSet w[5] = {};
			for (int i = 0; i < 5; i++)
			{
				w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				w[i].dstSet = wb_set_d[l];
				w[i].dstBinding = uint32_t(i);
				w[i].descriptorCount = 1;
				w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
				                    : i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w[i].pImageInfo = &ii3[i];
			}
			vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
		}
		wb_set_a = sets[wb_levels];
		VkDescriptorImageInfo ia[3] = {{VK_NULL_HANDLE, src.view, VK_IMAGE_LAYOUT_GENERAL},
		                               {wb_smp, wb_all, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, wb_hist_view, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet wa[3] = {};
		for (int i = 0; i < 3; i++)
		{
			wa[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			wa[i].dstSet = wb_set_a;
			wa[i].dstBinding = uint32_t(i);
			wa[i].descriptorCount = 1;
			wa[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			wa[i].pImageInfo = &ia[i];
		}
		vkUpdateDescriptorSets(device, 3, wa, 0, nullptr);
		return true;
	}

	void destroy_plane(Plane &p)
	{
		if (p.view) vkDestroyImageView(device, p.view, nullptr);
		if (p.image) vkDestroyImage(device, p.image, nullptr);
		if (p.mem) vkFreeMemory(device, p.mem, nullptr);
		p = Plane();
	}

	static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to,
	                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
	                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
	                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
	                    uint32_t src_family = VK_QUEUE_FAMILY_IGNORED, uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED)
	{
		VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = src_stage;
		b.srcAccessMask = src_access;
		b.dstStageMask = dst_stage;
		b.dstAccessMask = dst_access;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = src_family;
		b.dstQueueFamilyIndex = dst_family;
		b.image = img;
		b.subresourceRange = {aspect, 0, 1, 0, 1};
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2(c, &di);
	}

	VkImageCreateInfo client_image_info(VkImageDrmFormatModifierExplicitCreateInfoEXT *mod, VkSubresourceLayout *layouts,
	                                    VkExternalMemoryImageCreateInfo *ext, const TargetDesc &d, VkImageUsageFlags usage) const
	{
		for (int i = 0; i < 2; i++)
		{
			layouts[i] = {};
			layouts[i].offset = d.plane_offset[i];
			layouts[i].rowPitch = d.plane_pitch[i];
		}
		*ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext->handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		*mod = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
		mod->pNext = ext;
		mod->drmFormatModifier = d.modifier;
		mod->drmFormatModifierPlaneCount = 2;
		mod->pPlaneLayouts = layouts;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = mod;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = surface_format();
		ii.extent = {uint32_t(width), uint32_t(height), 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ii.usage = usage;
		ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return ii;
	}

	// One single-plane image on the client dma-buf: format fmt, UBWC modifier, plane data at mem offset `offset`.
	bool alias_image(VkFormat fmt, uint32_t w, uint32_t h, uint64_t modifier, uint64_t pitch, VkImage *img, std::string *err)
	{
		VkSubresourceLayout lay = {};
		lay.offset = 0;
		lay.rowPitch = pitch;
		VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
		mod.pNext = &ext;
		mod.drmFormatModifier = modifier;
		mod.drmFormatModifierPlaneCount = 1;
		mod.pPlaneLayouts = &lay;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &mod;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fmt;
		ii.extent = {w, h, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
		ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkResult r = vkCreateImage(device, &ii, nullptr, img);
		if (r)
			return fail(err, fmt == VK_FORMAT_R8_UNORM ? "vkCreateImage(alias R8 UBWC storage)" : "vkCreateImage(alias R8G8 UBWC storage)", r);
		return true;
	}

	// Modifier tiling features for a single-plane format (logged by the self-test).
	VkFormatFeatureFlags mod_features(VkFormat fmt, uint64_t modifier) const
	{
		VkDrmFormatModifierPropertiesListEXT l = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
		VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
		fp.pNext = &l;
		vkGetPhysicalDeviceFormatProperties2(gpu, fmt, &fp);
		std::vector<VkDrmFormatModifierPropertiesEXT> v(l.drmFormatModifierCount);
		l.pDrmFormatModifierProperties = v.data();
		vkGetPhysicalDeviceFormatProperties2(gpu, fmt, &fp);
		for (auto &e : v)
			if (e.drmFormatModifier == modifier)
				return e.drmFormatModifierTilingFeatures;
		return 0;
	}

	// v7.21: memory planes of a modifier (0 = not offered for this format)
	uint32_t mod_planes(VkFormat fmt, uint64_t modifier) const
	{
		VkDrmFormatModifierPropertiesListEXT l = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
		VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
		fp.pNext = &l;
		vkGetPhysicalDeviceFormatProperties2(gpu, fmt, &fp);
		std::vector<VkDrmFormatModifierPropertiesEXT> v(l.drmFormatModifierCount);
		l.pDrmFormatModifierProperties = v.data();
		vkGetPhysicalDeviceFormatProperties2(gpu, fmt, &fp);
		for (auto &e : v)
			if (e.drmFormatModifier == modifier)
				return e.drmFormatModifierPlaneCount;
		return 0;
	}

	bool create_alias(Target &t, const TargetDesc &d, std::string *err);
	void destroy_alias(Target &t);
	bool create_pack(std::string *err);              // v7.19 full colour
	bool create_rgba(Target &t, std::string *err);
	void destroy_rgba(Target &t);

	double ts_ms(uint64_t a, uint64_t b) const
	{
		return double(b - a) * double(props.limits.timestampPeriod) * 1e-6;
	}
};

Decoder::Decoder() : impl(new Impl) {}

Decoder::~Decoder()
{
	Impl &m = *impl;
	if (m.device)
		vkDeviceWaitIdle(m.device);
	for (size_t i = 0; i < m.targets.size(); i++)
		release_target(int(i));
	if (m.pdec) pyrowave_decoder_destroy(m.pdec);
	if (m.pdec4) pyrowave_decoder_destroy(m.pdec4);
	if (m.pdev) pyrowave_device_destroy(m.pdev);
	if (m.device)
	{
		m.destroy_plane(m.y);
		m.destroy_plane(m.cb);
		m.destroy_plane(m.cr);
		m.destroy_plane(m.cbcr);
		m.destroy_plane(m.y8);
		m.destroy_plane(m.c8);
		m.destroy_plane(m.yf);
		m.destroy_plane(m.cb4);
		m.destroy_plane(m.cr4);
		if (m.pk_pipe) vkDestroyPipeline(m.device, m.pk_pipe, nullptr);
		if (m.pkx_pipe) vkDestroyPipeline(m.device, m.pkx_pipe, nullptr);
		if (m.pkx_pl) vkDestroyPipelineLayout(m.device, m.pkx_pl, nullptr);
		if (m.pkx_dsl) vkDestroyDescriptorSetLayout(m.device, m.pkx_dsl, nullptr);
		if (m.pkx_sh) vkDestroyShaderModule(m.device, m.pkx_sh, nullptr);
		if (m.pk_pl) vkDestroyPipelineLayout(m.device, m.pk_pl, nullptr);
		if (m.pk_dsl) vkDestroyDescriptorSetLayout(m.device, m.pk_dsl, nullptr);
		if (m.pk_sh) vkDestroyShaderModule(m.device, m.pk_sh, nullptr);
		if (m.pk_pool) vkDestroyDescriptorPool(m.device, m.pk_pool, nullptr);
		if (m.pk_smp) vkDestroySampler(m.device, m.pk_smp, nullptr);
		if (m.sm_pipe) vkDestroyPipeline(m.device, m.sm_pipe, nullptr);
		if (m.sm_pl) vkDestroyPipelineLayout(m.device, m.sm_pl, nullptr);
		if (m.sm_dsl) vkDestroyDescriptorSetLayout(m.device, m.sm_dsl, nullptr);
		if (m.sm_shader) vkDestroyShaderModule(m.device, m.sm_shader, nullptr);
		if (m.wb_pipe_d) vkDestroyPipeline(m.device, m.wb_pipe_d, nullptr);
		if (m.wb_pipe_a) vkDestroyPipeline(m.device, m.wb_pipe_a, nullptr);
		if (m.wb_sh_d) vkDestroyShaderModule(m.device, m.wb_sh_d, nullptr);
		if (m.wb_sh_a) vkDestroyShaderModule(m.device, m.wb_sh_a, nullptr);
		if (m.wb_pl_d) vkDestroyPipelineLayout(m.device, m.wb_pl_d, nullptr);
		if (m.wb_pl_a) vkDestroyPipelineLayout(m.device, m.wb_pl_a, nullptr);
		if (m.wb_dsl_d) vkDestroyDescriptorSetLayout(m.device, m.wb_dsl_d, nullptr);
		if (m.wb_dsl_a) vkDestroyDescriptorSetLayout(m.device, m.wb_dsl_a, nullptr);
		if (m.wb_pool) vkDestroyDescriptorPool(m.device, m.wb_pool, nullptr);
		if (m.wb_smp) vkDestroySampler(m.device, m.wb_smp, nullptr);
		if (m.wb_smp0) vkDestroySampler(m.device, m.wb_smp0, nullptr);
		for (VkImageView v : m.wb_lvl)
			if (v) vkDestroyImageView(m.device, v, nullptr);
		if (m.wb_all) vkDestroyImageView(m.device, m.wb_all, nullptr);
		if (m.wb_img) vkDestroyImage(m.device, m.wb_img, nullptr);
		if (m.wb_hist_view) vkDestroyImageView(m.device, m.wb_hist_view, nullptr);
		if (m.wb_hist) vkDestroyImage(m.device, m.wb_hist, nullptr);
		if (m.wb_hist_mem) vkFreeMemory(m.device, m.wb_hist_mem, nullptr);
		if (m.wb_mem) vkFreeMemory(m.device, m.wb_mem, nullptr);
		if (m.sh_pipe) vkDestroyPipeline(m.device, m.sh_pipe, nullptr);
		if (m.sh_shader) vkDestroyShaderModule(m.device, m.sh_shader, nullptr);
		if (m.pf_pipe) vkDestroyPipeline(m.device, m.pf_pipe, nullptr);
		if (m.pf_pl) vkDestroyPipelineLayout(m.device, m.pf_pl, nullptr);
		if (m.pf_dsl) vkDestroyDescriptorSetLayout(m.device, m.pf_dsl, nullptr);
		if (m.pf_shader) vkDestroyShaderModule(m.device, m.pf_shader, nullptr);
		if (m.pipe) vkDestroyPipeline(m.device, m.pipe, nullptr);
		if (m.pl) vkDestroyPipelineLayout(m.device, m.pl, nullptr);
		if (m.dpool) vkDestroyDescriptorPool(m.device, m.dpool, nullptr);
		if (m.dsl) vkDestroyDescriptorSetLayout(m.device, m.dsl, nullptr);
		if (m.shader) vkDestroyShaderModule(m.device, m.shader, nullptr);
		if (m.qpool) vkDestroyQueryPool(m.device, m.qpool, nullptr);
		for (auto &sl : m.slots)
		{
			if (sl.qpool) vkDestroyQueryPool(m.device, sl.qpool, nullptr);
			if (sl.fence) vkDestroyFence(m.device, sl.fence, nullptr);
		}
		if (m.fence) vkDestroyFence(m.device, m.fence, nullptr);
		if (m.pool) vkDestroyCommandPool(m.device, m.pool, nullptr);
		vkDestroyDevice(m.device, nullptr);
	}
	if (m.instance)
		vkDestroyInstance(m.instance, nullptr);
	delete impl;
}

void Decoder::set_dither(bool on)
{
	impl->dither = on ? 1.0f : 0.0f;
}

void Decoder::set_postfx(float deblock, float sharpen, float r0, float r1)
{
	impl->pf_deblock = std::max(0.0f, std::min(4.0f, deblock));
	impl->pf_sharpen = std::max(0.0f, std::min(1.0f, sharpen));
	impl->pf_r0 = std::max(0.0f, std::min(2.0f, r0));
	impl->pf_r1 = std::max(impl->pf_r0.load() + 0.01f, std::min(2.0f, r1));
}

void Decoder::set_postfx_mode(int mode)
{
	impl->pf_mode = mode == 1 ? 1 : 0;
}

void Decoder::set_postfx_seam(int coarse, bool bpp_scale)
{
	impl->pf_coarse = std::max(0, std::min(2, coarse));
	impl->pf_bpp_scale = bpp_scale ? 1 : 0;
}

void Decoder::set_ramp(const float r[16])
{
	if (!impl)
		return;
	for (int i = 0; i < 16; i++)
		if (!std::isfinite(r[i]))
			return;
	std::lock_guard<std::mutex> lk(impl->ramp_mu);
	memcpy(impl->ramp, r, sizeof(impl->ramp));
	// keep the projections sane (a zero slope would divide by zero in the shaders)
	for (int e = 0; e < 2; e++)
	{
		float *q = impl->ramp + 4 + 6 * e;
		if (std::fabs(q[2]) < 0.05f) q[2] = 0.32f;
		if (std::fabs(q[4]) < 0.05f) q[4] = -0.33f;
	}
	impl->ramp[0] = std::min(1.0f, std::max(0.0f, impl->ramp[0]));
	impl->ramp[3] = std::min(1.0f, std::max(0.0f, impl->ramp[3]));
}

void Decoder::set_postfx_temporal(float amount, float clamp_width)
{
	if (!impl || !std::isfinite(amount) || !std::isfinite(clamp_width))
		return;
	impl->wb_temporal = std::min(1.0f, std::max(0.0f, amount));
	impl->wb_tclamp = std::min(4.0f, std::max(0.0f, clamp_width));
}

void Decoder::set_postfx_blur(float px, float contrast)
{
	impl->wb_px = std::max(0.0f, std::min(8.0f, px));
	impl->wb_contrast = std::max(0.0f, std::min(3.0f, contrast));
}

bool Decoder::postfx_blur_available() const
{
	return impl->wb_ok;
}

void Decoder::set_postfx_light(bool on)
{
	impl->sh_on = on;
}

void Decoder::set_postfx2(float deblock_wide, float deblock_inset, float sharpen, float r0, float r1, float wide_step)
{
	set_postfx(deblock_wide, sharpen, r0, r1);
	impl->pf_deblock_inset = std::max(0.0f, std::min(4.0f, deblock_inset));
	impl->pf_wide_step = wide_step > 1.5f ? 2.0f : 1.0f;
}

bool Decoder::postfx_available() const
{
	return impl->pf_ok;
}

void Decoder::set_preserve_surface_layout(bool preserve)
{
	impl->preserve_layout = preserve;
}

bool Decoder::pyrowave_dither_available() const
{
	return impl->pyro_dither;
}

std::string Decoder::alias_status() const
{
	if (!impl->alias && !impl->rgba)
		return "alias not requested";
	return impl->alias_note.empty() ? "no surface imported yet" : impl->alias_note;
}

std::string Decoder::device_name() const
{
	return impl->props.deviceName;
}

void *Decoder::pyrowave_device_handle() const
{
	return impl->pdev;
}

bool Decoder::init(int width, int height, int bits, std::string *err, int mode)
{
	Impl &m = *impl;
	m.bits = bits == 8 ? 8 : 10;
	m.mode = (m.bits == 8 && mode >= 0 && mode <= 6) ? mode : 0;
	m.rgba = m.mode == kModeFullColour;
	m.rgba_compressed = g_default_rgba_compressed.load();
	m.direct8 = m.mode == kModeDirect8 || m.mode == kModeDirect8Alias || m.mode == kModeDirect8ChromaDirect ||
	            m.mode == kModeDirect8StorageLuma;
	m.alias = m.mode >= kModeDitherAlias && m.mode <= kModeDirect8StorageLuma;
	m.pyro_dither = pyrowave_slp_has_output_dither();
	m.width = width;
	m.height = height;
	m.targets.reserve(64);   // v7.19: no reallocation under rgba_export on another thread
	VkResult r = volkInitialize();
	if (r)
		return fail(err, "volkInitialize (libvulkan.so.1)", r);

	m.app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	m.app.pApplicationName = "slp-pyrowave-decoder";
	m.app.pEngineName = "slp";
	m.app.apiVersion = VK_API_VERSION_1_3;
	m.ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	m.ici.pApplicationInfo = &m.app;
	if ((r = vkCreateInstance(&m.ici, nullptr, &m.instance)))
		return fail(err, "vkCreateInstance", r);
	volkLoadInstanceOnly(m.instance);

	uint32_t n = 0;
	vkEnumeratePhysicalDevices(m.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> gpus(n);
	vkEnumeratePhysicalDevices(m.instance, &n, gpus.data());
	for (auto g : gpus)
	{
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(g, &p);
		if (p.apiVersion >= VK_API_VERSION_1_3)
		{
			m.gpu = g;
			m.props = p;
			break;
		}
	}
	if (!m.gpu)
		return fail(err, "no Vulkan 1.3 GPU");
	vkGetPhysicalDeviceMemoryProperties(m.gpu, &m.memprops);

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m.gpu, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(m.gpu, &nq, qf.data());
	m.family = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
		{
			m.family = i;
			break;
		}
	if (m.family == UINT32_MAX)
		return fail(err, "no graphics+compute queue family");
	m.timestamps = qf[m.family].timestampValidBits != 0 && m.props.limits.timestampPeriod > 0;

	// Enable every supported core feature except robust buffer access (costly, not needed).
	m.f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	m.f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	m.f12.pNext = &m.f13;
	m.f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	m.f11.pNext = &m.f12;
	m.f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	m.f2.pNext = &m.f11;
	vkGetPhysicalDeviceFeatures2(m.gpu, &m.f2);
	m.f2.features.robustBufferAccess = VK_FALSE;
	m.f13.robustImageAccess = VK_FALSE;

	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(m.gpu, nullptr, &ne, nullptr);
	std::vector<VkExtensionProperties> exts(ne);
	vkEnumerateDeviceExtensionProperties(m.gpu, nullptr, &ne, exts.data());
	auto has = [&](const char *name) {
		for (auto &e : exts)
			if (!strcmp(e.extensionName, name))
				return true;
		return false;
	};
	const char *want[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
	};
	for (const char *w : want)
		if (has(w))
			m.dev_ext_store.emplace_back(w);
	for (auto &s : m.dev_ext_store)
		m.dev_exts.push_back(s.c_str());
	m.has_foreign = has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	m.has_modifiers = has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) && has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);

	m.qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	m.qci.queueFamilyIndex = m.family;
	m.qci.queueCount = 1;
	m.qci.pQueuePriorities = &m.prio;
	m.dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	m.dci.pNext = &m.f2;
	m.dci.queueCreateInfoCount = 1;
	m.dci.pQueueCreateInfos = &m.qci;
	m.dci.enabledExtensionCount = uint32_t(m.dev_exts.size());
	m.dci.ppEnabledExtensionNames = m.dev_exts.data();
	if ((r = vkCreateDevice(m.gpu, &m.dci, nullptr, &m.device)))
		return fail(err, "vkCreateDevice", r);
	volkLoadDevice(m.device);
	vkGetDeviceQueue(m.device, m.family, 0, &m.queue);

	pyrowave_device_create_info pci = {};
	pci.GetInstanceProcAddr = vkGetInstanceProcAddr;
	pci.instance = m.instance;
	pci.physical_device = m.gpu;
	pci.device = m.device;
	pci.instance_create_info = &m.ici;
	pci.device_create_info = &m.dci;
	m.pq.queue = m.queue;
	m.pq.familyIndex = m.family;
	m.pq.index = 0;
	pci.queue_info = &m.pq;
	pci.queue_info_count = 1;
	if (pyrowave_create_device(&pci, &m.pdev) != PYROWAVE_SUCCESS)
		return fail(err, "pyrowave_create_device");
	pyrowave_device_set_queue_type(m.pdev, VK_QUEUE_COMPUTE_BIT);

	pyrowave_decoder_create_info dinfo = {};
	dinfo.device = m.pdev;
	dinfo.width = width;
	dinfo.height = height;
	dinfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	dinfo.fragment_path = false; // compute was faster on Adreno 750 (round 3)
	if (pyrowave_decoder_create(&dinfo, &m.pdec) != PYROWAVE_SUCCESS)
		return fail(err, "pyrowave_decoder_create");
	if (m.rgba)
	{
		dinfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
		if (pyrowave_decoder_create(&dinfo, &m.pdec4) != PYROWAVE_SUCCESS)
			return fail(err, "pyrowave_decoder_create(4:4:4)");
	}

	const VkImageUsageFlags pu = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const VkFormat dfmt = m.direct8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16_UNORM;
	if (!m.create_plane(m.y, dfmt, width, height, pu, err) ||
	    !m.create_plane(m.cb, dfmt, width / 2, height / 2, pu, err) ||
	    !m.create_plane(m.cr, dfmt, width / 2, height / 2, pu, err) ||
	    !m.create_plane(m.cbcr, VK_FORMAT_R16G16_UNORM, width / 2, height / 2, pu, err))
		return false;
	if (m.rgba && (!m.create_plane(m.cb4, VK_FORMAT_R16_UNORM, width, height, pu, err) ||
	               !m.create_plane(m.cr4, VK_FORMAT_R16_UNORM, width, height, pu, err)))
		return false;
	if (m.bits == 8 &&
	    ((!m.direct8 && !m.create_plane(m.y8, VK_FORMAT_R8_UNORM, width, height, pu, err)) ||
	     !m.create_plane(m.c8, VK_FORMAT_R8G8_UNORM, width / 2, height / 2, pu, err)))
		return false;

	// Finishing pipeline: 10-bit = chroma pack (Cb, Cr -> R16G16); 8-bit = dithered Y8 + CbCr8.
	// direct8: chroma pack into R8G8 only (luma is copied straight from Pyrowave's R8 output).
	const bool b8 = m.bits == 8 && !m.direct8;
	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = b8 ? sizeof(slp_finish8_spv) : m.direct8 ? sizeof(slp_chroma_pack8_spv) : sizeof(slp_chroma_pack_spv);
	smi.pCode = b8 ? slp_finish8_spv : m.direct8 ? slp_chroma_pack8_spv : slp_chroma_pack_spv;
	if ((r = vkCreateShaderModule(m.device, &smi, nullptr, &m.shader)))
		return fail(err, "vkCreateShaderModule", r);
	const uint32_t nbind = b8 ? 5 : 3, nsampled = b8 ? 3 : 2;
	VkDescriptorSetLayoutBinding bind[5];
	for (uint32_t i = 0; i < nbind; i++)
		bind[i] = {i, i < nsampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
		           VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = nbind;
	dli.pBindings = bind;
	if ((r = vkCreateDescriptorSetLayout(m.device, &dli, nullptr, &m.dsl)))
		return fail(err, "vkCreateDescriptorSetLayout", r);
	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};   // v7.19: dither, chroma step
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &m.dsl;
	pli.pushConstantRangeCount = b8 ? 1 : 0;
	pli.pPushConstantRanges = &pcr;
	if ((r = vkCreatePipelineLayout(m.device, &pli, nullptr, &m.pl)))
		return fail(err, "vkCreatePipelineLayout", r);
	VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = m.shader;
	cpi.stage.pName = "main";
	cpi.layout = m.pl;
	if ((r = vkCreateComputePipelines(m.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &m.pipe)))
		return fail(err, "vkCreateComputePipelines", r);
	// One set for the intermediate planes + one per alias target.
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * 65}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 65}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	dpi.maxSets = 65;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = ps;
	if ((r = vkCreateDescriptorPool(m.device, &dpi, nullptr, &m.dpool)))
		return fail(err, "vkCreateDescriptorPool", r);
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = m.dpool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &m.dsl;
	if ((r = vkAllocateDescriptorSets(m.device, &dai, &m.dset)))
		return fail(err, "vkAllocateDescriptorSets", r);
	VkDescriptorImageInfo dii[5];
	if (b8)
	{
		VkImageView v[5] = {m.y.view, m.cb.view, m.cr.view, m.y8.view, m.c8.view};
		for (int i = 0; i < 5; i++)
			dii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
	}
	else
	{
		VkImageView v[3] = {m.cb.view, m.cr.view, m.direct8 ? m.c8.view : m.cbcr.view};
		for (int i = 0; i < 3; i++)
			dii[i] = {VK_NULL_HANDLE, v[i], VK_IMAGE_LAYOUT_GENERAL};
	}
	VkWriteDescriptorSet wr[5] = {};
	for (uint32_t i = 0; i < nbind; i++)
	{
		wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr[i].dstSet = m.dset;
		wr[i].dstBinding = i;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = i < nsampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		wr[i].pImageInfo = &dii[i];
	}
	vkUpdateDescriptorSets(m.device, nbind, wr, 0, nullptr);
	if (m.rgba)
	{
		// v7.19: finish8 for 4:4:4 frames (NV12 output): same set with the full-size chroma planes
		if ((r = vkAllocateDescriptorSets(m.device, &dai, &m.dset444)))
			return fail(err, "vkAllocateDescriptorSets(4:4:4)", r);
		dii[1].imageView = m.cb4.view;
		dii[2].imageView = m.cr4.view;
		for (uint32_t i = 0; i < nbind; i++)
			wr[i].dstSet = m.dset444;
		vkUpdateDescriptorSets(m.device, nbind, wr, 0, nullptr);
	}

	// v7.5 post-filter: out_y() (R8) -> yf (R8); only for 8-bit surfaces. A failure here only disables the filter.
	if (m.bits == 8 && (height % 4) == 0 && ((height / 4) % 16) == 0)
	{
		// v7.19 full colour: 16-bit luma, *_r16 shader variants
		bool ok = m.create_plane(m.yf, m.rgba ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM, width, height, pu, err);
		VkShaderModuleCreateInfo psm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		psm.codeSize = m.rgba ? sizeof(slp_postfx_r16_spv) : sizeof(slp_postfx_spv);
		psm.pCode = m.rgba ? slp_postfx_r16_spv : slp_postfx_spv;
		ok = ok && vkCreateShaderModule(m.device, &psm, nullptr, &m.pf_shader) == VK_SUCCESS;
		VkDescriptorSetLayoutBinding pb[2] = {
			{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
		VkDescriptorSetLayoutCreateInfo pdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		pdl.bindingCount = 2;
		pdl.pBindings = pb;
		ok = ok && vkCreateDescriptorSetLayout(m.device, &pdl, nullptr, &m.pf_dsl) == VK_SUCCESS;
		VkPushConstantRange ppc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 36};
		VkPipelineLayoutCreateInfo ppl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		ppl.setLayoutCount = 1;
		ppl.pSetLayouts = &m.pf_dsl;
		ppl.pushConstantRangeCount = 1;
		ppl.pPushConstantRanges = &ppc;
		ok = ok && vkCreatePipelineLayout(m.device, &ppl, nullptr, &m.pf_pl) == VK_SUCCESS;
		VkComputePipelineCreateInfo pcp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		pcp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		pcp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pcp.stage.module = m.pf_shader;
		pcp.stage.pName = "main";
		pcp.layout = m.pf_pl;
		ok = ok && vkCreateComputePipelines(m.device, VK_NULL_HANDLE, 1, &pcp, nullptr, &m.pf_pipe) == VK_SUCCESS;
		VkDescriptorSetAllocateInfo pda = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		pda.descriptorPool = m.dpool;
		pda.descriptorSetCount = 1;
		pda.pSetLayouts = &m.pf_dsl;
		ok = ok && vkAllocateDescriptorSets(m.device, &pda, &m.pf_dset) == VK_SUCCESS;
		if (ok)
		{
			VkDescriptorImageInfo pi[2] = {{VK_NULL_HANDLE, m.out_y().view, VK_IMAGE_LAYOUT_GENERAL},
			                               {VK_NULL_HANDLE, m.yf.view, VK_IMAGE_LAYOUT_GENERAL}};
			VkWriteDescriptorSet pw[2] = {};
			for (int i = 0; i < 2; i++)
			{
				pw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				pw[i].dstSet = m.pf_dset;
				pw[i].dstBinding = uint32_t(i);
				pw[i].descriptorCount = 1;
				pw[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				pw[i].pImageInfo = &pi[i];
			}
			vkUpdateDescriptorSets(m.device, 2, pw, 0, nullptr);
		}
		if (ok)
		{
			// v7.12: light sharpen pipeline; optional (falls back to postfx.comp when it cannot be created)
			VkShaderModuleCreateInfo hsm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			hsm.codeSize = m.rgba ? sizeof(slp_sharpen_r16_spv) : sizeof(slp_sharpen_spv);
			hsm.pCode = m.rgba ? slp_sharpen_r16_spv : slp_sharpen_spv;
			if (vkCreateShaderModule(m.device, &hsm, nullptr, &m.sh_shader) == VK_SUCCESS)
			{
				VkComputePipelineCreateInfo hcp = pcp;
				hcp.stage.module = m.sh_shader;
				if (vkCreateComputePipelines(m.device, VK_NULL_HANDLE, 1, &hcp, nullptr, &m.sh_pipe) != VK_SUCCESS)
					m.sh_pipe = VK_NULL_HANDLE;
			}
		}
		m.pf_ok = ok;
		if (err && !ok)
			err->clear();

		// v7.9 seam deblock: in place on out_y() (storage R8)
		bool sok = true;
		VkShaderModuleCreateInfo ssm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ssm.codeSize = m.rgba ? sizeof(slp_seam_r16_spv) : sizeof(slp_seam_spv);
		ssm.pCode = m.rgba ? slp_seam_r16_spv : slp_seam_spv;
		sok = sok && vkCreateShaderModule(m.device, &ssm, nullptr, &m.sm_shader) == VK_SUCCESS;
		VkDescriptorSetLayoutBinding sb = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo sdl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		sdl.bindingCount = 1;
		sdl.pBindings = &sb;
		sok = sok && vkCreateDescriptorSetLayout(m.device, &sdl, nullptr, &m.sm_dsl) == VK_SUCCESS;
		VkPushConstantRange spc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};   // v7.14: + ramp (seam.comp)
		VkPipelineLayoutCreateInfo spl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		spl.setLayoutCount = 1;
		spl.pSetLayouts = &m.sm_dsl;
		spl.pushConstantRangeCount = 1;
		spl.pPushConstantRanges = &spc;
		sok = sok && vkCreatePipelineLayout(m.device, &spl, nullptr, &m.sm_pl) == VK_SUCCESS;
		VkComputePipelineCreateInfo scp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		scp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		scp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		scp.stage.module = m.sm_shader;
		scp.stage.pName = "main";
		scp.layout = m.sm_pl;
		sok = sok && vkCreateComputePipelines(m.device, VK_NULL_HANDLE, 1, &scp, nullptr, &m.sm_pipe) == VK_SUCCESS;
		VkDescriptorSetAllocateInfo sda = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		sda.descriptorPool = m.dpool;
		sda.descriptorSetCount = 1;
		sda.pSetLayouts = &m.sm_dsl;
		sok = sok && vkAllocateDescriptorSets(m.device, &sda, &m.sm_dset) == VK_SUCCESS;
		if (sok)
		{
			VkDescriptorImageInfo si = {VK_NULL_HANDLE, m.out_y().view, VK_IMAGE_LAYOUT_GENERAL};
			VkWriteDescriptorSet sw = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			sw.dstSet = m.sm_dset;
			sw.dstBinding = 0;
			sw.descriptorCount = 1;
			sw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			sw.pImageInfo = &si;
			vkUpdateDescriptorSets(m.device, 1, &sw, 0, nullptr);
		}
		m.sm_ok = ok && sok;
		if (err && !sok)
			err->clear();

		// v7.13 wide-view blur: R16F mip chain (half size .. 1/32) of tiles 1 and 3 as a 2-layer array
		m.wb_ok = m.sm_ok && m.create_wideblur(uint32_t(height / 4));
		if (err && !m.wb_ok)
			err->clear();
	}
	if (m.rgba && !m.create_pack(err))
		return false;

	VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = m.family;
	if ((r = vkCreateCommandPool(m.device, &cpci, nullptr, &m.pool)))
		return fail(err, "vkCreateCommandPool", r);
	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = m.pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	if ((r = vkAllocateCommandBuffers(m.device, &cbai, &m.cmd)))
		return fail(err, "vkAllocateCommandBuffers", r);
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if ((r = vkCreateFence(m.device, &fci, nullptr, &m.fence)))
		return fail(err, "vkCreateFence", r);
	if (m.timestamps)
	{
		VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qpi.queryCount = 4;
		if (vkCreateQueryPool(m.device, &qpi, nullptr, &m.qpool) != VK_SUCCESS)
			m.timestamps = false;
	}
	for (auto &sl : m.slots)
	{
		VkCommandBufferAllocateInfo sbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		sbai.commandPool = m.pool;
		sbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		sbai.commandBufferCount = 1;
		if ((r = vkAllocateCommandBuffers(m.device, &sbai, &sl.cmd)))
			return fail(err, "vkAllocateCommandBuffers(slot)", r);
		VkFenceCreateInfo sfci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if ((r = vkCreateFence(m.device, &sfci, nullptr, &sl.fence)))
			return fail(err, "vkCreateFence(slot)", r);
		if (m.timestamps)
		{
			VkQueryPoolCreateInfo sqpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			sqpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
			sqpi.queryCount = 8;   // v7.21: per-pass timestamps
			if (vkCreateQueryPool(m.device, &sqpi, nullptr, &sl.qpool) != VK_SUCCESS)
				m.timestamps = false;
		}
	}

	// One-time: planes to GENERAL.
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(m.cmd, &bi);
	for (Plane *p : {&m.y, &m.cb, &m.cr, &m.cbcr, &m.y8, &m.c8, &m.yf, &m.cb4, &m.cr4})
		if (p->image)
		Impl::barrier(m.cmd, p->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	// Black until the first real picture (a "repeat" frame before any decode shows black, not garbage).
	{
		VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkClearColorValue black = {}, mid = {};
		mid.float32[0] = mid.float32[1] = 0.5f;
		vkCmdClearColorImage(m.cmd, m.y.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
		vkCmdClearColorImage(m.cmd, m.cb.image, VK_IMAGE_LAYOUT_GENERAL, &mid, 1, &rr);
		vkCmdClearColorImage(m.cmd, m.cr.image, VK_IMAGE_LAYOUT_GENERAL, &mid, 1, &rr);
		if (m.rgba)
		{
			vkCmdClearColorImage(m.cmd, m.cb4.image, VK_IMAGE_LAYOUT_GENERAL, &mid, 1, &rr);
			vkCmdClearColorImage(m.cmd, m.cr4.image, VK_IMAGE_LAYOUT_GENERAL, &mid, 1, &rr);
		}
		VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
		mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(m.cmd, &di);
	}
	vkEndCommandBuffer(m.cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &m.cmd;
	if ((r = vkQueueSubmit(m.queue, 1, &si, m.fence)))
		return fail(err, "vkQueueSubmit(init)", r);
	vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX);
	vkResetFences(m.device, 1, &m.fence);
	return true;
}

int Decoder::create_client_like_surface(uint64_t modifier, TargetDesc *out, std::string *err)
{
	Impl &m = *impl;
	TargetDesc d;
	d.modifier = modifier;
	// vrlink's own UBWC layouts (from its SOURCE_CHANGE handler): plane 0 = Y metadata + Y data, then plane 1.
	auto al = [](uint64_t v, uint64_t a) { return (v + a - 1) / a * a; };
	uint64_t W = uint64_t(m.width), H = uint64_t(m.height);
	if (m.bits == 8)
	{
		// Q08C / NV12 UBWC: pitch align(W,128); Y meta align(ceil(W/32),64) x align(ceil(H/8),16), 4K aligned.
		uint64_t pitch = al(W, 128);
		uint64_t meta = al(al((W + 31) / 32, 64) * al((H + 7) / 8, 16), 4096);
		d.plane_pitch[0] = d.plane_pitch[1] = pitch;
		d.plane_offset[1] = meta + pitch * al(H, 32); // 7,614,464 at 1344x5376
	}
	else
	{
		// Q10C / TP10 UBWC: pitch = 1792 at 1344 (3 px per 4 bytes); plane 1 at 9,719,808.
		uint64_t pitch = al((W + 191) / 192 * 256, 256);
		uint64_t meta = al(al((W + 47) / 48, 64) * al((H + 3) / 4, 16), 4096);
		d.plane_pitch[0] = d.plane_pitch[1] = pitch;
		d.plane_offset[1] = meta + pitch * al(H, 16);
	}
	d.plane_offset[0] = 0;
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod;
	VkSubresourceLayout layouts[2];
	VkExternalMemoryImageCreateInfo ext;
	VkImageCreateInfo ii = m.client_image_info(&mod, layouts, &ext, d, VK_IMAGE_USAGE_SAMPLED_BIT);
	VkImage img = VK_NULL_HANDLE;
	VkResult r = vkCreateImage(m.device, &ii, nullptr, &img);
	if (r)
		return fail(err, "vkCreateImage(client-like P010)", r), -1;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m.device, img, &mr);
	VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = &ex;
	ded.image = img;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &ded;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem = VK_NULL_HANDLE;
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &mem)))
		return vkDestroyImage(m.device, img, nullptr), fail(err, "vkAllocateMemory(client-like)", r), -1;
	vkBindImageMemory(m.device, img, mem, 0);
	VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = mem;
	gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	int fd = -1;
	if ((r = vkGetMemoryFdKHR(m.device, &gi, &fd)))
		return fail(err, "vkGetMemoryFdKHR", r), -1;
	d.dmabuf_fd = fd;
	d.alloc_size = mr.size;
	*out = d;
	// The exporting image/memory are intentionally kept (like vrlink keeps its own); store as a hidden target.
	Target t;
	t.used = true;
	t.image = img;
	t.mem = mem;
	m.targets.push_back(t);
	return int(m.targets.size() - 1);
}

int Decoder::import_target(const TargetDesc &d, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> tl(m.tmu);
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod;
	VkSubresourceLayout layouts[2];
	VkExternalMemoryImageCreateInfo ext;
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                          (d.readable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT : 0);
	VkImageCreateInfo ii = m.client_image_info(&mod, layouts, &ext, d, usage);
	Target t;
	t.readable = d.readable;
	VkResult r = vkCreateImage(m.device, &ii, nullptr, &t.image);
	if (r)
		return fail(err, "vkCreateImage(import P010)", r), -1;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m.device, t.image, &mr);
	int fd = dup(d.dmabuf_fd);
	if (fd < 0)
		return vkDestroyImage(m.device, t.image, nullptr), fail(err, "dup(dma-buf fd)"), -1;
	VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	vkGetMemoryFdPropertiesKHR(m.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
	uint32_t bits = mr.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = fd;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = &imp;
	ded.image = t.image;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &ded;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(bits, 0);
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &t.mem)))
	{
		close(fd); // not consumed on failure
		vkDestroyImage(m.device, t.image, nullptr);
		return fail(err, "vkAllocateMemory(import dma-buf)", r), -1;
	}
	if ((r = vkBindImageMemory(m.device, t.image, t.mem, 0)))
	{
		vkFreeMemory(m.device, t.mem, nullptr);
		vkDestroyImage(m.device, t.image, nullptr);
		return fail(err, "vkBindImageMemory(import)", r), -1;
	}
	t.used = true;
	if (m.rgba)
	{
		std::string rerr;
		if (!m.create_rgba(t, &rerr))
			m.alias_note = "full colour: no shared image for this surface (" + rerr + "), NV12 only";
		else
			m.alias_note = "full colour: shared 10-bit RGBA image ready";
	}
	if (m.alias)
	{
		std::string aerr;
		char feats[128];
		snprintf(feats, sizeof(feats), " [modifier tiling features: R8 0x%x, R8G8 0x%x]",
		         unsigned(m.mod_features(VK_FORMAT_R8_UNORM, d.modifier)), unsigned(m.mod_features(VK_FORMAT_R8G8_UNORM, d.modifier)));
		if (m.create_alias(t, d, &aerr))
			m.alias_note = std::string(t.ydirect == 1 ? "luma + chroma written directly (R8 / R8G8 alias)"
			                           : t.ydirect == 2 ? "chroma written directly (R8G8 alias), luma through NV12 plane 0 storage"
			                                            : "chroma written directly (R8G8 alias), luma copied") +
			               aerr + feats;
		else
			m.alias_note = "alias unavailable, using plane copies: " + aerr + feats;
	}
	for (size_t i = 0; i < m.targets.size(); i++)
		if (!m.targets[i].used)
		{
			m.targets[i] = t;
			return int(i);
		}
	m.targets.push_back(t);
	return int(m.targets.size() - 1);
}

bool Decoder::Impl::create_alias(Target &t, const TargetDesc &d, std::string *err)
{
	Impl &m = *this;
	const uint32_t W = uint32_t(m.width), H = uint32_t(m.height);
	const bool want_ay = m.mode == kModeDitherAlias || m.mode == kModeDirect8Alias;
	if ((want_ay && !m.alias_image(VK_FORMAT_R8_UNORM, W, H, d.modifier, d.plane_pitch[0], &t.ay, err)) ||
	    !m.alias_image(VK_FORMAT_R8G8_UNORM, W / 2, H / 2, d.modifier, d.plane_pitch[1], &t.ac, err))
		return destroy_alias(t), false;
	std::string ys_note;
	if (m.mode == kModeDirect8StorageLuma)
	{
		// The client format itself (NV12, UBWC layout of the driver's own NV12 planes) with STORAGE usage;
		// Pyrowave writes luma through an R8 view of PLANE_0.
		VkImageDrmFormatModifierExplicitCreateInfoEXT mod;
		VkSubresourceLayout layouts[2];
		VkExternalMemoryImageCreateInfo ext;
		VkImageCreateInfo ii = m.client_image_info(&mod, layouts, &ext, d, VK_IMAGE_USAGE_STORAGE_BIT);
		const VkFormat fl[3] = {m.surface_format(), VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
		VkImageFormatListCreateInfo fli = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
		fli.viewFormatCount = 3;
		fli.pViewFormats = fl;
		fli.pNext = ii.pNext;
		ii.pNext = &fli;
		ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		VkResult rr = vkCreateImage(m.device, &ii, nullptr, &t.ys);
		if (rr)
		{
			t.ys = VK_NULL_HANDLE;
			ys_note = std::string("; NV12 storage image refused (") + vkr(rr) + "), luma copied";
		}
	}
	VkMemoryRequirements ry = {}, rc, rs = {};
	if (t.ay) vkGetImageMemoryRequirements(m.device, t.ay, &ry);
	vkGetImageMemoryRequirements(m.device, t.ac, &rc);
	if (t.ys) vkGetImageMemoryRequirements(m.device, t.ys, &rs);
	int fd = dup(d.dmabuf_fd);
	if (fd < 0)
		return destroy_alias(t), fail(err, "dup(dma-buf fd) for alias");
	off_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	uint64_t size = sz > 0 ? uint64_t(sz) : d.alloc_size;
	char buf[224];
	if (d.plane_offset[0] + ry.size > size || d.plane_offset[1] + rc.size > size || (d.plane_offset[1] % rc.alignment))
	{
		close(fd);
		snprintf(buf, sizeof(buf), "alias layout does not fit (Y %llu B, C %llu B @%llu align %llu, dma-buf %llu B)",
		         (unsigned long long)ry.size, (unsigned long long)rc.size, (unsigned long long)d.plane_offset[1],
		         (unsigned long long)rc.alignment, (unsigned long long)size);
		*err = buf;
		return destroy_alias(t), false;
	}
	if (t.ys && rs.size > size)
	{
		vkDestroyImage(m.device, t.ys, nullptr);
		t.ys = VK_NULL_HANDLE;
		ys_note = "; NV12 storage image larger than the dma-buf, luma copied";
	}
	VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	vkGetMemoryFdPropertiesKHR(m.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
	uint32_t bits = rc.memoryTypeBits & (t.ay ? ry.memoryTypeBits : ~0u) & (t.ys ? rs.memoryTypeBits : ~0u) &
	                (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = fd;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &imp; // not dedicated: several images are bound into it
	ai.allocationSize = size;
	ai.memoryTypeIndex = m.find_mem(bits, 0);
	VkResult r;
	if (ai.memoryTypeIndex == UINT32_MAX)
		return close(fd), destroy_alias(t), fail(err, "alias memory type");
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &t.amem)))
		return close(fd), destroy_alias(t), fail(err, "vkAllocateMemory(alias import)", r);
	if ((t.ay && (r = vkBindImageMemory(m.device, t.ay, t.amem, d.plane_offset[0]))) ||
	    (r = vkBindImageMemory(m.device, t.ac, t.amem, d.plane_offset[1])))
		return destroy_alias(t), fail(err, "vkBindImageMemory(alias)", r);
	if (t.ys && (r = vkBindImageMemory(m.device, t.ys, t.amem, 0)))
	{
		vkDestroyImage(m.device, t.ys, nullptr);
		t.ys = VK_NULL_HANDLE;
		ys_note = std::string("; NV12 storage image bind failed (") + vkr(r) + "), luma copied";
	}
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (t.ay)
	{
		vi.image = t.ay;
		vi.format = VK_FORMAT_R8_UNORM;
		if ((r = vkCreateImageView(m.device, &vi, nullptr, &t.ayv)))
			return destroy_alias(t), fail(err, "vkCreateImageView(alias Y)", r);
	}
	vi.image = t.ac;
	vi.format = VK_FORMAT_R8G8_UNORM;
	if ((r = vkCreateImageView(m.device, &vi, nullptr, &t.acv)))
		return destroy_alias(t), fail(err, "vkCreateImageView(alias C)", r);
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = m.dpool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &m.dsl;
	if ((r = vkAllocateDescriptorSets(m.device, &dai, &t.dset)))
		return destroy_alias(t), fail(err, "vkAllocateDescriptorSets(alias)", r);
	// mode 2: finish8 (Y, Cb, Cr -> alias Y, alias CbCr); otherwise chroma_pack8 (Cb, Cr -> alias CbCr).
	const bool fin = m.mode == kModeDitherAlias;
	VkImageView v[5] = {m.y.view, m.cb.view, m.cr.view, t.ayv, t.acv};
	VkImageView v3[3] = {m.cb.view, m.cr.view, t.acv};
	const uint32_t n = fin ? 5 : 3, nsampled = fin ? 3 : 2;
	VkDescriptorImageInfo dii[5];
	VkWriteDescriptorSet wr[5] = {};
	for (uint32_t i = 0; i < n; i++)
	{
		dii[i] = {VK_NULL_HANDLE, fin ? v[i] : v3[i], VK_IMAGE_LAYOUT_GENERAL};
		wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr[i].dstSet = t.dset;
		wr[i].dstBinding = i;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = i < nsampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		wr[i].pImageInfo = &dii[i];
	}
	vkUpdateDescriptorSets(m.device, n, wr, 0, nullptr);
	t.alias_ok = true;
	t.ydirect = t.ay ? 1 : t.ys ? 2 : 0;
	if (err)
		*err = ys_note;
	return true;
}

void Decoder::Impl::destroy_alias(Target &t)
{
	if (t.dset) vkFreeDescriptorSets(device, dpool, 1, &t.dset);
	if (t.ayv) vkDestroyImageView(device, t.ayv, nullptr);
	if (t.acv) vkDestroyImageView(device, t.acv, nullptr);
	if (t.ay) vkDestroyImage(device, t.ay, nullptr);
	if (t.ac) vkDestroyImage(device, t.ac, nullptr);
	if (t.ys) vkDestroyImage(device, t.ys, nullptr);
	t.ys = VK_NULL_HANDLE;
	t.ydirect = 0;
	if (t.amem) vkFreeMemory(device, t.amem, nullptr);
	t.dset = VK_NULL_HANDLE;
	t.ayv = t.acv = VK_NULL_HANDLE;
	t.ay = t.ac = VK_NULL_HANDLE;
	t.amem = VK_NULL_HANDLE;
	t.alias_ok = false;
}

// v7.19 full colour: pack pipeline (Y, Yf, Cb, Cr -> shared RGBA image), one descriptor pool for all targets.
bool Decoder::Impl::create_pack(std::string *err)
{
	VkResult r;
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if ((r = vkCreateSampler(device, &si, nullptr, &pk_smp)))
		return fail(err, "vkCreateSampler(pack)", r);
	VkDescriptorSetLayoutBinding b[5] = {
	    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dl.bindingCount = 5;
	dl.pBindings = b;
	if ((r = vkCreateDescriptorSetLayout(device, &dl, nullptr, &pk_dsl)))
		return fail(err, "vkCreateDescriptorSetLayout(pack)", r);
	VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &pk_dsl;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pr;
	if ((r = vkCreatePipelineLayout(device, &pli, nullptr, &pk_pl)))
		return fail(err, "vkCreatePipelineLayout(pack)", r);
	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = sizeof(slp_pack_rgba10_spv);
	smi.pCode = slp_pack_rgba10_spv;
	if ((r = vkCreateShaderModule(device, &smi, nullptr, &pk_sh)))
		return fail(err, "vkCreateShaderModule(pack)", r);
	VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cp.stage.module = pk_sh;
	cp.stage.pName = "main";
	cp.layout = pk_pl;
	if ((r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pk_pipe)))
		return fail(err, "vkCreateComputePipelines(pack)", r);
	// v7.21: the pack with the luma filters folded in: + the wide-blur pyramid (sampled) and history (storage)
	{
		VkDescriptorSetLayoutBinding bx[7] = {
		    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
		VkDescriptorSetLayoutCreateInfo dx = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dx.bindingCount = 7;
		dx.pBindings = bx;
		VkPushConstantRange px = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
		VkPipelineLayoutCreateInfo plx = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		plx.setLayoutCount = 1;
		plx.pushConstantRangeCount = 1;
		plx.pPushConstantRanges = &px;
		VkShaderModuleCreateInfo smx = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		smx.codeSize = sizeof(slp_pack_rgba10_fx_spv);
		smx.pCode = slp_pack_rgba10_fx_spv;
		if (props.limits.maxPushConstantsSize >= 128 &&
		    vkCreateDescriptorSetLayout(device, &dx, nullptr, &pkx_dsl) == VK_SUCCESS &&
		    (plx.pSetLayouts = &pkx_dsl, vkCreatePipelineLayout(device, &plx, nullptr, &pkx_pl) == VK_SUCCESS) &&
		    vkCreateShaderModule(device, &smx, nullptr, &pkx_sh) == VK_SUCCESS)
		{
			VkComputePipelineCreateInfo cx = cp;
			cx.stage.module = pkx_sh;
			cx.layout = pkx_pl;
			pkx_ok = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cx, nullptr, &pkx_pipe) == VK_SUCCESS;
		}
	}
	const uint32_t sets = 4 * kMaxOut;   // rset[2] + xset[2] per target
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * sets},
	                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * sets},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * sets}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	dpi.maxSets = sets;
	dpi.poolSizeCount = 3;
	dpi.pPoolSizes = ps;
	if ((r = vkCreateDescriptorPool(device, &dpi, nullptr, &pk_pool)))
		return fail(err, "vkCreateDescriptorPool(pack)", r);
	pk_ok = true;
	return true;
}

// v7.19 full colour: the target's shared image. LINEAR A2B10G10R10, its own (dedicated) dma-buf; cleared to black and
// handed to the foreign queue family so vrcompositor never samples garbage before the first pack.
bool Decoder::Impl::create_rgba(Target &t, std::string *err)
{
	if (!pk_ok)
		return fail(err, "pack pipeline missing");
	VkResult r;
	const VkFormat fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	const uint64_t linear = 0; // DRM_FORMAT_MOD_LINEAR
	// v7.21: optionally the GPU's compressed tiling (UBWC): less memory traffic for our pack and vrcompositor's
	// sampling than LINEAR at 4 bytes per pixel. Used only if the driver offers it for this format as one memory
	// plane with storage, sampling and copies; anything else (or a failed create) falls back to LINEAR.
	const uint64_t ubwc = 0x0500000000000001ull;   // DRM_FORMAT_MOD_QCOM_COMPRESSED
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	const bool try_ubwc = has_modifiers && rgba_compressed && mod_planes(fmt, ubwc) == 1 &&
	                      (mod_features(fmt, ubwc) & need) == need;
	VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
	ml.drmFormatModifierCount = 1;
	ml.pDrmFormatModifiers = try_ubwc ? &ubwc : &linear;
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.pNext = &ml;
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.pNext = has_modifiers ? &ext : nullptr;   // hosts without modifiers (lavapipe): plain LINEAR, not exported
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = fmt;
	ii.extent = {uint32_t(width), uint32_t(height), 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = has_modifiers ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
	ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	r = vkCreateImage(device, &ii, nullptr, &t.rimg);
	if (r && try_ubwc)
	{
		ml.pDrmFormatModifiers = &linear;
		r = vkCreateImage(device, &ii, nullptr, &t.rimg);
	}
	if (r)
		return fail(err, "vkCreateImage(full-colour RGBA)", r);
	t.rmod = 0;
	if (has_modifiers)
	{
		VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
		if (vkGetImageDrmFormatModifierPropertiesEXT(device, t.rimg, &mp) == VK_SUCCESS)
			t.rmod = mp.drmFormatModifier;
	}
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(device, t.rimg, &mr);
	VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = has_modifiers ? &ex : nullptr;
	ded.image = t.rimg;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &ded;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, 0);
	if ((r = vkAllocateMemory(device, &ai, nullptr, &t.rmem)))
		return destroy_rgba(t), fail(err, "vkAllocateMemory(full-colour RGBA)", r);
	if ((r = vkBindImageMemory(device, t.rimg, t.rmem, 0)))
		return destroy_rgba(t), fail(err, "vkBindImageMemory(full-colour RGBA)", r);
	t.rexport = has_modifiers;
	t.rsize = mr.size;
	VkImageSubresource sub = {has_modifiers ? VkImageAspectFlags(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
	                                        : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0};
	VkSubresourceLayout lay = {};
	vkGetImageSubresourceLayout(device, t.rimg, &sub, &lay);
	t.roffset = lay.offset;
	t.rpitch = lay.rowPitch;
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = t.rimg;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if ((r = vkCreateImageView(device, &vi, nullptr, &t.rview)))
		return destroy_rgba(t), fail(err, "vkCreateImageView(full-colour RGBA)", r);
	VkDescriptorSetLayout two[2] = {pk_dsl, pk_dsl};
	VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	da.descriptorPool = pk_pool;
	da.descriptorSetCount = 2;
	da.pSetLayouts = two;
	if ((r = vkAllocateDescriptorSets(device, &da, t.rset)))
	{
		t.rset[0] = t.rset[1] = VK_NULL_HANDLE;
		return destroy_rgba(t), fail(err, "vkAllocateDescriptorSets(pack)", r);
	}
	const VkImageView yfv = yf.view ? yf.view : y.view;
	for (int c = 0; c < 2; c++)
	{
		VkDescriptorImageInfo di[5] = {{VK_NULL_HANDLE, y.view, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, yfv, VK_IMAGE_LAYOUT_GENERAL},
		                               {pk_smp, c ? cb4.view : cb.view, VK_IMAGE_LAYOUT_GENERAL},
		                               {pk_smp, c ? cr4.view : cr.view, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, t.rview, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[5] = {};
		for (int i = 0; i < 5; i++)
		{
			w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w[i].dstSet = t.rset[c];
			w[i].dstBinding = uint32_t(i);
			w[i].descriptorCount = 1;
			w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
			                    : i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			w[i].pImageInfo = &di[i];
		}
		vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
	}
	// v7.21: sets of the pack with the luma filters folded in (needs the wide-blur pyramid and history)
	if (pkx_ok && wb_all && wb_hist_view)
	{
		VkDescriptorSetLayout twox[2] = {pkx_dsl, pkx_dsl};
		da.pSetLayouts = twox;
		if (vkAllocateDescriptorSets(device, &da, t.xset) != VK_SUCCESS)
			t.xset[0] = t.xset[1] = VK_NULL_HANDLE;
		else
			for (int c = 0; c < 2; c++)
			{
				VkDescriptorImageInfo di[7] = {{VK_NULL_HANDLE, y.view, VK_IMAGE_LAYOUT_GENERAL},
				                               {VK_NULL_HANDLE, yfv, VK_IMAGE_LAYOUT_GENERAL},
				                               {pk_smp, c ? cb4.view : cb.view, VK_IMAGE_LAYOUT_GENERAL},
				                               {pk_smp, c ? cr4.view : cr.view, VK_IMAGE_LAYOUT_GENERAL},
				                               {VK_NULL_HANDLE, t.rview, VK_IMAGE_LAYOUT_GENERAL},
				                               {wb_smp, wb_all, VK_IMAGE_LAYOUT_GENERAL},
				                               {VK_NULL_HANDLE, wb_hist_view, VK_IMAGE_LAYOUT_GENERAL}};
				VkWriteDescriptorSet w[7] = {};
				for (int i = 0; i < 7; i++)
				{
					w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					w[i].dstSet = t.xset[c];
					w[i].dstBinding = uint32_t(i);
					w[i].descriptorCount = 1;
					w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
					                    : (i == 4 || i == 6) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
					                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					w[i].pImageInfo = &di[i];
				}
				vkUpdateDescriptorSets(device, 7, w, 0, nullptr);
			}
	}
	// black (Cr 0.5, Y 0, Cb 0.5), then out to the foreign family
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(cmd, 0);
	vkBeginCommandBuffer(cmd, &bi);
	barrier(cmd, t.rimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
	        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VkClearColorValue black = {};
	black.float32[0] = black.float32[2] = 512.0f / 1023.0f;
	black.float32[3] = 1.0f;
	VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
	const uint32_t foreign = has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
	barrier(cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
	        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT,
	        has_modifiers ? family : VK_QUEUE_FAMILY_IGNORED, has_modifiers ? foreign : VK_QUEUE_FAMILY_IGNORED);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	s.commandBufferCount = 1;
	s.pCommandBuffers = &cmd;
	if ((r = vkQueueSubmit(queue, 1, &s, fence)))
		return destroy_rgba(t), fail(err, "vkQueueSubmit(RGBA clear)", r);
	vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &fence);
	return true;
}

void Decoder::Impl::destroy_rgba(Target &t)
{
	if (t.rset[0] || t.rset[1])
	{
		VkDescriptorSet s2[2] = {t.rset[0], t.rset[1]};
		vkFreeDescriptorSets(device, pk_pool, s2[1] ? 2 : 1, s2);
	}
	if (t.xset[0])
		vkFreeDescriptorSets(device, pk_pool, 2, t.xset);
	t.xset[0] = t.xset[1] = VK_NULL_HANDLE;
	t.rmod = 0;
	if (t.rview) vkDestroyImageView(device, t.rview, nullptr);
	if (t.rimg) vkDestroyImage(device, t.rimg, nullptr);
	if (t.rmem) vkFreeMemory(device, t.rmem, nullptr);
	t.rset[0] = t.rset[1] = VK_NULL_HANDLE;
	t.rview = VK_NULL_HANDLE;
	t.rimg = VK_NULL_HANDLE;
	t.rmem = VK_NULL_HANDLE;
	t.rexport = false;
}

void Decoder::set_default_rgba_compressed(bool on)
{
	g_default_rgba_compressed = on;
}

void Decoder::set_merge(bool on)
{
	impl->merge_on = on;
}

void Decoder::set_fast_pyramid(bool on)
{
	impl->wb_fast = on;
}

uint64_t Decoder::rgba_modifier(int index) const
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> tl(m.tmu);
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].used)
		return 0;
	return m.targets[index].rmod;
}

int Decoder::pass_report(double ms[6], double *total_ms)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.ps_mu);
	const int n = m.ps_n;
	for (int i = 0; i < Impl::kPasses; i++)
	{
		ms[i] = n ? m.ps_sum[i] / n : 0.0;
		m.ps_sum[i] = 0;
	}
	if (total_ms)
		*total_ms = n ? m.ps_total / n : 0.0;
	m.ps_total = 0;
	m.ps_n = 0;
	return n;
}

bool Decoder::full_colour() const
{
	return impl->rgba && impl->pk_ok;
}

bool Decoder::rgba_export(int index, RgbaExport *out, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> tl(m.tmu);
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].used || !m.targets[index].rimg)
		return fail(err, "rgba_export: target has no full-colour image");
	Target &t = m.targets[index];
	if (!t.rexport)
		return fail(err, "rgba_export: not exportable on this device");
	VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = t.rmem;
	gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	int fd = -1;
	VkResult r = vkGetMemoryFdKHR(m.device, &gi, &fd);
	if (r || fd < 0)
		return fail(err, "vkGetMemoryFdKHR(RGBA)", r);
	out->fd = fd;
	out->width = uint32_t(m.width);
	out->height = uint32_t(m.height);
	out->vk_format = uint32_t(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
	out->modifier = t.rmod;
	out->offset = t.roffset;
	out->row_pitch = t.rpitch;
	out->size = t.rsize;
	return true;
}

void Decoder::set_target_output(int index, int out)
{
	if (index >= 0 && index < Impl::kMaxOut)
		impl->tout[index] = (out == kOutRGBA || out == kOutBoth) ? out : kOutNV12;
}

int Decoder::target_output(int index) const
{
	return (index >= 0 && index < Impl::kMaxOut) ? impl->tout[index].load() : kOutNV12;
}

void Decoder::release_target(int index)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> tl(m.tmu);
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].used)
		return;
	vkQueueWaitIdle(m.queue);
	Target &t = m.targets[index];
	m.destroy_alias(t);
	m.destroy_rgba(t);
	if (index < Impl::kMaxOut)
		m.tout[index] = kOutNV12;
	vkDestroyImage(m.device, t.image, nullptr);
	vkFreeMemory(m.device, t.mem, nullptr);
	t = Target();
}

static bool push_frame(pyrowave_decoder dec, const uint8_t *frame, size_t size, std::string *err, bool *complete)
{
	FrameHeader h;
	const uint32_t *sizes;
	const uint8_t *payload;
	if (!parse_frame(frame, size, &h, &sizes, &payload))
		return fail(err, "parse SLPW frame");
	// Each SLPW frame is decoded on its own: clear so a restarted encoder (sequence reset) or a
	// skipped frame never makes Pyrowave drop packets as "older".
	pyrowave_decoder_clear(dec);
	size_t off = 0;
	for (uint32_t i = 0; i < h.num_packets; i++)
	{
		if (pyrowave_decoder_push_packet(dec, payload + off, sizes[i]) != PYROWAVE_SUCCESS)
			return fail(err, "pyrowave_decoder_push_packet");
		off += sizes[i];
	}
	*complete = pyrowave_decoder_decode_is_ready(dec, false);
	return true;
}

bool Decoder::decode_into(const uint8_t *frame, size_t size, int index, DecodeTiming *timing, std::string *err)
{
	int ticket = submit(frame, size, index, err);
	return ticket >= 0 && wait(ticket, timing, err);
}

int Decoder::submit(const uint8_t *frame, size_t size, int index, std::string *err)
{
	Impl &m = *impl;
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].used)
		return fail(err, "decode_into: bad target"), -1;
	auto t0 = std::chrono::steady_clock::now();
	FrameHeader fh;
	const uint32_t *fsz;
	const uint8_t *fpl;
	if (!parse_frame(frame, size, &fh, &fsz, &fpl))
		return fail(err, "parse SLPW frame"), -1;
	// Take the next slot; it is free once the frame submitted two frames ago has been waited for. That also keeps
	// Granite's two frame contexts (staging memory) from being recycled while the GPU still reads them.
	uint64_t tk;
	Impl::Slot *sp;
	{
		std::unique_lock<std::mutex> lk(m.smu);
		tk = m.next_ticket++;
		sp = &m.slots[tk % Impl::kSlots];
		m.scv.wait(lk, [&] { return !sp->busy; });
		sp->busy = true;
	}
	Impl::Slot &s = *sp;
	auto release_slot = [&]() {
		std::lock_guard<std::mutex> lk(m.smu);
		s.busy = false;
		m.scv.notify_all();
	};
	bool complete = false;
	const bool repeat = fh.num_packets == 0; // re-present the planes from the last decode
	// v7.19: 4:4:4 frames need the full-colour decoder (mode 6); repeats re-present whatever the planes hold
	const int chroma = repeat ? m.last_chroma : (fh.chroma == 1 ? 1 : 0);
	if (chroma == 1 && !m.pdec4)
		return release_slot(), fail(err, "4:4:4 frame, but full colour is off on the Frame: turn Full colour off in Pyrowave Link, or enable it on the "
		                 "Frame (./install.sh --full-colour) and restart SteamVR there"), -1;
	pyrowave_decoder dec = chroma ? m.pdec4 : m.pdec;
	if (!repeat && !push_frame(dec, frame, size, err, &complete))
		return release_slot(), -1;
	if (repeat)
		complete = true;
	Target &t = m.targets[index];
	int tout = (m.rgba && t.rimg && index < Impl::kMaxOut) ? m.tout[index].load() : int(kOutNV12);
	const bool want_rgba = tout != kOutNV12, want_nv12 = tout != kOutRGBA;
	Plane *cbp = chroma ? &m.cb4 : &m.cb, *crp = chroma ? &m.cr4 : &m.cr;
	s.t0 = t0;
	s.complete = complete;
	s.direct = t.alias_ok;
	s.luma_direct = t.alias_ok && t.ydirect != 0;
	s.output = tout;
	s.c444 = chroma == 1;

	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(s.cmd, 0);
	vkBeginCommandBuffer(s.cmd, &bi);
	if (m.timestamps)
	{
		vkCmdResetQueryPool(s.cmd, s.qpool, 0, 8);
		vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s.qpool, 0);
	}
	// The previous submission (possibly still running) must finish before this one touches shared state
	// (Pyrowave's payload buffer, our planes).
	{
		VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(s.cmd, &di);
	}
	// Previous frame's readers (pack / copies) must finish before the planes are overwritten.
	for (Plane *p : {&m.y, cbp, crp})
		Impl::barrier(s.cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, 0,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	const uint32_t foreign = m.has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
	const bool c_direct = t.alias_ok;                 // chroma written into the surface through the R8G8 alias
	const int ydir = t.alias_ok ? t.ydirect : 0;      // 1: R8 alias, 2: NV12 plane 0 storage
	// Who writes luma straight into the surface: Pyrowave (direct 8-bit) or the dither pass (mode 2, R8 alias).
	const bool y_by_pyro = m.direct8 && ydir != 0;
	const bool y_direct = ydir != 0;
	// Acquire the directly written parts from UNDEFINED (contents fully replaced).
	if (c_direct)
		Impl::barrier(s.cmd, t.ac, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	if (ydir == 1)
		Impl::barrier(s.cmd, t.ay, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	if (ydir == 2)
		Impl::barrier(s.cmd, t.ys, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_IMAGE_ASPECT_PLANE_0_BIT);

	pyrowave_gpu_buffers gb = {};
	Plane *pp[3] = {&m.y, cbp, crp};
	for (int i = 0; i < 3; i++)
	{
		auto &v = gb.planes[i];
		v.image = pp[i]->image;
		v.width = pp[i]->w;
		v.height = pp[i]->h;
		v.image_format = pp[i]->format;
		v.view_format = pp[i]->format;
		v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		v.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
		v.layout = VK_IMAGE_LAYOUT_GENERAL;
	}
	if (y_by_pyro && ydir == 1)
		gb.planes[0].image = t.ay;
	else if (y_by_pyro && ydir == 2)
	{
		gb.planes[0].image = t.ys;
		gb.planes[0].image_format = m.surface_format();
		gb.planes[0].view_format = VK_FORMAT_R8_UNORM;
		gb.planes[0].aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
	}
	// Direct 8-bit: Pyrowave's own final pass rounds to 8 bits; with the fork it adds the ordered dither.
	if (m.direct8)
		pyrowave_decoder_set_ycbcr_scaling_factor(dec, (m.dither > 0.0f && m.pyro_dither) ? -1.0f / 255.0f : 1.0f);
	pyrowave_result pr = PYROWAVE_SUCCESS;
	// A repeat normally re-presents m.y/cb/cr. With luma written straight into the surface there is no kept
	// copy, so run the decode again: Pyrowave keeps the last frame's packets until the next clear().
	if (!repeat || y_by_pyro)
	{
		pyrowave_device_set_command_buffer(m.pdev, s.cmd);
		pr = pyrowave_decoder_decode_gpu_buffer(dec, nullptr, nullptr, &gb);
		pyrowave_device_set_command_buffer(m.pdev, VK_NULL_HANDLE);
	}
	if (pr != PYROWAVE_SUCCESS)
	{
		vkEndCommandBuffer(s.cmd);
		release_slot();
		return fail(err, "pyrowave_decoder_decode_gpu_buffer"), -1;
	}
	if (!repeat)
		m.last_chroma = chroma;
	if (m.timestamps)
		vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, s.qpool, 1);

	// Decode writes -> finishing pass reads (and the luma copy).
	for (Plane *p : {&m.y, cbp, crp})
		Impl::barrier(s.cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
		              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	const bool b8 = m.bits == 8 && !m.direct8; // full-resolution dither pass (modes 0 / 2)
	Plane &oy = m.out_y(), &oc = m.out_c();
	// The finishing pass (dither to 8 bits / chroma pack). v7.19 full colour: runs after the filters, below, and
	// only when the NV12 surface is written.
	auto finish = [&](VkDescriptorSet set, float cstep) {
		for (Plane *p : {&m.y8, &oc})
			if (p->image && p != &m.y)
				Impl::barrier(s.cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				              VK_PIPELINE_STAGE_2_COPY_BIT, 0,
				              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipe);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pl, 0, 1, &set, 0, nullptr);
		if (b8 || m.rgba)
		{
			const float fpc[2] = {m.dither, cstep};
			vkCmdPushConstants(s.cmd, m.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fpc), fpc);
			vkCmdDispatch(s.cmd, (m.y.w + 7) / 8, (m.y.h + 7) / 8, 1);
		}
		else
			vkCmdDispatch(s.cmd, (oc.w + 7) / 8, (oc.h + 7) / 8, 1);
		for (Plane *p : {&m.y8, &oc})
			if (p->image && p != &m.y)
				Impl::barrier(s.cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
				              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	};
	// v7.22: mode 0 filters the 8-bit planes the finishing pass writes, in place. A repeat skips those filters, so it
	// must not redo the finishing pass either (that would overwrite the filtered planes with an unfiltered picture).
	if (!m.rgba && !(repeat && b8 && !c_direct))
		finish(c_direct ? t.dset : m.dset, 1.0f);
	// v7.21 per-pass timestamps: 2 finish (4:2:0 modes), 3 seam deblock, 4 blur pyramid, 5 blur apply, 6 deblock /
	// sharpen, 7 end (pack and / or copies)
	auto stamp = [&](uint32_t q) {
		if (m.timestamps)
			vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, s.qpool, q);
	};
	stamp(2);
	// v7.5 luma post-filter (deblock / dering + inset sharpening) into yf; the luma copy then reads yf.
	const bool copy_y = !y_direct, copy_c = !c_direct;
	const float pf_db_w = m.pf_deblock, pf_db_i = m.pf_deblock_inset, pf_sharpen = m.pf_sharpen;
	// Bits per luma pixel of this frame drive the deblock strength: ~nothing above ~1 bpp, full below ~0.3.
	if (!repeat)
	{
		const float bpp = float(fh.payload_bytes) * 8.0f / float(std::max(1, m.width * m.height));
		m.pf_bpp_factor = std::max(0.15f, std::min(1.0f, (1.0f - bpp) / 0.7f));
	}
	// v7.8: separate strengths for the wide views (odd tiles) and the insets (even tiles). Below ~0.6 LSB a deblock
	// cannot change an 8-bit pixel and is skipped; tiles with nothing to do are copied straight from the decode.
	// v7.9: seam mode (default) deblocks in place on the decoded plane before anything reads it; the smoothing pass
	// below then only sharpens. Smooth mode (pf_mode 1) is the v7.8 behaviour.
	const bool seam_mode = m.pf_mode.load() == 0;
	const float bppf = m.pf_bpp_scale.load() ? m.pf_bpp_factor : 1.0f;
	const float al_w = pf_db_w * (10.0f / 255.0f) * bppf;
	const float al_i = pf_db_i * (10.0f / 255.0f) * bppf;
	const bool seam_w = seam_mode && m.sm_ok && copy_y && al_w >= 1.0f / 255.0f;
	const bool seam_i = seam_mode && m.sm_ok && copy_y && al_i >= 1.0f / 255.0f;
	if ((seam_w || seam_i) && !repeat)   // a repeat re-copies the already filtered plane
	{
		Impl::barrier(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		struct { float ai, aw, tile, dir, grid, ramp, amax, p0, p1, p2, p3, p4; float rpc[16]; } spc = {
		    seam_i ? al_i : 0.0f, seam_w ? al_w : 0.0f, float(m.height / 4), 0.0f, 16.0f, 4.0f,
		    (10.0f / 255.0f) * bppf, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {}};
		static_assert(sizeof(spc) == 112, "seam.comp push constants");
		m.ramp_pc(spc.rpc);   // v7.14: ramped wide-view deblock
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.sm_pipe);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.sm_pl, 0, 1, &m.sm_dset, 0, nullptr);
		// v7.10: coarse grids first (64 px with 16-px ramps, 32 with 8), then the 16-px grid
		const int coarse = m.pf_coarse.load();
		const int grids[3] = {64, 32, 16};
		for (int pass = 0; pass < 6; pass++)
		{
			const int g = grids[pass / 2], dir = pass & 1;
			if ((g == 64 && coarse < 2) || (g == 32 && coarse < 1))
				continue;
			spc.dir = float(dir);
			spc.grid = float(g);
			spc.ramp = float(g / 4);
			const uint32_t lines = dir ? (uint32_t(m.height) / g - 1) : (uint32_t(m.width) / g - 1);
			const uint32_t along = dir ? uint32_t(m.width) : uint32_t(m.height);
			vkCmdPushConstants(s.cmd, m.sm_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
			vkCmdDispatch(s.cmd, (lines * along + 63) / 64, 1, 1);
			Impl::barrier(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
			              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
			                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
		}
	}
	stamp(3);
	const float sig_w = seam_mode ? 0.0f : pf_db_w * (3.0f / 255.0f) * m.pf_bpp_factor;
	const float sig_i = seam_mode ? 0.0f : pf_db_i * (3.0f / 255.0f) * m.pf_bpp_factor;
	const bool run_w = m.pf_ok && copy_y && sig_w >= 0.6f / 255.0f;
	const bool run_i = m.pf_ok && copy_y && (sig_i >= 0.6f / 255.0f || pf_sharpen > 0.0f);
	const bool pf = run_w || run_i;
	const uint32_t tile = m.height / 4;
	// v7.12: in seam mode with sharpening only, use the light sharpen shader over the square that holds the
	// sharpening radius of each inset; the rest of each inset is copied straight from the decode.
	const bool light = pf && run_i && !run_w && m.sh_pipe != VK_NULL_HANDLE && m.sh_on.load() && sig_i < 0.6f / 255.0f && pf_sharpen > 0.0f;
	uint32_t sq0 = 0, sq1 = tile;   // square [sq0, sq1) in tile coordinates, both axes (multiples of 8)
	if (light)
	{
		const float half = std::min(1.0f, float(m.pf_r1)) * 0.5f * float(tile) + 2.0f;
		const uint32_t c = tile / 2;
		const uint32_t h8 = std::min(c, (uint32_t(std::ceil(half)) + 7u) & ~7u);
		sq0 = c - h8;
		sq1 = std::min(tile, c + h8);
	}
	auto tile_filtered = [&](uint32_t t4) { return (t4 & 1) ? run_w : run_i; };
	// v7.13: contrast-preserving blur of the wide views, in place, after the seam deblock (skipped on repeats: the
	// plane is already filtered)
	const float wb_px = m.wb_px.load();
	const float wb_t = m.wb_temporal.load();   // v7.17
	if (!(wb_t > 0.0f))
		m.wb_hist_ok = false;
	const bool wb_run = m.wb_ok && copy_y && (wb_px >= 0.02f || wb_t > 0.0f);
	// v7.21: RGBA-only frames (the normal full-colour case) fold the blur's last step and the light inset sharpening
	// into the pack: the filtered luma is never written to the 16-bit plane and read back. Not with the smoothing
	// wide deblock (run_w), which reads the blurred plane.
	const bool merge = m.rgba && want_rgba && !want_nv12 && m.merge_on.load() && t.xset[chroma] != VK_NULL_HANDLE && !run_w;
	const bool merge_blur = merge && wb_run && (!repeat || m.wb_pyr_ok);
	const bool merge_light = merge && pf && light;
	if (!wb_run && !repeat)
		m.wb_pyr_ok = false;
	struct { float lod, contrast, dither, tile, max_lod, temporal, tclamp, hist_ok; float rpc[16]; } apc = {};
	static_assert(sizeof(apc) == 96, "wideblur_apply.comp push constants");
	if (wb_run)
	{
		// lod: 0.5 px -> 0, 1 px -> 1, 2 px -> 2, 4 px -> 3 (the chain also needs lod + 1.5 for the contrast term)
		// v7.14: this is the blur at the view centre; the shader scales it by the local pixel density and the ramp
		const uint32_t T = uint32_t(m.height / 4);
		apc = {std::min(wb_px, 64.0f), m.wb_contrast.load(), m.dither ? 1.0f : 0.0f, float(T), float(m.wb_levels) - 1.5f,
		       std::max(0.0f, wb_t), m.wb_tclamp.load(), m.wb_hist_ok ? 1.0f : 0.0f, {}};
		m.ramp_pc(apc.rpc);
	}
	if (wb_run && !repeat)
	{
		const uint32_t T = uint32_t(m.height / 4);
		// v7.17: history from earlier submissions on this queue (kept across frames: UNDEFINED only the first time)
		Impl::barrier(s.cmd, m.wb_hist, m.wb_hist_inited ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		              VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		m.wb_hist_inited = true;
		Impl::barrier(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
		Impl::barrier(s.cmd, m.wb_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.wb_pipe_d);
		for (uint32_t l = 0; l < m.wb_levels; l++)
		{
			const uint32_t sz = (T / 2) >> l;
			struct { int32_t level, tile, fast; } dpc = {int32_t(l), int32_t(T), m.wb_fast.load() ? 1 : 0};
			vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.wb_pl_d, 0, 1, &m.wb_set_d[l], 0, nullptr);
			vkCmdPushConstants(s.cmd, m.wb_pl_d, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dpc), &dpc);
			vkCmdDispatch(s.cmd, (sz + 7) / 8, (sz + 7) / 8, 2);
			Impl::barrier(s.cmd, m.wb_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		}
		m.wb_pyr_ok = true;
		m.wb_hist_ok = wb_t > 0.0f;   // the apply (here or in the pack) writes the history
	}
	stamp(4);
	if (wb_run && !repeat && !merge_blur)
	{
		const uint32_t T = uint32_t(m.height / 4);
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.wb_pipe_a);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.wb_pl_a, 0, 1, &m.wb_set_a, 0, nullptr);
		vkCmdPushConstants(s.cmd, m.wb_pl_a, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(apc), &apc);
		vkCmdDispatch(s.cmd, (T + 15) / 16, (T + 15) / 16, 2);
		Impl::barrier(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
		                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	stamp(5);
	if (pf && !merge_light)
	{
		Impl::barrier(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		Impl::barrier(s.cmd, m.yf.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		struct { float sig_i, sig_w, sharpen, r0, r1, dither, tile, y0, wide_step; } pc = {
			sig_i >= 0.6f / 255.0f ? sig_i : 0.0f, run_w ? sig_w : 0.0f, pf_sharpen, m.pf_r0, m.pf_r1,
			m.dither > 0.0f ? 1.0f : 0.0f, float(tile), 0.0f, m.pf_wide_step};
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, light ? m.sh_pipe : m.pf_pipe);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pf_pl, 0, 1, &m.pf_dset, 0, nullptr);
		if (light)
		{
			for (uint32_t t4 = 0; t4 < 4; t4 += 2)
			{
				pc.sig_i = float(sq0);                 // x0 in sharpen.comp
				pc.y0 = float(t4 * tile + sq0);
				vkCmdPushConstants(s.cmd, m.pf_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
				vkCmdDispatch(s.cmd, (sq1 - sq0 + 7) / 8, (sq1 - sq0 + 7) / 8, 1);
			}
		}
		else if (run_w && run_i)
		{
			vkCmdPushConstants(s.cmd, m.pf_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			vkCmdDispatch(s.cmd, (m.yf.w + 15) / 16, (m.yf.h + 15) / 16, 1);
		}
		else
		{
			for (uint32_t t4 = 0; t4 < 4; t4++)
			{
				if (!tile_filtered(t4))
					continue;
				pc.y0 = float(t4 * tile);
				vkCmdPushConstants(s.cmd, m.pf_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
				vkCmdDispatch(s.cmd, (m.yf.w + 15) / 16, (tile + 15) / 16, 1);
			}
		}
		Impl::barrier(s.cmd, m.yf.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	Plane &ly = (run_w && run_i) ? m.yf : oy;   // whole-plane luma copy source (otherwise per tile, below)
	stamp(6);

	// Copies for whatever was not written directly.
	if (!m.rgba && (copy_y || copy_c))
	{
		const VkImageAspectFlags asp = (copy_y ? VK_IMAGE_ASPECT_PLANE_0_BIT : 0) | (copy_c ? VK_IMAGE_ASPECT_PLANE_1_BIT : 0);
		if (m.preserve_layout)
			Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, asp,
			              foreign, m.family);
		else
			Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, asp);
		VkImageCopy cp = {};
		cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		if (copy_y)
		{
			cp.dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			if (light)
			{
				// v7.12: insets = sharpened square from yf + the four bands around it from the decode; wide views
				// from the decode. All regions disjoint, so two copies and no barrier between them.
				VkImageCopy rf[2], ro[10];
				uint32_t nf = 0, no = 0;
				auto rect = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
					VkImageCopy r = cp;
					r.srcOffset = {int32_t(x), int32_t(y), 0};
					r.dstOffset = r.srcOffset;
					r.extent = {w, h, 1};
					return r;
				};
				const uint32_t side = sq1 - sq0;
				for (uint32_t t4 = 0; t4 < 4; t4++)
				{
					const uint32_t y = t4 * tile;
					if (t4 & 1)
					{
						ro[no++] = rect(0, y, oy.w, tile);
						continue;
					}
					rf[nf++] = rect(sq0, y + sq0, side, side);
					if (sq0 > 0)
					{
						ro[no++] = rect(0, y, oy.w, sq0);                       // above the square
						ro[no++] = rect(0, y + sq0, sq0, side);                 // left of it
					}
					if (sq1 < tile)
					{
						ro[no++] = rect(0, y + sq1, oy.w, tile - sq1);          // below
						ro[no++] = rect(sq1, y + sq0, oy.w - sq1, side);        // right
					}
				}
				vkCmdCopyImage(s.cmd, m.yf.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nf, rf);
				if (no)
					vkCmdCopyImage(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, no, ro);
			}
			else if (pf && !(run_w && run_i))
			{
				// per tile: filtered tiles from yf, the others straight from the decode
				VkImageCopy rf[4], ro[4];
				uint32_t nf = 0, no = 0;
				for (uint32_t t4 = 0; t4 < 4; t4++)
				{
					VkImageCopy r = cp;
					r.srcOffset = {0, int32_t(t4 * tile), 0};
					r.dstOffset = r.srcOffset;
					r.extent = {oy.w, tile, 1};
					if (tile_filtered(t4))
						rf[nf++] = r;
					else
						ro[no++] = r;
				}
				if (nf)
					vkCmdCopyImage(s.cmd, m.yf.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nf, rf);
				if (no)
					vkCmdCopyImage(s.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, no, ro);
			}
			else
			{
				cp.extent = {ly.w, ly.h, 1};
				vkCmdCopyImage(s.cmd, ly.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
			}
		}
		if (copy_c)
		{
			cp.dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp.extent = {oc.w, oc.h, 1};
			vkCmdCopyImage(s.cmd, oc.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
		}
		// Hand the surface back to the outside world (vrcompositor samples it via its own import).
		Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, asp,
		              m.family, foreign);
	}
	if (m.rgba)
	{
		// v7.19 full colour. Every filter above wrote the 16-bit luma (in place) or yf; make those visible.
		VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(s.cmd, &di);
		if (want_rgba)
		{
			// Contents fully replaced: acquire from UNDEFINED; afterwards back to the foreign family (vrcompositor).
			Impl::barrier(s.cmd, t.rimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
			uint32_t mask = 0;
			if (pf && !light)
				for (uint32_t t4 = 0; t4 < 4; t4++)
					if ((run_w && run_i) || tile_filtered(t4))
						mask |= 1u << t4;
			if (merge)
			{
				// v7.21: pack_rgba10_fx.comp (see the push-constant layout there)
				const float wt = repeat ? ((m.wb_hist_ok && wb_t > 0.0f) ? apc.temporal : 0.0f) : apc.temporal;
				struct { float a[4], b[4], c[4], d[4], rpc[16]; } xpc = {
				    {m.dither > 0.0f ? 1.0f : 0.0f, float(tile), merge_light ? 1.0f : 0.0f, float(mask)},
				    {float(sq0), float(sq1), pf_sharpen, m.pf_r0.load()},
				    {m.pf_r1.load(), apc.lod, apc.contrast, apc.max_lod},
				    {wt, apc.tclamp, apc.hist_ok, float((merge_blur ? 1 : 0) | (repeat ? 2 : 0))},
				    {}};
				static_assert(sizeof(xpc) == 128, "pack_rgba10_fx.comp push constants");
				memcpy(xpc.rpc, apc.rpc, sizeof(xpc.rpc));
				vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pkx_pipe);
				vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pkx_pl, 0, 1, &t.xset[chroma], 0, nullptr);
				vkCmdPushConstants(s.cmd, m.pkx_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(xpc), &xpc);
			}
			else
			{
				struct { float dither, tile, light, sq0, sq1, tilemask; } kpc = {
				    m.dither > 0.0f ? 1.0f : 0.0f, float(tile), (pf && light) ? 1.0f : 0.0f, float(sq0), float(sq1), float(mask)};
				static_assert(sizeof(kpc) == 24, "pack_rgba10.comp push constants");
				vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pk_pipe);
				vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pk_pl, 0, 1, &t.rset[chroma], 0, nullptr);
				vkCmdPushConstants(s.cmd, m.pk_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(kpc), &kpc);
			}
			vkCmdDispatch(s.cmd, (uint32_t(m.width) + 7) / 8, (uint32_t(m.height) + 7) / 8, 1);
			Impl::barrier(s.cmd, t.rimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			              VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT,
			              t.rexport ? m.family : VK_QUEUE_FAMILY_IGNORED, t.rexport ? foreign : VK_QUEUE_FAMILY_IGNORED);
		}
		if (want_nv12)
		{
			// NV12 surface (until vrcompositor has switched it over): mode 0's finishing pass on the filtered 16-bit
			// planes (4:4:4 chroma box-averaged), then the two plane copies. The light sharpen (yf) is RGBA-only.
			finish(chroma ? m.dset444 : m.dset, chroma ? 2.0f : 1.0f);
			const VkImageAspectFlags asp = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
			if (m.preserve_layout)
				Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, asp,
				              foreign, m.family);
			else
				Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, asp);
			VkImageCopy cp = {};
			cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			cp.dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp.extent = {m.y8.w, m.y8.h, 1};
			vkCmdCopyImage(s.cmd, m.y8.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
			cp.dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp.extent = {m.c8.w, m.c8.h, 1};
			vkCmdCopyImage(s.cmd, m.c8.image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
			Impl::barrier(s.cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, asp,
			              m.family, foreign);
		}
	}
	if (c_direct)
		Impl::barrier(s.cmd, t.ac, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT, m.family,
		              foreign);
	if (ydir == 1)
		Impl::barrier(s.cmd, t.ay, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT, m.family,
		              foreign);
	if (ydir == 2)
		Impl::barrier(s.cmd, t.ys, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_PLANE_0_BIT,
		              m.family, foreign);
	if (m.timestamps)
		vkCmdWriteTimestamp2(s.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.qpool, 7);
	vkEndCommandBuffer(s.cmd);

	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &s.cmd;
	VkResult r = vkQueueSubmit(m.queue, 1, &si, s.fence);
	if (r)
		return release_slot(), fail(err, "vkQueueSubmit(decode)", r), -1;
	return int(tk & 0x3fffffff);
}

bool Decoder::wait(int ticket, DecodeTiming *timing, std::string *err)
{
	Impl &m = *impl;
	Impl::Slot &s = m.slots[uint64_t(ticket) % Impl::kSlots];
	VkResult r = vkWaitForFences(m.device, 1, &s.fence, VK_TRUE, 500ull * 1000 * 1000);
	auto t1 = std::chrono::steady_clock::now();
	if (r == VK_SUCCESS)
		vkResetFences(m.device, 1, &s.fence);
	else
	{
		// Never hand the slot back with an unsignalled fence that may still fire: wait for the queue.
		vkQueueWaitIdle(m.queue);
		vkResetFences(m.device, 1, &s.fence);
	}
	if (timing)
	{
		timing->complete = s.complete;
		timing->direct = s.direct;
		timing->luma_direct = s.luma_direct;
		timing->output = s.output;
		timing->chroma444 = s.c444;
		timing->cpu_ms = std::chrono::duration<double, std::milli>(t1 - s.t0).count();
		if (m.timestamps && r == VK_SUCCESS)
		{
			uint64_t ts[8] = {};
			if (vkGetQueryPoolResults(m.device, s.qpool, 0, 8, sizeof(ts), ts, sizeof(uint64_t),
			                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			{
				timing->gpu_decode_ms = m.ts_ms(ts[0], ts[1]);
				timing->gpu_pack_copy_ms = m.ts_ms(ts[1], ts[7]);
				timing->gpu_finish_ms = m.ts_ms(ts[1], ts[6]);
				timing->gpu_copy_ms = m.ts_ms(ts[6], ts[7]);
				timing->gpu_start_ms = std::chrono::duration<double, std::milli>(t1 - s.t0).count() -
				                       m.ts_ms(ts[0], ts[7]);
				// v7.21: finish, seam, pyramid, blur, deblock/sharpen, pack/copies
				for (int i = 0; i < Impl::kPasses; i++)
					timing->pass_ms[i] = m.ts_ms(ts[i + 1], ts[i + 2]);
				std::lock_guard<std::mutex> lk(m.ps_mu);
				for (int i = 0; i < Impl::kPasses; i++)
					m.ps_sum[i] += timing->pass_ms[i];
				m.ps_total += m.ts_ms(ts[0], ts[7]);
				m.ps_n++;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lk(m.smu);
		s.busy = false;
		m.scv.notify_all();
	}
	if (r)
		return fail(err, "vkWaitForFences(decode)", r);
	return true;
}

bool Decoder::readback_target(int index, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err)
{
	Impl &m = *impl;
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].readable)
		return fail(err, "readback: target not readable");
	Target &t = m.targets[index];
	const VkDeviceSize bs = m.bits == 8 ? 1 : 2;
	VkDeviceSize ysz = VkDeviceSize(m.y.w) * m.y.h * bs, csz = VkDeviceSize(m.cbcr.w) * m.cbcr.h * 2 * bs;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = ysz + csz;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf;
	VkResult r = vkCreateBuffer(m.device, &bci, nullptr, &buf);
	if (r)
		return fail(err, "vkCreateBuffer(readback)", r);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(m.device, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory mem;
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &mem)))
		return vkDestroyBuffer(m.device, buf, nullptr), fail(err, "vkAllocateMemory(readback)", r);
	vkBindBufferMemory(m.device, buf, mem, 0);

	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(m.cmd, 0);
	vkBeginCommandBuffer(m.cmd, &bi);
	Impl::barrier(m.cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
	              VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
	              m.has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL, m.family);
	VkBufferImageCopy c[2] = {};
	c[0].bufferOffset = 0;
	c[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	c[0].imageExtent = {m.y.w, m.y.h, 1};
	c[1].bufferOffset = ysz;
	c[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	c[1].imageExtent = {m.cbcr.w, m.cbcr.h, 1};
	vkCmdCopyImageToBuffer(m.cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 2, c);
	VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(m.cmd, &di);
	vkEndCommandBuffer(m.cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &m.cmd;
	r = vkQueueSubmit(m.queue, 1, &si, m.fence);
	if (!r)
		r = vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, 2000ull * 1000 * 1000);
	vkResetFences(m.device, 1, &m.fence);
	bool ok = r == VK_SUCCESS;
	if (ok)
	{
		void *p = nullptr;
		vkMapMemory(m.device, mem, 0, VK_WHOLE_SIZE, 0, &p);
		y.resize(size_t(ysz / bs));
		cbcr.resize(size_t(csz / bs));
		if (bs == 2)
		{
			memcpy(y.data(), p, size_t(ysz));
			memcpy(cbcr.data(), static_cast<uint8_t *>(p) + ysz, size_t(csz));
		}
		else
		{
			const uint8_t *b = static_cast<const uint8_t *>(p);
			for (size_t i = 0; i < y.size(); i++) y[i] = uint16_t(b[i] << 8);
			for (size_t i = 0; i < cbcr.size(); i++) cbcr[i] = uint16_t(b[ysz + i] << 8);
		}
		vkUnmapMemory(m.device, mem);
	}
	else
		fail(err, "readback submit/wait", r);
	vkDestroyBuffer(m.device, buf, nullptr);
	vkFreeMemory(m.device, mem, nullptr);
	return ok;
}

static bool sampled_read(Decoder::Impl &m, VkImage image, VkFormat fmt, bool external, std::vector<uint16_t> &y,
                         std::vector<uint16_t> &cbcr, std::string *err)
{
	VkDevice dev = m.device;
	VkResult r;
	VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
	VkSampler smp = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkShaderModule sm = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorPool dp = VK_NULL_HANDLE;
	Plane oy, oc;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory bmem = VK_NULL_HANDLE;
	bool ok = false;
	do
	{
		VkSamplerYcbcrConversionCreateInfo yc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
		yc.format = fmt;
		yc.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
		yc.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
		yc.xChromaOffset = yc.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
		yc.chromaFilter = VK_FILTER_LINEAR;
		if ((r = vkCreateSamplerYcbcrConversion(dev, &yc, nullptr, &conv))) { fail(err, "vkCreateSamplerYcbcrConversion", r); break; }
		VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
		ci.conversion = conv;
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.pNext = &ci;
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if ((r = vkCreateSampler(dev, &si, nullptr, &smp))) { fail(err, "vkCreateSampler(ycbcr)", r); break; }
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.pNext = &ci;
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if ((r = vkCreateImageView(dev, &vi, nullptr, &view))) { fail(err, "vkCreateImageView(ycbcr)", r); break; }
		if (!m.create_plane(oy, VK_FORMAT_R16_UNORM, m.y.w, m.y.h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, err) ||
		    !m.create_plane(oc, VK_FORMAT_R16G16_UNORM, m.cbcr.w, m.cbcr.h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, err))
			break;
		VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		smi.codeSize = sizeof(slp_sample_readback_spv);
		smi.pCode = slp_sample_readback_spv;
		if ((r = vkCreateShaderModule(dev, &smi, nullptr, &sm))) { fail(err, "vkCreateShaderModule(readback)", r); break; }
		VkDescriptorSetLayoutBinding b[3] = {
			{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &smp},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		};
		VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dli.bindingCount = 3;
		dli.pBindings = b;
		if ((r = vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl))) { fail(err, "dsl(readback)", r); break; }
		VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 1;
		pli.pSetLayouts = &dsl;
		if ((r = vkCreatePipelineLayout(dev, &pli, nullptr, &pl))) { fail(err, "pl(readback)", r); break; }
		VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cpi.stage.module = sm;
		cpi.stage.pName = "main";
		cpi.layout = pl;
		if ((r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe))) { fail(err, "pipeline(readback)", r); break; }
		// Combined image sampler with a YCbCr conversion may need several descriptors (combinedImageSamplerDescriptorCount).
		VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = 1;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = ps;
		if ((r = vkCreateDescriptorPool(dev, &dpi, nullptr, &dp))) { fail(err, "pool(readback)", r); break; }
		VkDescriptorSet ds;
		VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = dp;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &dsl;
		if ((r = vkAllocateDescriptorSets(dev, &dai, &ds))) { fail(err, "set(readback)", r); break; }
		VkDescriptorImageInfo ii[3] = {
			{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL},
			{VK_NULL_HANDLE, oy.view, VK_IMAGE_LAYOUT_GENERAL},
			{VK_NULL_HANDLE, oc.view, VK_IMAGE_LAYOUT_GENERAL},
		};
		VkWriteDescriptorSet w[3] = {};
		for (int i = 0; i < 3; i++)
		{
			w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w[i].dstSet = ds;
			w[i].dstBinding = uint32_t(i);
			w[i].descriptorCount = 1;
			w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			w[i].pImageInfo = &ii[i];
		}
		vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);

		VkDeviceSize ysz = VkDeviceSize(m.y.w) * m.y.h * 2, csz = VkDeviceSize(m.cbcr.w) * m.cbcr.h * 4;
		VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.size = ysz + csz;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if ((r = vkCreateBuffer(dev, &bci, nullptr, &buf))) { fail(err, "buffer(readback)", r); break; }
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, buf, &mr);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if ((r = vkAllocateMemory(dev, &ai, nullptr, &bmem))) { fail(err, "mem(readback)", r); break; }
		vkBindBufferMemory(dev, buf, bmem, 0);

		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(m.cmd, 0);
		vkBeginCommandBuffer(m.cmd, &bi);
		if (external)
			Decoder::Impl::barrier(m.cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			              VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
			              m.has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL, m.family);
		else
			Decoder::Impl::barrier(m.cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			              VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
		for (Plane *p : {&oy, &oc})
			Decoder::Impl::barrier(m.cmd, p->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
			              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
		vkCmdDispatch(m.cmd, (m.y.w + 7) / 8, (m.y.h + 7) / 8, 1);
		for (Plane *p : {&oy, &oc})
			Decoder::Impl::barrier(m.cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c[2] = {};
		c[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c[0].imageExtent = {m.y.w, m.y.h, 1};
		c[1].bufferOffset = ysz;
		c[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c[1].imageExtent = {m.cbcr.w, m.cbcr.h, 1};
		vkCmdCopyImageToBuffer(m.cmd, oy.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c[0]);
		vkCmdCopyImageToBuffer(m.cmd, oc.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c[1]);
		VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(m.cmd, &di);
		vkEndCommandBuffer(m.cmd);
		VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		sub.commandBufferCount = 1;
		sub.pCommandBuffers = &m.cmd;
		r = vkQueueSubmit(m.queue, 1, &sub, m.fence);
		if (!r)
			r = vkWaitForFences(dev, 1, &m.fence, VK_TRUE, 2000ull * 1000 * 1000);
		vkResetFences(dev, 1, &m.fence);
		if (r) { fail(err, "readback_sampled submit/wait", r); break; }
		void *mp = nullptr;
		vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &mp);
		y.resize(size_t(ysz / 2));
		cbcr.resize(size_t(csz / 2));
		memcpy(y.data(), mp, size_t(ysz));
		memcpy(cbcr.data(), static_cast<uint8_t *>(mp) + ysz, size_t(csz));
		vkUnmapMemory(dev, bmem);
		ok = true;
	} while (false);
	if (buf) vkDestroyBuffer(dev, buf, nullptr);
	if (bmem) vkFreeMemory(dev, bmem, nullptr);
	m.destroy_plane(oy);
	m.destroy_plane(oc);
	if (dp) vkDestroyDescriptorPool(dev, dp, nullptr);
	if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
	if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
	if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
	if (sm) vkDestroyShaderModule(dev, sm, nullptr);
	if (view) vkDestroyImageView(dev, view, nullptr);
	if (smp) vkDestroySampler(dev, smp, nullptr);
	if (conv) vkDestroySamplerYcbcrConversion(dev, conv, nullptr);
	return ok;
}

bool Decoder::readback_sampled(int index, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err)
{
	Impl &m = *impl;
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].readable)
		return fail(err, "readback_sampled: target not readable");
	return sampled_read(m, m.targets[index].image, m.surface_format(), true, y, cbcr, err);
}

namespace
{
struct ExpImage
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
};

// Creates a 2-plane YCbCr image with a DRM modifier: explicit layout if pitch != 0, else a one-entry list.
bool create_mod_image(Decoder::Impl &m, VkFormat fmt, uint32_t w, uint32_t h, uint64_t modifier, uint64_t off1, uint64_t pitch,
                      ExpImage &out, std::string *err)
{
	VkSubresourceLayout pl[2] = {};
	pl[0].offset = 0;
	pl[0].rowPitch = pitch;
	pl[1].offset = off1;
	pl[1].rowPitch = pitch;
	VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	ex.drmFormatModifier = modifier;
	ex.drmFormatModifierPlaneCount = 2;
	ex.pPlaneLayouts = pl;
	VkImageDrmFormatModifierListCreateInfoEXT li = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
	li.drmFormatModifierCount = 1;
	li.pDrmFormatModifiers = &modifier;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.pNext = pitch ? static_cast<const void *>(&ex) : static_cast<const void *>(&li);
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = fmt;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult r = vkCreateImage(m.device, &ii, nullptr, &out.image);
	if (r)
		return fail(err, "vkCreateImage(experiment)", r);
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m.device, out.image, &mr);
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = out.image;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &ded;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &out.mem)))
		return fail(err, "vkAllocateMemory(experiment)", r);
	if ((r = vkBindImageMemory(m.device, out.image, out.mem, 0)))
		return fail(err, "vkBindImageMemory(experiment)", r);
	return true;
}

void destroy_exp(Decoder::Impl &m, ExpImage &e)
{
	if (e.image) vkDestroyImage(m.device, e.image, nullptr);
	if (e.mem) vkFreeMemory(m.device, e.mem, nullptr);
	e = ExpImage();
}

bool submit_wait(Decoder::Impl &m, std::string *err)
{
	vkEndCommandBuffer(m.cmd);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &m.cmd;
	VkResult r = vkQueueSubmit(m.queue, 1, &si, m.fence);
	if (!r)
		r = vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, 2000ull * 1000 * 1000);
	vkResetFences(m.device, 1, &m.fence);
	return r ? fail(err, "experiment submit/wait", r) : true;
}

void begin(Decoder::Impl &m)
{
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(m.cmd, 0);
	vkBeginCommandBuffer(m.cmd, &bi);
}

void full_barrier(VkCommandBuffer c)
{
	VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2(c, &di);
}
} // namespace

bool Decoder::write_experiment(int kind, int method, std::vector<uint16_t> &y, std::vector<uint16_t> &cbcr, std::string *err)
{
	Impl &m = *impl;
	const bool nv12 = kind == 2;
	const VkFormat fmt = nv12 ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM : VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
	const uint32_t W = m.y.w, H = m.y.h;
	ExpImage dst, mid;
	Plane y8, c8;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory bmem = VK_NULL_HANDLE;
	VkShaderModule sm = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorPool dp = VK_NULL_HANDLE;
	bool ok = false;
	do
	{
		bool made = false;
		if (kind == 0)
			made = create_mod_image(m, fmt, W, H, 0, 0, 0, dst, err);
		else if (kind == 1 || kind == 3)
			made = create_mod_image(m, fmt, W, H, 0x0500000000000001ull, 9719808, 1792, dst, err);
		else
			made = create_mod_image(m, fmt, W, H, 0x0500000000000001ull, 7614464, 1408, dst, err);
		if (!made)
			break;
		if (kind == 3 && !create_mod_image(m, fmt, W, H, 0, 0, 0, mid, err))
			break;

		// Source planes (16-bit from the decoder, or converted to 8-bit for NV12).
		VkImage sy = m.y.image, sc = m.cbcr.image;
		uint32_t bpp_y = 2, bpp_c = 4;
		begin(m);
		if (nv12)
		{
			if (!m.create_plane(y8, VK_FORMAT_R8_UNORM, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, err) ||
			    !m.create_plane(c8, VK_FORMAT_R8G8_UNORM, W / 2, H / 2, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, err))
			{
				vkEndCommandBuffer(m.cmd);
				break;
			}
			VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			smi.codeSize = sizeof(slp_to8bit_spv);
			smi.pCode = slp_to8bit_spv;
			vkCreateShaderModule(m.device, &smi, nullptr, &sm);
			VkDescriptorSetLayoutBinding b[4] = {
				{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
				{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
				{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
				{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			};
			VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			dli.bindingCount = 4;
			dli.pBindings = b;
			vkCreateDescriptorSetLayout(m.device, &dli, nullptr, &dsl);
			VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			pli.setLayoutCount = 1;
			pli.pSetLayouts = &dsl;
			vkCreatePipelineLayout(m.device, &pli, nullptr, &pl);
			VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			cpi.stage.module = sm;
			cpi.stage.pName = "main";
			cpi.layout = pl;
			VkResult r = vkCreateComputePipelines(m.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
			if (r)
			{
				vkEndCommandBuffer(m.cmd);
				fail(err, "to8bit pipeline", r);
				break;
			}
			VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
			VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			dpi.maxSets = 1;
			dpi.poolSizeCount = 2;
			dpi.pPoolSizes = ps;
			vkCreateDescriptorPool(m.device, &dpi, nullptr, &dp);
			VkDescriptorSet ds;
			VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			dai.descriptorPool = dp;
			dai.descriptorSetCount = 1;
			dai.pSetLayouts = &dsl;
			vkAllocateDescriptorSets(m.device, &dai, &ds);
			VkDescriptorImageInfo ii[4] = {
				{VK_NULL_HANDLE, m.y.view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, m.cbcr.view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, y8.view, VK_IMAGE_LAYOUT_GENERAL},
				{VK_NULL_HANDLE, c8.view, VK_IMAGE_LAYOUT_GENERAL},
			};
			VkWriteDescriptorSet w[4] = {};
			for (int i = 0; i < 4; i++)
			{
				w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				w[i].dstSet = ds;
				w[i].dstBinding = uint32_t(i);
				w[i].descriptorCount = 1;
				w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				w[i].pImageInfo = &ii[i];
			}
			vkUpdateDescriptorSets(m.device, 4, w, 0, nullptr);
			for (Plane *p : {&y8, &c8})
				Impl::barrier(m.cmd, p->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
				              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
			full_barrier(m.cmd);
			vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
			vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
			vkCmdDispatch(m.cmd, (W + 7) / 8, (H + 7) / 8, 1);
			full_barrier(m.cmd);
			sy = y8.image;
			sc = c8.image;
			bpp_y = 1;
			bpp_c = 2;
		}
		VkImage first = kind == 3 ? mid.image : dst.image;
		Impl::barrier(m.cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
		              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
		if (kind == 3)
			Impl::barrier(m.cmd, mid.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
			              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
		full_barrier(m.cmd);
		if (method == 0)
		{
			VkImageCopy cp[2] = {};
			cp[0].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			cp[0].dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp[0].extent = {W, H, 1};
			cp[1].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			cp[1].dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp[1].extent = {W / 2, H / 2, 1};
			vkCmdCopyImage(m.cmd, sy, VK_IMAGE_LAYOUT_GENERAL, first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp[0]);
			vkCmdCopyImage(m.cmd, sc, VK_IMAGE_LAYOUT_GENERAL, first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp[1]);
		}
		else
		{
			VkDeviceSize ysz = VkDeviceSize(W) * H * bpp_y, csz = VkDeviceSize(W / 2) * (H / 2) * bpp_c;
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = ysz + csz;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			vkCreateBuffer(m.device, &bci, nullptr, &buf);
			VkMemoryRequirements mr;
			vkGetBufferMemoryRequirements(m.device, buf, &mr);
			VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			ai.allocationSize = mr.size;
			ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			vkAllocateMemory(m.device, &ai, nullptr, &bmem);
			vkBindBufferMemory(m.device, buf, bmem, 0);
			VkBufferImageCopy c[2] = {};
			c[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c[0].imageExtent = {W, H, 1};
			c[1].bufferOffset = ysz;
			c[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c[1].imageExtent = {W / 2, H / 2, 1};
			vkCmdCopyImageToBuffer(m.cmd, sy, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c[0]);
			vkCmdCopyImageToBuffer(m.cmd, sc, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c[1]);
			full_barrier(m.cmd);
			c[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
			c[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
			vkCmdCopyBufferToImage(m.cmd, buf, first, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, c);
		}
		full_barrier(m.cmd);
		if (kind == 3)
		{
			Impl::barrier(m.cmd, mid.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
			              VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
			VkImageCopy cp[2] = {};
			cp[0].srcSubresource = cp[0].dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp[0].extent = {W, H, 1};
			cp[1].srcSubresource = cp[1].dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp[1].extent = {W / 2, H / 2, 1};
			vkCmdCopyImage(m.cmd, mid.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cp);
			full_barrier(m.cmd);
		}
		Impl::barrier(m.cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
		              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
		              VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
		if (!submit_wait(m, err))
			break;
		ok = sampled_read(m, dst.image, fmt, false, y, cbcr, err);
	} while (false);
	if (buf) vkDestroyBuffer(m.device, buf, nullptr);
	if (bmem) vkFreeMemory(m.device, bmem, nullptr);
	m.destroy_plane(y8);
	m.destroy_plane(c8);
	if (dp) vkDestroyDescriptorPool(m.device, dp, nullptr);
	if (pipe) vkDestroyPipeline(m.device, pipe, nullptr);
	if (pl) vkDestroyPipelineLayout(m.device, pl, nullptr);
	if (dsl) vkDestroyDescriptorSetLayout(m.device, dsl, nullptr);
	if (sm) vkDestroyShaderModule(m.device, sm, nullptr);
	destroy_exp(m, mid);
	destroy_exp(m, dst);
	return ok;
}

bool Decoder::readback_rgba(int index, std::vector<uint32_t> &px, std::string *err, bool via_import)
{
	Impl &m = *impl;
	if (index < 0 || size_t(index) >= m.targets.size() || !m.targets[index].rimg)
		return fail(err, "readback_rgba: target has no full-colour image");
	Target &t = m.targets[index];
	// v7.21: import our own export exactly like the compositor side of the layer (slp_layer.c cfc_import)
	VkImage src = t.rimg, imp = VK_NULL_HANDLE;
	VkDeviceMemory impmem = VK_NULL_HANDLE;
	struct ImpGuard
	{
		Impl &m;
		VkImage &i;
		VkDeviceMemory &mem;
		~ImpGuard()
		{
			if (i) vkDestroyImage(m.device, i, nullptr);
			if (mem) vkFreeMemory(m.device, mem, nullptr);
		}
	} guard{m, imp, impmem};
	if (via_import)
	{
		RgbaExport ex;
		if (!rgba_export(index, &ex, err))
			return false;
		VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkSubresourceLayout lay = {};
		lay.offset = ex.offset;
		lay.rowPitch = ex.row_pitch;
		VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
		mod.pNext = &ext;
		mod.drmFormatModifier = ex.modifier;
		mod.drmFormatModifierPlaneCount = 1;
		mod.pPlaneLayouts = &lay;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &mod;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		ii.extent = {ex.width, ex.height, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkResult rr = vkCreateImage(m.device, &ii, nullptr, &imp);
		if (rr)
		{
			imp = VK_NULL_HANDLE;
			close(ex.fd);
			return fail(err, "import check: vkCreateImage", rr);
		}
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(m.device, imp, &mr);
		VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
		vkGetMemoryFdPropertiesKHR(m.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, ex.fd, &fp);
		uint32_t bits = mr.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u), type = 0;
		while (type < 32 && !(bits & (1u << type)))
			type++;
		VkImportMemoryFdInfoKHR imf = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		imf.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		imf.fd = ex.fd;
		VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		ded.pNext = &imf;
		ded.image = imp;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &ded;
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = type;
		if (type >= 32 || (ex.size && mr.size > ex.size))
		{
			close(ex.fd);
			return fail(err, "import check: memory type / size");
		}
		if ((rr = vkAllocateMemory(m.device, &mai, nullptr, &impmem)))
		{
			impmem = VK_NULL_HANDLE;
			close(ex.fd);
			return fail(err, "import check: vkAllocateMemory", rr);
		}
		if ((rr = vkBindImageMemory(m.device, imp, impmem, 0)))
			return fail(err, "import check: vkBindImageMemory", rr);
		src = imp;
	}
	const VkDeviceSize n = VkDeviceSize(m.width) * m.height;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = n * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory bmem = VK_NULL_HANDLE;
	VkResult r = vkCreateBuffer(m.device, &bci, nullptr, &buf);
	if (r)
		return fail(err, "vkCreateBuffer(readback RGBA)", r);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(m.device, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	bool ok = false;
	do
	{
		if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m.device, &ai, nullptr, &bmem) != VK_SUCCESS ||
		    vkBindBufferMemory(m.device, buf, bmem, 0) != VK_SUCCESS)
		{
			fail(err, "readback RGBA buffer");
			break;
		}
		vkQueueWaitIdle(m.queue);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(m.cmd, 0);
		vkBeginCommandBuffer(m.cmd, &bi);
		const uint32_t foreign = m.has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
		const uint32_t fsrc = t.rexport ? foreign : VK_QUEUE_FAMILY_IGNORED, fdst = t.rexport ? m.family : VK_QUEUE_FAMILY_IGNORED;
		Impl::barrier(m.cmd, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
		              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT, fsrc, fdst);
		VkBufferImageCopy c = {};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {uint32_t(m.width), uint32_t(m.height), 1};
		vkCmdCopyImageToBuffer(m.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
		Impl::barrier(m.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
		              0, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT, fdst, fsrc);
		vkEndCommandBuffer(m.cmd);
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &m.cmd;
		if ((r = vkQueueSubmit(m.queue, 1, &si, m.fence)))
		{
			fail(err, "vkQueueSubmit(readback RGBA)", r);
			break;
		}
		vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX);
		vkResetFences(m.device, 1, &m.fence);
		void *p = nullptr;
		if (vkMapMemory(m.device, bmem, 0, n * 4, 0, &p) != VK_SUCCESS)
		{
			fail(err, "vkMapMemory(readback RGBA)");
			break;
		}
		px.resize(size_t(n));
		memcpy(px.data(), p, size_t(n) * 4);
		vkUnmapMemory(m.device, bmem);
		ok = true;
	} while (false);
	vkDestroyBuffer(m.device, buf, nullptr);
	if (bmem) vkFreeMemory(m.device, bmem, nullptr);
	return ok;
}

bool Decoder::decode_cpu_reference(const uint8_t *frame, size_t size, std::vector<uint16_t> &y,
                                   std::vector<uint16_t> &cb, std::vector<uint16_t> &cr, std::string *err)
{
	Impl &m = *impl;
	bool complete;
	FrameHeader fh;
	const uint32_t *fsz;
	const uint8_t *fpl;
	if (!parse_frame(frame, size, &fh, &fsz, &fpl))
		return fail(err, "parse SLPW frame");
	const bool c444 = fh.chroma == 1;
	if (c444 && !m.pdec4)
		return fail(err, "4:4:4 reference needs a full-colour decoder");
	pyrowave_decoder dec = c444 ? m.pdec4 : m.pdec;
	if (!push_frame(dec, frame, size, err, &complete))
		return false;
	int cw = c444 ? m.width : m.width / 2, ch = c444 ? m.height : m.height / 2;
	y.assign(size_t(m.width) * m.height, 0);
	cb.assign(size_t(cw) * ch, 0);
	cr.assign(size_t(cw) * ch, 0);
	pyrowave_cpu_buffer b = {};
	b.data[0] = y.data();
	b.data[1] = cb.data();
	b.data[2] = cr.data();
	b.row_stride_in_bytes[0] = size_t(m.width) * 2;
	b.row_stride_in_bytes[1] = b.row_stride_in_bytes[2] = size_t(cw) * 2;
	b.plane_size_in_bytes[0] = y.size() * 2;
	b.plane_size_in_bytes[1] = b.plane_size_in_bytes[2] = cb.size() * 2;
	b.width = m.width;
	b.height = m.height;
	b.format = c444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P16 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P16;
	if (pyrowave_decoder_decode_cpu_buffer_synchronous(dec, &b) != PYROWAVE_SUCCESS)
		return fail(err, "pyrowave_decoder_decode_cpu_buffer_synchronous");
	// The CPU path sets its own scaling factor; restore 1.0 for the GPU path.
	pyrowave_decoder_set_ycbcr_scaling_factor(dec, 1.0f);
	return true;
}
} // namespace slp
