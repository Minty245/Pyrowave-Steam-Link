// Steam Link Pyrowave Frame layer v7.26: HEVC full colour (Steam Link Full Colour). See slp_unpack.hpp, unpack.comp.
// SPDX-License-Identifier: MIT
//
// Own small Vulkan device (no Pyrowave). Per decoder surface: a read-only import of vrlink's dma-buf with a YCbCr view
// (identity model, full range, midpoint, NEAREST: raw samples), and a shared 10-bit RGBA image of the same size
// (exported to vrcompositor exactly like the Pyrowave decoder's full-colour images). unpack() runs one compute pass.
#include "slp_unpack.hpp"
#include "slp_pyro_dec.hpp"

#include "volk.h"
#include "unpack_spv.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>

namespace slp
{
namespace
{
bool ufail(std::string *err, const char *what, VkResult r = VK_SUCCESS)
{
	if (err)
	{
		char buf[256];
		if (r != VK_SUCCESS) snprintf(buf, sizeof(buf), "%s failed (%d)", what, int(r));
		else snprintf(buf, sizeof(buf), "%s failed", what);
		*err = buf;
	}
	return false;
}

struct Surf
{
	bool used = false;
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0;
	bool external = false;               // imported dma-buf (vs self-test local image)
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkImage rimg = VK_NULL_HANDLE;
	VkDeviceMemory rmem = VK_NULL_HANDLE;
	VkImageView rview = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	uint64_t rmod = 0, roffset = 0, rpitch = 0, rsize = 0;
	bool rexport = false;
	int pipe = -1;
};

struct Pipe
{
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
	VkSampler smp = VK_NULL_HANDLE;
	VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
};
} // namespace

struct Unpacker::Impl
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memprops = {};
	VkPhysicalDeviceProperties props = {};
	bool has_modifiers = false, has_foreign = false;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkShaderModule sh = VK_NULL_HANDLE;
	std::vector<Pipe> pipes;
	std::vector<Surf> surfs;
	std::mutex mu;

	uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) const
	{
		for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
		return UINT32_MAX;
	}
	uint32_t foreign() const { return has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL; }

	static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
	                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da, uint32_t sf = VK_QUEUE_FAMILY_IGNORED,
	                    uint32_t df = VK_QUEUE_FAMILY_IGNORED)
	{
		VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
		b.oldLayout = from; b.newLayout = to;
		b.srcQueueFamilyIndex = sf; b.dstQueueFamilyIndex = df;
		b.image = img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b;
		vkCmdPipelineBarrier2(c, &di);
	}

	bool submit_wait(std::string *err, const char *what)
	{
		VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		s.commandBufferCount = 1;
		s.pCommandBuffers = &cmd;
		VkResult r = vkQueueSubmit(queue, 1, &s, fence);
		if (r) return ufail(err, what, r);
		r = vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull);
		vkResetFences(device, 1, &fence);
		return r ? ufail(err, "vkWaitForFences", r) : true;
	}
	void begin()
	{
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(cmd, 0);
		vkBeginCommandBuffer(cmd, &bi);
	}

	int pipe_for(VkFormat fmt, std::string *err)
	{
		for (size_t i = 0; i < pipes.size(); i++) if (pipes[i].fmt == fmt) return int(i);
		Pipe p;
		p.fmt = fmt;
		VkSamplerYcbcrConversionCreateInfo cc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
		cc.format = fmt;
		cc.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
		cc.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
		cc.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		                 VK_COMPONENT_SWIZZLE_IDENTITY};
		cc.xChromaOffset = cc.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
		cc.chromaFilter = VK_FILTER_NEAREST;
		VkResult r = vkCreateSamplerYcbcrConversion(device, &cc, nullptr, &p.conv);
		if (r) return ufail(err, "vkCreateSamplerYcbcrConversion", r), -1;
		VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
		ci.conversion = p.conv;
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.pNext = &ci;
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if ((r = vkCreateSampler(device, &si, nullptr, &p.smp))) return ufail(err, "vkCreateSampler(YCbCr)", r), -1;
		VkDescriptorSetLayoutBinding b[2] = {};
		b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1;
		b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b[0].pImmutableSamplers = &p.smp;
		b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[1].descriptorCount = 1;
		b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		dl.bindingCount = 2; dl.pBindings = b;
		if ((r = vkCreateDescriptorSetLayout(device, &dl, nullptr, &p.dsl))) return ufail(err, "vkCreateDescriptorSetLayout", r), -1;
		VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
		VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
		if ((r = vkCreatePipelineLayout(device, &pli, nullptr, &p.pl))) return ufail(err, "vkCreatePipelineLayout", r), -1;
		VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cp.stage.module = sh;
		cp.stage.pName = "main";
		cp.layout = p.pl;
		if ((r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &p.pipe)))
			return ufail(err, "vkCreateComputePipelines(unpack)", r), -1;
		// YCbCr immutable samplers may take several descriptors each (combinedImageSamplerDescriptorCount)
		VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 * 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}};
		VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		dp.maxSets = 64; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
		if ((r = vkCreateDescriptorPool(device, &dp, nullptr, &p.pool))) return ufail(err, "vkCreateDescriptorPool", r), -1;
		pipes.push_back(p);
		return int(pipes.size() - 1);
	}

	bool create_rgba(Surf &t, std::string *err)
	{
		const VkFormat fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		const uint64_t linear = 0;   // DRM_FORMAT_MOD_LINEAR (vrcompositor imports it with this modifier)
		VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
		ml.drmFormatModifierCount = 1;
		ml.pDrmFormatModifiers = &linear;
		VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.pNext = &ml;
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = has_modifiers ? &ext : nullptr;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fmt;
		ii.extent = {t.w, t.h, 1};
		ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = has_modifiers ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkResult r = vkCreateImage(device, &ii, nullptr, &t.rimg);
		if (r) return ufail(err, "vkCreateImage(full-colour RGBA)", r);
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
		if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, 0);
		if ((r = vkAllocateMemory(device, &ai, nullptr, &t.rmem))) return ufail(err, "vkAllocateMemory(full-colour RGBA)", r);
		if ((r = vkBindImageMemory(device, t.rimg, t.rmem, 0))) return ufail(err, "vkBindImageMemory(full-colour RGBA)", r);
		t.rexport = has_modifiers;
		t.rsize = mr.size;
		t.rmod = 0;
		if (has_modifiers) {
			VkImageSubresource sub = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
			VkSubresourceLayout lay = {};
			vkGetImageSubresourceLayout(device, t.rimg, &sub, &lay);
			t.roffset = lay.offset;
			t.rpitch = lay.rowPitch;
		}
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = t.rimg;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if ((r = vkCreateImageView(device, &vi, nullptr, &t.rview))) return ufail(err, "vkCreateImageView(full-colour RGBA)", r);
		// black, then out to the foreign family (vrcompositor)
		begin();
		barrier(cmd, t.rimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
		        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue black = {};
		black.float32[0] = black.float32[2] = 512.0f / 1023.0f;
		black.float32[3] = 1.0f;
		VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
		barrier(cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
		        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, has_modifiers ? family : VK_QUEUE_FAMILY_IGNORED,
		        has_modifiers ? foreign() : VK_QUEUE_FAMILY_IGNORED);
		vkEndCommandBuffer(cmd);
		return submit_wait(err, "vkQueueSubmit(RGBA clear)");
	}

	bool finish_surface(Surf &t, std::string *err)
	{
		VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
		ci.conversion = pipes[t.pipe].conv;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.pNext = &ci;
		vi.image = t.img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = t.fmt;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkResult r = vkCreateImageView(device, &vi, nullptr, &t.view);
		if (r) return ufail(err, "vkCreateImageView(YCbCr surface)", r);
		if (!create_rgba(t, err)) return false;
		VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		da.descriptorPool = pipes[t.pipe].pool;
		da.descriptorSetCount = 1;
		da.pSetLayouts = &pipes[t.pipe].dsl;
		if ((r = vkAllocateDescriptorSets(device, &da, &t.set))) { t.set = VK_NULL_HANDLE; return ufail(err, "vkAllocateDescriptorSets", r); }
		VkDescriptorImageInfo di[2] = {{VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, t.rview, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[2] = {};
		for (int i = 0; i < 2; i++) {
			w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			w[i].dstSet = t.set;
			w[i].dstBinding = uint32_t(i);
			w[i].descriptorCount = 1;
			w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w[i].pImageInfo = &di[i];
		}
		vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
		return true;
	}

	void destroy_surface(Surf &t)
	{
		if (t.set && t.pipe >= 0) vkFreeDescriptorSets(device, pipes[t.pipe].pool, 1, &t.set);
		if (t.view) vkDestroyImageView(device, t.view, nullptr);
		if (t.img) vkDestroyImage(device, t.img, nullptr);
		if (t.mem) vkFreeMemory(device, t.mem, nullptr);
		if (t.rview) vkDestroyImageView(device, t.rview, nullptr);
		if (t.rimg) vkDestroyImage(device, t.rimg, nullptr);
		if (t.rmem) vkFreeMemory(device, t.rmem, nullptr);
		t = Surf();
	}

	int store(const Surf &t)
	{
		for (size_t i = 0; i < surfs.size(); i++)
			if (!surfs[i].used) { surfs[i] = t; return int(i); }
		surfs.push_back(t);
		return int(surfs.size() - 1);
	}
};

Unpacker::Unpacker() : impl(new Impl) {}

Unpacker::~Unpacker()
{
	Impl &m = *impl;
	if (m.device) {
		vkDeviceWaitIdle(m.device);
		for (auto &t : m.surfs) if (t.used) m.destroy_surface(t);
		for (auto &p : m.pipes) {
			if (p.pool) vkDestroyDescriptorPool(m.device, p.pool, nullptr);
			if (p.pipe) vkDestroyPipeline(m.device, p.pipe, nullptr);
			if (p.pl) vkDestroyPipelineLayout(m.device, p.pl, nullptr);
			if (p.dsl) vkDestroyDescriptorSetLayout(m.device, p.dsl, nullptr);
			if (p.smp) vkDestroySampler(m.device, p.smp, nullptr);
			if (p.conv) vkDestroySamplerYcbcrConversion(m.device, p.conv, nullptr);
		}
		if (m.sh) vkDestroyShaderModule(m.device, m.sh, nullptr);
		if (m.fence) vkDestroyFence(m.device, m.fence, nullptr);
		if (m.pool) vkDestroyCommandPool(m.device, m.pool, nullptr);
		vkDestroyDevice(m.device, nullptr);
	}
	if (m.instance) vkDestroyInstance(m.instance, nullptr);
	delete impl;
}

bool Unpacker::init(std::string *err)
{
	Impl &m = *impl;
	VkResult r = volkInitialize();
	if (r) return ufail(err, "volkInitialize (libvulkan.so.1)", r);
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "slp-hevc-full-colour";
	app.pEngineName = "slp";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if ((r = vkCreateInstance(&ici, nullptr, &m.instance))) return ufail(err, "vkCreateInstance", r);
	volkLoadInstanceOnly(m.instance);
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(m.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> gpus(n);
	vkEnumeratePhysicalDevices(m.instance, &n, gpus.data());
	for (auto g : gpus) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(g, &p);
		if (p.apiVersion >= VK_API_VERSION_1_3) { m.gpu = g; m.props = p; break; }
	}
	if (!m.gpu) return ufail(err, "no Vulkan 1.3 GPU");
	vkGetPhysicalDeviceMemoryProperties(m.gpu, &m.memprops);
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m.gpu, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(m.gpu, &nq, qf.data());
	m.family = UINT32_MAX;
	for (uint32_t i = 0; i < nq; i++)
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { m.family = i; break; }
	if (m.family == UINT32_MAX) return ufail(err, "no compute queue family");

	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	f11.pNext = &f13;
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &f11;
	vkGetPhysicalDeviceFeatures2(m.gpu, &f2);
	if (!f11.samplerYcbcrConversion) return ufail(err, "samplerYcbcrConversion not supported");
	if (!f13.synchronization2) return ufail(err, "synchronization2 not supported");
	VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	e13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	e11.pNext = &e13;
	e11.samplerYcbcrConversion = VK_TRUE;
	VkPhysicalDeviceFeatures2 e2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	e2.pNext = &e11;
	e2.features.shaderStorageImageWriteWithoutFormat = f2.features.shaderStorageImageWriteWithoutFormat;

	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(m.gpu, nullptr, &ne, nullptr);
	std::vector<VkExtensionProperties> exts(ne);
	vkEnumerateDeviceExtensionProperties(m.gpu, nullptr, &ne, exts.data());
	auto has = [&](const char *name) {
		for (auto &e : exts) if (!strcmp(e.extensionName, name)) return true;
		return false;
	};
	std::vector<const char *> dev_exts;
	for (const char *w : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	                      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME})
		if (has(w)) dev_exts.push_back(w);
	m.has_foreign = has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	m.has_modifiers = has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) && has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = m.family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &e2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = uint32_t(dev_exts.size());
	dci.ppEnabledExtensionNames = dev_exts.data();
	if ((r = vkCreateDevice(m.gpu, &dci, nullptr, &m.device))) return ufail(err, "vkCreateDevice", r);
	volkLoadDevice(m.device);
	vkGetDeviceQueue(m.device, m.family, 0, &m.queue);
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = m.family;
	if ((r = vkCreateCommandPool(m.device, &cpi, nullptr, &m.pool))) return ufail(err, "vkCreateCommandPool", r);
	VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cba.commandPool = m.pool;
	cba.commandBufferCount = 1;
	if ((r = vkAllocateCommandBuffers(m.device, &cba, &m.cmd))) return ufail(err, "vkAllocateCommandBuffers", r);
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if ((r = vkCreateFence(m.device, &fci, nullptr, &m.fence))) return ufail(err, "vkCreateFence", r);
	VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = sizeof(slp_unpack_spv);
	smi.pCode = slp_unpack_spv;
	if ((r = vkCreateShaderModule(m.device, &smi, nullptr, &m.sh))) return ufail(err, "vkCreateShaderModule(unpack)", r);
	return true;
}

