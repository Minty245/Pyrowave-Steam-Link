// Host test of pack_rgba10.comp (v7.19 full colour) on any Vulkan device (lavapipe): R16 Y / Yf / Cb / Cr planes ->
// A2B10G10R10 (LINEAR, like the shared image) -> compared with a CPU reference (channel order, 4:2:0 bilinear at the
// midpoint, 4:4:4 exact, light square / tile mask luma selection, dither within a code).
// Usage: pack_test [W]   (atlas W x 4W)
#include "volk.h"
#include "pack_rgba10_spv.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice gpu;
static VkDevice dev;
static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(2);
}
struct Img { VkImage i; VkDeviceMemory m; VkImageView v; uint32_t w, h; VkFormat f; };
static Img mk(uint32_t w, uint32_t h, VkFormat f, VkImageTiling til, VkImageUsageFlags u)
{
	Img r = {};
	r.w = w; r.h = h; r.f = f;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent = {w, h, 1};
	ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = til; ii.usage = u;
	CK(vkCreateImage(dev, &ii, nullptr, &r.i));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, r.i, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, 0);
	CK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
	CK(vkBindImageMemory(dev, r.i, r.m, 0));
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = r.i; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	CK(vkCreateImageView(dev, &vi, nullptr, &r.v));
	return r;
}

static float bayer4(int x, int y)
{
	x &= 3; y &= 3;
	int q = x ^ y;
	return float(((q & 1) << 3) | ((y & 1) << 2) | (((q >> 1) & 1) << 1) | ((y >> 1) & 1));
}

