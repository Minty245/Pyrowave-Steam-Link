// Host test of postfx.comp on any Vulkan device (lavapipe): R8 in -> shader -> R8 out, compared with a CPU
// reference of the same math. Usage: postfx_test in.raw out.raw W H sigma_r sharpen r0 r1 dither
#include "volk.h"
#include "postfx_spv.h"
#include "seam_spv.h"
#include "sharpen_spv.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); exit(1); } } while (0)

static uint32_t find_mem(VkPhysicalDevice g, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(g, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 10) { printf("usage\n"); return 1; }
	int W = atoi(argv[3]), H = atoi(argv[4]);
	// argv: in out W H sigma_inset sharpen r0 r1 dither [sigma_wide] [wide_step]
	struct { float sig_i, sig_w, sharpen, r0, r1, dither, tile, y0, step; } pc = {(float)atof(argv[5]),
		argc > 10 ? (float)atof(argv[10]) : (float)atof(argv[5]), (float)atof(argv[6]), (float)atof(argv[7]),
		(float)atof(argv[8]), (float)atof(argv[9]), float(H / 4), 0.0f, argc > 11 ? (float)atof(argv[11]) : 1.0f};
	std::vector<uint8_t> in(size_t(W) * H), out(in.size());
	FILE *f = fopen(argv[1], "rb");
	if (!f || fread(in.data(), 1, in.size(), f) != in.size()) { printf("read fail\n"); return 1; }
	fclose(f);

	CK(volkInitialize());
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CK(vkCreateInstance(&ici, nullptr, &inst));
	volkLoadInstance(inst);
	uint32_t n = 1;
	VkPhysicalDevice gpu;
	vkEnumeratePhysicalDevices(inst, &n, &gpu);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.synchronization2 = VK_TRUE;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f13;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice dev;
	CK(vkCreateDevice(gpu, &dci, nullptr, &dev));
	volkLoadDevice(dev);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	auto mk_img = [&](VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkImageUsageFlags u) {
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8_UNORM; ii.extent = {(uint32_t)W, (uint32_t)H, 1};
		ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = u;
		CK(vkCreateImage(dev, &ii, nullptr, img));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(dev, *img, &mr);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = find_mem(gpu, mr.memoryTypeBits, 0);
		CK(vkAllocateMemory(dev, &ai, nullptr, mem));
		CK(vkBindImageMemory(dev, *img, *mem, 0));
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8_UNORM;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		CK(vkCreateImageView(dev, &vi, nullptr, view));
	};
	VkImage si, di; VkDeviceMemory sm, dm; VkImageView sv, dv;
	const bool seam = getenv("SEAM") != nullptr;   // SEAM=1: run seam.comp in place (args: alpha_inset alpha_wide)
	mk_img(&si, &sm, &sv, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	mk_img(&di, &dm, &dv, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	VkBuffer buf; VkDeviceMemory bm;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = in.size();
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateBuffer(dev, &bci, nullptr, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(gpu, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &bm));
	CK(vkBindBufferMemory(dev, buf, bm, 0));
	void *map;
	CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, &map));
	memcpy(map, in.data(), in.size());

	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	const bool light = getenv("LIGHT") != nullptr;  // LIGHT=1: sharpen.comp over the whole image (x0 = y0 = 0)
	smi.codeSize = seam ? sizeof(slp_seam_spv) : light ? sizeof(slp_sharpen_spv) : sizeof(slp_postfx_spv);
	smi.pCode = seam ? slp_seam_spv : light ? slp_sharpen_spv : slp_postfx_spv;
	VkShaderModule shm;
	CK(vkCreateShaderModule(dev, &smi, nullptr, &shm));
	VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	                                     {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	VkDescriptorSetLayoutBinding sb1 = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
	if (seam) { dli.bindingCount = 1; dli.pBindings = &sb1; } else { dli.bindingCount = 2; dli.pBindings = b; }
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
	VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, seam ? 112u : 36u};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
	VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = shm; cpi.stage.pName = "main";
	cpi.layout = pl;
	VkPipeline pipe;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
	VkDescriptorSet ds;
	CK(vkAllocateDescriptorSets(dev, &dai, &ds));
	VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, sv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dv, VK_IMAGE_LAYOUT_GENERAL}};
	VkWriteDescriptorSet w[2] = {};
	for (int i = 0; i < 2; i++) {
		w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w[i].dstSet = ds; w[i].dstBinding = i; w[i].descriptorCount = 1;
		w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		w[i].pImageInfo = &ii[i];
	}
	if (seam) {
		VkDescriptorImageInfo s1 = {VK_NULL_HANDLE, sv, VK_IMAGE_LAYOUT_GENERAL};
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &s1;
		vkUpdateDescriptorSets(dev, 1, w, 0, nullptr);
	} else
		vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);

	VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	VkCommandPool cp;
	CK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
	VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cba.commandPool = cp; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
	VkCommandBuffer c;
	CK(vkAllocateCommandBuffers(dev, &cba, &c));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CK(vkBeginCommandBuffer(c, &bi));
	auto bar = [&](VkImage img, VkImageLayout from, VkImageLayout to) {
		VkImageMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		mb.oldLayout = from; mb.newLayout = to; mb.image = img;
		mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(c, &d);
	};
	bar(si, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	bar(di, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	VkBufferImageCopy bic = {};
	bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	bic.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
	vkCmdCopyBufferToImage(c, buf, si, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
	bar(si, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
	if (seam) {
		// v7.14: RAMP=x adds the shared edge ramp (Steam Frame default projection, gaze straight ahead), cap = deblock 1.0
		float rampv = getenv("RAMP") ? (float)atof(getenv("RAMP")) : 0.0f;
		struct { float ai, aw, tile, dir, grid, ramp, amax, p0, p1, p2, p3, p4; float r[16]; } sp = {(float)atof(argv[5]), (float)atof(argv[6]), float(H / 4), 0, 16, 4,
			10.0f / 255.0f, 0, 0, 0, 0, 0, {rampv, 15.0f, 55.0f, 0.0f, 0.32f, 0.58f, -0.33f, 0.40f, 0.32f, 0.42f, -0.33f, 0.40f, 0.58f, 0.40f, 0.42f, 0.40f}};
		int coarse = getenv("COARSE") ? atoi(getenv("COARSE")) : 0;
		const int grids[3] = {64, 32, 16};
		for (int pass = 0; pass < 6; pass++) {
			int g = grids[pass / 2], dir = pass & 1;
			if ((g == 64 && coarse < 2) || (g == 32 && coarse < 1)) continue;
			sp.dir = float(dir); sp.grid = float(g); sp.ramp = float(g / 4);
			uint32_t lines = dir ? H / g - 1 : W / g - 1, along = dir ? W : H;
			vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 112, &sp);
			vkCmdDispatch(c, (lines * along + 63) / 64, 1, 1);
			bar(si, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
		}
		vkCmdCopyImageToBuffer(c, si, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
	} else {
		if (light) { pc.sig_i = 0; pc.y0 = 0; }
		vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 36, &pc);
		if (light) vkCmdDispatch(c, (W + 7) / 8, (H + 7) / 8, 1);
		else vkCmdDispatch(c, (W + 15) / 16, (H + 15) / 16, 1);
		bar(di, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
		vkCmdCopyImageToBuffer(c, di, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
	}
	CK(vkEndCommandBuffer(c));
	VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	sub.commandBufferCount = 1; sub.pCommandBuffers = &c;
	CK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
	memcpy(out.data(), map, out.size());
	f = fopen(argv[2], "wb");
	fwrite(out.data(), 1, out.size(), f);
	fclose(f);
	printf("ok\n");
	return 0;
}