std::string Unpacker::device_name() const { return impl->props.deviceName; }

int Unpacker::import_surface(const SurfaceDesc &d, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	if (!m.has_modifiers) return ufail(err, "no dma-buf / DRM modifier support"), -1;
	Surf t;
	t.fmt = VkFormat(d.vk_format);
	t.w = d.width; t.h = d.height;
	t.external = true;
	t.pipe = m.pipe_for(t.fmt, err);
	if (t.pipe < 0) return -1;
	VkSubresourceLayout lay[2] = {};
	for (int i = 0; i < 2; i++) { lay[i].offset = d.plane_offset[i]; lay[i].rowPitch = d.plane_pitch[i]; }
	VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	mod.pNext = &ext;
	mod.drmFormatModifier = d.modifier;
	mod.drmFormatModifierPlaneCount = 2;
	mod.pPlaneLayouts = lay;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.pNext = &mod;
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = t.fmt;
	ii.extent = {d.width, d.height, 1};
	ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult r = vkCreateImage(m.device, &ii, nullptr, &t.img);
	if (r) return ufail(err, "vkCreateImage(import surface)", r), -1;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m.device, t.img, &mr);
	int fd = dup(d.dmabuf_fd);
	if (fd < 0) { m.destroy_surface(t); return ufail(err, "dup(dma-buf fd)"), -1; }
	VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	vkGetMemoryFdPropertiesKHR(m.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
	uint32_t bits = mr.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
	VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = fd;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = &imp;
	ded.image = t.img;
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.pNext = &ded;
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(bits, 0);
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &t.mem))) {
		close(fd);
		m.destroy_surface(t);
		return ufail(err, "vkAllocateMemory(import surface)", r), -1;
	}
	if ((r = vkBindImageMemory(m.device, t.img, t.mem, 0))) { m.destroy_surface(t); return ufail(err, "vkBindImageMemory(import)", r), -1; }
	if (!m.finish_surface(t, err)) { m.destroy_surface(t); return -1; }
	t.used = true;
	return m.store(t);
}