int main(int argc, char **argv)
{
	const int W = argc > 1 ? atoi(argv[1]) : 128, H = 4 * W, T = W;
	CK(volkInitialize());
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CK(vkCreateInstance(&ici, nullptr, &inst));
	volkLoadInstance(inst);
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &gpu);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueCount = 1; qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	CK(vkCreateDevice(gpu, &dci, nullptr, &dev));
	volkLoadDevice(dev);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	Img y = mk(W, H, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su), yf = mk(W, H, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su);
	Img cb2 = mk(W / 2, H / 2, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su), cr2 = mk(W / 2, H / 2, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su);
	Img cb4 = mk(W, H, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su), cr4 = mk(W, H, VK_FORMAT_R16_UNORM, VK_IMAGE_TILING_OPTIMAL, su);
	Img out = mk(W, H, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_TILING_LINEAR,
	             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	// data
	srand(7);
	auto rnd = [] { return uint16_t(rand() & 0xffff); };
	std::vector<uint16_t> dy(size_t(W) * H), dyf(dy.size()), db2(size_t(W / 2) * (H / 2)), dr2(db2.size()), db4(dy.size()), dr4(dy.size());
	for (int j = 0; j < H; j++)
		for (int i = 0; i < W; i++)
		{
			size_t k = size_t(j) * W + i;
			dy[k] = uint16_t((i * 977 + j * 131) & 0xffff);
			dyf[k] = rnd();
			db4[k] = uint16_t(32768 + 20000 * std::sin(i * 0.37 + j * 0.11));
			dr4[k] = ((i ^ j) & 1) ? 50000 : 15000;   // per-pixel checker
		}
	for (size_t k = 0; k < db2.size(); k++) { db2[k] = rnd(); dr2[k] = rnd(); }

	// staging upload
	size_t total = (dy.size() * 4 + db2.size() * 2) * 2 + size_t(W) * H * 4;
	VkBuffer buf; VkDeviceMemory bm;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = total; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateBuffer(dev, &bci, nullptr, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &bm));
	CK(vkBindBufferMemory(dev, buf, bm, 0));
	uint8_t *map;
	CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&map));
	struct Up { Img *im; std::vector<uint16_t> *d; size_t off; } ups[6] = {{&y, &dy}, {&yf, &dyf}, {&cb2, &db2}, {&cr2, &dr2}, {&cb4, &db4}, {&cr4, &dr4}};
	size_t off = 0;
	for (auto &u : ups) { u.off = off; memcpy(map + off, u.d->data(), u.d->size() * 2); off += u.d->size() * 2; }
	const size_t out_off = off;

	VkCommandPool cp;
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	CK(vkCreateCommandPool(dev, &cpi, nullptr, &cp));
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cba.commandPool = cp; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
	CK(vkAllocateCommandBuffers(dev, &cba, &cb));
	auto bar = [&](VkImage im, VkImageLayout a, VkImageLayout b) {
		VkImageMemoryBarrier m = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		m.oldLayout = a; m.newLayout = b; m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		m.image = im; m.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &m);
	};
	VkFence fence;
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CK(vkCreateFence(dev, &fci, nullptr, &fence));
	auto run = [&](auto rec) {
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &bi);
		rec();
		vkEndCommandBuffer(cb);
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1; si.pCommandBuffers = &cb;
		CK(vkQueueSubmit(q, 1, &si, fence));
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
	};
	run([&] {
		for (auto &u : ups)
		{
			bar(u.im->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy c = {};
			c.bufferOffset = u.off;
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {u.im->w, u.im->h, 1};
			vkCmdCopyBufferToImage(cb, buf, u.im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
			bar(u.im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		}
		bar(out.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	});

	// pipeline (same layout as the decoder's create_pack)
	VkSampler smp;
	VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CK(vkCreateSampler(dev, &sci, nullptr, &smp));
	VkDescriptorSetLayoutBinding b[5] = {
	    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dl.bindingCount = 5; dl.pBindings = b;
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl));
	VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = sizeof(slp_pack_rgba10_spv); smi.pCode = slp_pack_rgba10_spv;
	VkShaderModule sm;
	CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
	VkComputePipelineCreateInfo cpc = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cpc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpc.stage.module = sm; cpc.stage.pName = "main"; cpc.layout = pl;
	VkPipeline pipe;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpc, nullptr, &pipe));
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 2; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
	VkDescriptorSet sets[2];
	VkDescriptorSetLayout two[2] = {dsl, dsl};
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = dp; dai.descriptorSetCount = 2; dai.pSetLayouts = two;
	CK(vkAllocateDescriptorSets(dev, &dai, sets));
	for (int c = 0; c < 2; c++)
	{
		VkDescriptorImageInfo di[5] = {{VK_NULL_HANDLE, y.v, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, yf.v, VK_IMAGE_LAYOUT_GENERAL},
		                               {smp, c ? cb4.v : cb2.v, VK_IMAGE_LAYOUT_GENERAL}, {smp, c ? cr4.v : cr2.v, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, out.v, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[5] = {};
		for (int i = 0; i < 5; i++)
		{
			w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w[i].dstSet = sets[c]; w[i].dstBinding = i; w[i].descriptorCount = 1;
			w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			w[i].pImageInfo = &di[i];
		}
		vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
	}
	VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout lay;
	vkGetImageSubresourceLayout(dev, out.i, &sub, &lay);

	struct PC { float dither, tile, light, sq0, sq1, tilemask; };
	int fails = 0;
	auto test = [&](const char *name, int c444, PC pc, int tol) {
		run([&] {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[c444], 0, nullptr);
			vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
			vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
			bar(out.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
		});
		// read the LINEAR image through its own row pitch (like vrcompositor's import would)
		void *om;
		CK(vkMapMemory(dev, out.m, 0, VK_WHOLE_SIZE, 0, &om));
		int md[3] = {0, 0, 0};
		for (int j = 0; j < H; j++)
			for (int i = 0; i < W; i++)
			{
				uint32_t v = *(const uint32_t *)((const uint8_t *)om + lay.offset + size_t(j) * lay.rowPitch + size_t(i) * 4);
				int R = v & 1023, G = (v >> 10) & 1023, B = (v >> 20) & 1023, A = v >> 30;
				int t = j / T, ly = j - t * T;
				bool fromf = pc.light > 0.5f ? ((t & 1) == 0 && i >= pc.sq0 && i < pc.sq1 && ly >= pc.sq0 && ly < pc.sq1)
				                             : ((int(pc.tilemask) >> t) & 1);
				size_t k = size_t(j) * W + i;
				double ry = (fromf ? dyf[k] : dy[k]) / 65535.0 * 1023.0;
				double rb, rr;
				if (c444) { rb = db4[k] / 65535.0 * 1023.0; rr = dr4[k] / 65535.0 * 1023.0; }
				else
				{
					const int cw = W / 2, ch = H / 2;
					double fx = (i + 0.5) * 0.5 - 0.5, fy = (j + 0.5) * 0.5 - 0.5;
					int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
					double ax = fx - x0, ay = fy - y0;
					auto at = [&](const std::vector<uint16_t> &p, int xx, int yy) {
						xx = xx < 0 ? 0 : xx >= cw ? cw - 1 : xx; yy = yy < 0 ? 0 : yy >= ch ? ch - 1 : yy;
						return p[size_t(yy) * cw + xx] / 65535.0 * 1023.0;
					};
					auto bil = [&](const std::vector<uint16_t> &p) {
						return (at(p, x0, y0) * (1 - ax) + at(p, x0 + 1, y0) * ax) * (1 - ay) + (at(p, x0, y0 + 1) * (1 - ax) + at(p, x0 + 1, y0 + 1) * ax) * ay;
					};
					rb = bil(db2); rr = bil(dr2);
				}
				double d = pc.dither * ((bayer4(i, j) + 0.5) / 16.0 - 0.5);
				double d2 = pc.dither * ((bayer4(i + 2, j + 1) + 0.5) / 16.0 - 0.5);
				auto q = [](double v, double dd) { double r = std::floor(v + 0.5 + dd); return int(r < 0 ? 0 : r > 1023 ? 1023 : r); };
				int ey = std::abs(G - q(ry, d)), eb = std::abs(B - q(rb, -d2)), er = std::abs(R - q(rr, d2));
				md[0] = std::max(md[0], ey); md[1] = std::max(md[1], eb); md[2] = std::max(md[2], er);
				if (A != 3) md[0] = 99;
			}
		vkUnmapMemory(dev, out.m);
		bool ok = md[0] <= tol && md[1] <= tol && md[2] <= tol;
		fails += !ok;
		printf("  %-44s max diff Y %d Cb %d Cr %d (10-bit codes) -> %s\n", name, md[0], md[1], md[2], ok ? "MATCH" : "MISMATCH");
	};
	printf("pack_test: %dx%d, output row pitch %llu\n", W, H, (unsigned long long)lay.rowPitch);
	test("4:2:0, luma from Y", 0, {0, float(T), 0, 0, 0, 0}, 1);
	test("4:4:4 (per-pixel Cr checker), luma from Y", 1, {0, float(T), 0, 0, 0, 0}, 1);
	test("4:4:4, tiles 1 and 3 luma from Yf", 1, {0, float(T), 0, 0, 0, 10}, 1);
	test("4:4:4, all tiles from Yf", 1, {0, float(T), 0, 0, 0, 15}, 1);
	test("4:4:4, light square [T/4, 3T/4) of the insets", 1, {0, float(T), 1, float(T / 4), float(3 * T / 4), 0}, 1);
	test("4:2:0, dither on", 0, {1, float(T), 0, 0, 0, 0}, 1);
	test("4:4:4, dither on", 1, {1, float(T), 0, 0, 0, 0}, 1);
	printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
	(void)out_off;
	return fails ? 1 : 0;
}
