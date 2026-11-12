// Host test of the v7.13 wide-view blur (wideblur_down.comp + wideblur_apply.comp) on any Vulkan device (lavapipe):
// R8 atlas in -> mip chain -> in-place blur of tiles 1 and 3 -> R8 out. Usage: wideblur_test in.raw out.raw W H px contrast dither [ramp overshoot]
#include "volk.h"
#include "wideblur_down_spv.h"
#include "wideblur_apply_spv.h"
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

#include <algorithm>
int main(int argc, char **argv)
{
	if (argc < 8) { printf("usage\n"); return 1; }
	int W = atoi(argv[3]), H = atoi(argv[4]);
	float px = (float)atof(argv[5]), contrast = (float)atof(argv[6]), dither = (float)atof(argv[7]);
	float rampv = argc > 8 ? (float)atof(argv[8]) : 0.0f, ovs = argc > 9 ? (float)atof(argv[9]) : 0.0f;
	// v7.17: TEMPORAL=t [CLAMP=c] B=second.raw: run frame A (argv[1]) then frame B with the history carried over;
	// the output is frame B
	float tmp = getenv("TEMPORAL") ? (float)atof(getenv("TEMPORAL")) : 0.0f, tcl = getenv("CLAMP") ? (float)atof(getenv("CLAMP")) : 1.0f;
	std::vector<uint8_t> in2;
	if (getenv("B")) { in2.resize(size_t(W) * H); FILE *g = fopen(getenv("B"), "rb"); if (!g || fread(in2.data(), 1, in2.size(), g) != in2.size()) return 1; fclose(g); }
	const uint32_t T = uint32_t(H / 4), half = T / 2;
	uint32_t levels = 0;
	for (uint32_t s = half; s >= 8 && levels < 6; s /= 2) levels++;
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
	qci.queueCount = 1; qci.pQueuePriorities = &prio;
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f13;
	f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
	VkDevice dev;
	CK(vkCreateDevice(gpu, &dci, nullptr, &dev));
	volkLoadDevice(dev);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	auto bind_mem = [&](VkImage img, VkDeviceMemory *mem) {
		VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size; ai.memoryTypeIndex = find_mem(gpu, mr.memoryTypeBits, 0);
		CK(vkAllocateMemory(dev, &ai, nullptr, mem)); CK(vkBindImageMemory(dev, img, *mem, 0));
	};
	// luma plane
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8_UNORM; ii.extent = {(uint32_t)W, (uint32_t)H, 1};
	ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImage yi; VkDeviceMemory ym; CK(vkCreateImage(dev, &ii, nullptr, &yi)); bind_mem(yi, &ym);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = yi; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8_UNORM;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView yv; CK(vkCreateImageView(dev, &vi, nullptr, &yv));
	// mip chain (same as Decoder::Impl::create_wideblur)
	ii.format = VK_FORMAT_R16_SFLOAT; ii.extent = {half, half, 1}; ii.mipLevels = levels; ii.arrayLayers = 2;
	ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkImage mi; VkDeviceMemory mm; CK(vkCreateImage(dev, &ii, nullptr, &mi)); bind_mem(mi, &mm);
	vi.image = mi; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = VK_FORMAT_R16_SFLOAT;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 2};
	VkImageView mall; CK(vkCreateImageView(dev, &vi, nullptr, &mall));
	VkImageView mlv[6];
	VkImage hi; VkDeviceMemory hmem; VkImageView hv;
	{ VkImageCreateInfo h2 = ii; h2.extent = {T, T, 1}; h2.mipLevels = 1; h2.usage = VK_IMAGE_USAGE_STORAGE_BIT;
	  CK(vkCreateImage(dev, &h2, nullptr, &hi)); bind_mem(hi, &hmem);
	  VkImageViewCreateInfo v2 = vi; v2.image = hi; v2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2}; CK(vkCreateImageView(dev, &v2, nullptr, &hv)); }
	for (uint32_t l = 0; l < levels; l++) { vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 2}; CK(vkCreateImageView(dev, &vi, nullptr, &mlv[l])); }
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = float(levels - 1);
	VkSampler smp; CK(vkCreateSampler(dev, &si, nullptr, &smp));
	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 12}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
	VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 7; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
	VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
	VkDescriptorSetLayoutBinding bd[3] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	                                      {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	                                      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	VkDescriptorSetLayoutBinding ba[3] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	                                      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	                                      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	VkDescriptorSetLayout dsl_d, dsl_a;
	dl.bindingCount = 3; dl.pBindings = bd; CK(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl_d));
	dl.bindingCount = 3; dl.pBindings = ba; CK(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl_a));
	auto make = [&](VkDescriptorSetLayout dsl, uint32_t pcs, const uint32_t *code, size_t cs, VkPipelineLayout *pl, VkPipeline *pipe) {
		VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pcs};
		VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
		CK(vkCreatePipelineLayout(dev, &pli, nullptr, pl));
		VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = cs; smi.pCode = code;
		VkShaderModule sm; CK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
		VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cp.stage.module = sm; cp.stage.pName = "main"; cp.layout = *pl;
		CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, pipe));
	};
	VkPipelineLayout pld, pla; VkPipeline pd, pa;
	make(dsl_d, 8, slp_wideblur_down_spv, sizeof(slp_wideblur_down_spv), &pld, &pd);
	make(dsl_a, 96, slp_wideblur_apply_spv, sizeof(slp_wideblur_apply_spv), &pla, &pa);
	VkDescriptorSetLayout lays[7]; for (uint32_t i = 0; i < levels; i++) lays[i] = dsl_d; lays[levels] = dsl_a;
	VkDescriptorSet sets[7];
	VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	da.descriptorPool = dp; da.descriptorSetCount = levels + 1; da.pSetLayouts = lays;
	CK(vkAllocateDescriptorSets(dev, &da, sets));
	for (uint32_t l = 0; l < levels; l++) {
		VkDescriptorImageInfo i3[3] = {{VK_NULL_HANDLE, yv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, mall, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, mlv[l], VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[3] = {};
		for (int i = 0; i < 3; i++) { w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[i].dstSet = sets[l]; w[i].dstBinding = i; w[i].descriptorCount = 1;
			w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &i3[i]; }
		vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
	}
	VkDescriptorImageInfo i2[3] = {{VK_NULL_HANDLE, yv, VK_IMAGE_LAYOUT_GENERAL}, {smp, mall, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, hv, VK_IMAGE_LAYOUT_GENERAL}};
	VkWriteDescriptorSet w2[3] = {};
	for (int i = 0; i < 3; i++) { w2[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w2[i].dstSet = sets[levels]; w2[i].dstBinding = i; w2[i].descriptorCount = 1;
		w2[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w2[i].pImageInfo = &i2[i]; }
	vkUpdateDescriptorSets(dev, 3, w2, 0, nullptr);
	VkBuffer buf; VkDeviceMemory bm;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = in.size();
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateBuffer(dev, &bci, nullptr, &buf));
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
	ai.memoryTypeIndex = find_mem(gpu, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &bm)); CK(vkBindBufferMemory(dev, buf, bm, 0));
	void *map; CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, &map)); memcpy(map, in.data(), in.size());
	VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool cpool; CK(vkCreateCommandPool(dev, &cpci, nullptr, &cpool));
	VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cba.commandPool = cpool; cba.commandBufferCount = 1;
	VkCommandBuffer c; CK(vkAllocateCommandBuffers(dev, &cba, &c));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(c, &bi));
	auto bar = [&](VkImage img, VkImageLayout from, uint32_t nl, uint32_t nlay) {
		VkImageMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		mb.oldLayout = from; mb.newLayout = VK_IMAGE_LAYOUT_GENERAL; mb.image = img;
		mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, nl, 0, nlay};
		VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(c, &d);
	};
	bar(yi, VK_IMAGE_LAYOUT_UNDEFINED, 1, 1); bar(mi, VK_IMAGE_LAYOUT_UNDEFINED, levels, 2); bar(hi, VK_IMAGE_LAYOUT_UNDEFINED, 1, 2);
	VkBufferImageCopy bic = {}; bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bic.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
	int nframes = in2.empty() ? 1 : 2;
	VkBuffer buf2 = VK_NULL_HANDLE; VkDeviceMemory bm2; void *map2 = nullptr;
	if (nframes == 2) { VkBufferCreateInfo b2 = bci; CK(vkCreateBuffer(dev, &b2, nullptr, &buf2)); VkMemoryRequirements r2; vkGetBufferMemoryRequirements(dev, buf2, &r2);
		VkMemoryAllocateInfo a2 = ai; a2.allocationSize = r2.size; CK(vkAllocateMemory(dev, &a2, nullptr, &bm2)); CK(vkBindBufferMemory(dev, buf2, bm2, 0));
		CK(vkMapMemory(dev, bm2, 0, VK_WHOLE_SIZE, 0, &map2)); memcpy(map2, in2.data(), in2.size()); }
	for (int frame = 0; frame < nframes; frame++) {
	if (frame) { bar(yi, VK_IMAGE_LAYOUT_GENERAL, 1, 1); bar(hi, VK_IMAGE_LAYOUT_GENERAL, 1, 2); }
	vkCmdCopyBufferToImage(c, frame ? buf2 : buf, yi, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
	bar(yi, VK_IMAGE_LAYOUT_GENERAL, 1, 1);
	vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pd);
	for (uint32_t l = 0; l < levels; l++) {
		uint32_t sz = half >> l; struct { int32_t level, tile; } dpc = {int32_t(l), int32_t(T)};
		vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pld, 0, 1, &sets[l], 0, nullptr);
		vkCmdPushConstants(c, pld, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &dpc);
		vkCmdDispatch(c, (sz + 7) / 8, (sz + 7) / 8, 2);
		bar(mi, VK_IMAGE_LAYOUT_GENERAL, levels, 2);
	}
	float max_lod = float(levels) - 1.5f;
	float lod = px;   // v7.16: the shader takes px
	// rp, pj0 (ax bx ay by), pj1, cen (eye0 cx cy, eye1 cx cy): Steam Frame default projection, gaze straight ahead
	struct { float lod, contrast, dither, tile, max_lod, tmp, tcl, hok; float r[16]; } apc = {lod, contrast, dither, float(T), max_lod, tmp, tcl, frame ? 1.0f : 0.0f,
		{rampv, 15.0f, 55.0f, ovs, 0.32f, 0.58f, -0.33f, 0.40f, 0.32f, 0.42f, -0.33f, 0.40f, 0.58f, 0.40f, 0.42f, 0.40f}};
	vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pa);
	vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pla, 0, 1, &sets[levels], 0, nullptr);
	vkCmdPushConstants(c, pla, VK_SHADER_STAGE_COMPUTE_BIT, 0, 96, &apc);
	vkCmdDispatch(c, (T + 15) / 16, (T + 15) / 16, 2);
	}
	bar(yi, VK_IMAGE_LAYOUT_GENERAL, 1, 1);
	if (getenv("DUMP_MIP")) {   // debug: mip level 0, both layers (R16F) instead of the luma
		VkBufferImageCopy mc = {}; mc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2}; mc.imageExtent = {half, half, 1};
		vkCmdCopyImageToBuffer(c, mi, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &mc);
	} else
		vkCmdCopyImageToBuffer(c, yi, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &bic);
	CK(vkEndCommandBuffer(c));
	VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; sub.commandBufferCount = 1; sub.pCommandBuffers = &c;
	CK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
	memcpy(out.data(), map, out.size());
	f = fopen(argv[2], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
	printf("ok levels %u px %.3f\n", levels, px);
	return 0;
}