void Unpacker::release(int index)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	if (index < 0 || size_t(index) >= m.surfs.size() || !m.surfs[index].used) return;
	vkQueueWaitIdle(m.queue);
	m.destroy_surface(m.surfs[index]);
}

bool Unpacker::unpack(int index, const PackedLayout &l, double *ms, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	if (index < 0 || size_t(index) >= m.surfs.size() || !m.surfs[index].used) return ufail(err, "unpack: unknown surface");
	Surf &t = m.surfs[index];
	// v7.31: the decoder pads its surfaces (width to its tile alignment, e.g. 1280 -> 1344, 704 -> 768; height to 32), so
	// the picture only has to fit inside the surface
	if (l.width > int(t.w) || l.packed_height > int(t.h) || l.height <= 0 || l.height > l.packed_height || l.width < 2)
		return ufail(err, "unpack: layout does not fit the surface");
	auto t0 = std::chrono::steady_clock::now();
	const Pipe &p = m.pipes[t.pipe];
	m.begin();
	const uint32_t fam = t.external ? m.foreign() : VK_QUEUE_FAMILY_IGNORED;
	const uint32_t rfam = t.rexport ? m.foreign() : VK_QUEUE_FAMILY_IGNORED;
	const uint32_t own = (t.external || t.rexport) ? m.family : VK_QUEUE_FAMILY_IGNORED;
	// acquire: the decoder wrote the surface (V4L2 says the frame is done); vrcompositor sampled the RGBA image before
	Impl::barrier(m.cmd, t.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
	              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, fam,
	              t.external ? own : VK_QUEUE_FAMILY_IGNORED);
	Impl::barrier(m.cmd, t.rimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
	              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, rfam,
	              t.rexport ? own : VK_QUEUE_FAMILY_IGNORED);
	vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
	vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &t.set, 0, nullptr);
	int32_t pc[8] = {l.width, l.height, l.packed_height, l.strip, l.mode, int32_t(t.w), int32_t(t.h), 0};
	vkCmdPushConstants(m.cmd, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
	vkCmdDispatch(m.cmd, (t.w + 15) / 16, (t.h + 15) / 16, 1);
	// release both back
	Impl::barrier(m.cmd, t.rimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, t.rexport ? own : VK_QUEUE_FAMILY_IGNORED, rfam);
	Impl::barrier(m.cmd, t.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0, t.external ? own : VK_QUEUE_FAMILY_IGNORED, fam);
	vkEndCommandBuffer(m.cmd);
	bool ok = m.submit_wait(err, "vkQueueSubmit(unpack)");
	if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return ok;
}

bool Unpacker::rgba_export(int index, RgbaExport *out, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	if (index < 0 || size_t(index) >= m.surfs.size() || !m.surfs[index].used) return ufail(err, "rgba_export: unknown surface");
	Surf &t = m.surfs[index];
	if (!t.rexport) return ufail(err, "rgba_export: not exportable on this device");
	VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = t.rmem;
	gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	int fd = -1;
	VkResult r = vkGetMemoryFdKHR(m.device, &gi, &fd);
	if (r || fd < 0) return ufail(err, "vkGetMemoryFdKHR(RGBA)", r);
	out->fd = fd;
	out->width = t.w;
	out->height = t.h;
	out->vk_format = uint32_t(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
	out->modifier = t.rmod;
	out->offset = t.roffset;
	out->row_pitch = t.rpitch;
	out->size = t.rsize;
	return true;
}

int Unpacker::create_test_surface(uint32_t w, uint32_t h, uint32_t vk_format, const std::vector<uint16_t> &y,
                                  const std::vector<uint16_t> &cbcr, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	Surf t;
	t.fmt = VkFormat(vk_format);
	t.w = w; t.h = h;
	t.pipe = m.pipe_for(t.fmt, err);
	if (t.pipe < 0) return -1;
	const bool eight = t.fmt == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = t.fmt;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult r = vkCreateImage(m.device, &ii, nullptr, &t.img);
	if (r) return ufail(err, "vkCreateImage(test surface)", r), -1;
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(m.device, t.img, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, 0);
	if ((r = vkAllocateMemory(m.device, &ai, nullptr, &t.mem)) || (r = vkBindImageMemory(m.device, t.img, t.mem, 0)))
		return m.destroy_surface(t), ufail(err, "test surface memory", r), -1;
	// staging upload of both planes
	const size_t bpp = eight ? 1 : 2;
	const size_t ysz = size_t(w) * h * bpp, csz = size_t(w / 2) * (h / 2) * 2 * bpp;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = ysz + csz;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer buf;
	vkCreateBuffer(m.device, &bci, nullptr, &buf);
	vkGetBufferMemoryRequirements(m.device, buf, &mr);
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bm;
	vkAllocateMemory(m.device, &ai, nullptr, &bm);
	vkBindBufferMemory(m.device, buf, bm, 0);
	uint8_t *ptr;
	vkMapMemory(m.device, bm, 0, VK_WHOLE_SIZE, 0, (void **)&ptr);
	for (size_t i = 0; i < size_t(w) * h; i++) {
		if (eight) ptr[i] = uint8_t(y[i] >> 8); else memcpy(ptr + 2 * i, &y[i], 2);
	}
	for (size_t i = 0; i < size_t(w / 2) * (h / 2) * 2; i++) {
		if (eight) ptr[ysz + i] = uint8_t(cbcr[i] >> 8); else memcpy(ptr + ysz + 2 * i, &cbcr[i], 2);
	}
	vkUnmapMemory(m.device, bm);
	m.begin();
	VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = t.img;
	b.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(m.cmd, &di);
	VkBufferImageCopy cp[2] = {};
	cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	cp[0].imageExtent = {w, h, 1};
	cp[1].bufferOffset = ysz;
	cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	cp[1].imageExtent = {w / 2, h / 2, 1};
	vkCmdCopyBufferToImage(m.cmd, buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cp);
	b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier2(m.cmd, &di);
	vkEndCommandBuffer(m.cmd);
	bool ok = m.submit_wait(err, "test upload");
	vkDestroyBuffer(m.device, buf, nullptr);
	vkFreeMemory(m.device, bm, nullptr);
	if (!ok || !m.finish_surface(t, err)) return m.destroy_surface(t), -1;
	t.used = true;
	return m.store(t);
}

bool Unpacker::readback_rgba(int index, std::vector<uint32_t> &px, std::string *err)
{
	Impl &m = *impl;
	std::lock_guard<std::mutex> lk(m.mu);
	if (index < 0 || size_t(index) >= m.surfs.size() || !m.surfs[index].used) return ufail(err, "readback: unknown surface");
	Surf &t = m.surfs[index];
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size_t(t.w) * t.h * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf;
	vkCreateBuffer(m.device, &bci, nullptr, &buf);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(m.device, buf, &mr);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = m.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bm;
	vkAllocateMemory(m.device, &ai, nullptr, &bm);
	vkBindBufferMemory(m.device, buf, bm, 0);
	m.begin();
	Impl::barrier(m.cmd, t.rimg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	              VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy cp = {};
	cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	cp.imageExtent = {t.w, t.h, 1};
	vkCmdCopyImageToBuffer(m.cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
	Impl::barrier(m.cmd, t.rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
	              VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
	vkEndCommandBuffer(m.cmd);
	bool ok = m.submit_wait(err, "readback");
	if (ok) {
		void *p;
		vkMapMemory(m.device, bm, 0, VK_WHOLE_SIZE, 0, &p);
		px.assign(static_cast<uint32_t *>(p), static_cast<uint32_t *>(p) + size_t(t.w) * t.h);
		vkUnmapMemory(m.device, bm);
	}
	vkDestroyBuffer(m.device, buf, nullptr);
	vkFreeMemory(m.device, bm, nullptr);
	return ok;
}
} // namespace slp
