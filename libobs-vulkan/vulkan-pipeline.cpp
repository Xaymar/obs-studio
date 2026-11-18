// Pipeline cache, fixed-function state setters, projection, upload arena, descriptors and device_draw.
#include "vulkan-subsystem.hpp"

#include <graphics/effect.h>
#include <graphics/vec4.h>
#include <util/base.h>
#include <util/dstr.h>

#include <algorithm>
#include <stdlib.h>

/* ---- dynamic rendering ---------------------------------------------------------------------------------------- */

// Batching rule: consecutive draws into the same colour and depth views share one vkCmdBeginRendering/
// vkCmdEndRendering. The instance ends when a draw targets another colour or depth view (or none) or needs a barrier
// (barriers are recorded outside rendering), when anything else records commands (vk_cmd: clear, copy, stage, upload,
// unmap, present) and when the frame is submitted (vk_flush). The render target and depth buffer are registered with
// the tracker when an instance begins, not per draw: draws within one instance are ordered by rasterization order.
void vk_end_rendering(gs_device *d)
{
	if (d->rendering_view) {
		vkCmdEndRendering(d->frames.frames[d->frames.index].cmd);
		d->rendering_view = VK_NULL_HANDLE;
		d->rendering_depth = VK_NULL_HANDLE;
	}
}

/* ---- upload arena and descriptor pools ------------------------------------------------------------------------ */

bool vk_arena_upload(gs_device *d, const void *data, VkDeviceSize size, VkDeviceSize align, vk_buffer &buf)
{
	vk_frame_cmd(d); // the arena belongs to the frame being recorded
	vk_frame &f = d->frames.frames[d->frames.index];
	align = std::max<VkDeviceSize>(align, 1);
	for (;; ++f.arena_chunk) {
		if (f.arena_chunk == f.arena.size()) {
			// Grow: a new chunk at least twice the last; the old ones stay and are reused next time.
			vk_arena_chunk c;
			c.size = std::max({(VkDeviceSize)1 << 20, f.arena.empty() ? 0 : f.arena.back().size * 2, size});
			void *mapped;
			if (!vk_create_host_buffer(d, c.size,
						   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
							   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
							   VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
						   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, c.buffer,
						   c.alloc, mapped)) {
				blog(LOG_ERROR, "[libobs-vulkan] upload arena: out of memory");
				return false;
			}
			c.mapped = (uint8_t *)mapped;
			f.arena.push_back(c);
		}
		vk_arena_chunk &c = f.arena[f.arena_chunk];
		const VkDeviceSize off = (c.used + align - 1) / align * align;
		if (off + size <= c.size) {
			// Host writes before the submission are visible to it without a barrier.
			memcpy(c.mapped + off, data, (size_t)size);
			vmaFlushAllocation(d->vma, c.alloc, off, size);
			c.used = off + size;
			buf.buffer = c.buffer;
			buf.offset = off;
			return true;
		}
	}
}

static VkDescriptorSet alloc_set(gs_device *d, VkDescriptorSetLayout layout)
{
	vk_frame &f = d->frames.frames[d->frames.index];
	for (;; ++f.desc_pool) {
		const bool fresh = f.desc_pool == f.desc_pools.size();
		if (fresh) {
			const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
							      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024}};
			VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pi.maxSets = 512;
			pi.poolSizeCount = 2;
			pi.pPoolSizes = sizes;
			VkDescriptorPool pool;
			if (vkCreateDescriptorPool(d->dev, &pi, nullptr, &pool) != VK_SUCCESS) {
				return VK_NULL_HANDLE;
			}
			f.desc_pools.push_back(pool);
		}
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = f.desc_pools[f.desc_pool];
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &layout;
		VkDescriptorSet set;
		VkResult r = vkAllocateDescriptorSets(d->dev, &ai, &set);
		if (r == VK_SUCCESS) {
			return set;
		}
		if (fresh || (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)) {
			return VK_NULL_HANDLE; // even an empty pool cannot hold it
		}
	}
}

/* ---- pipelines ---------------------------------------------------------------------------------------------- */

static VkShaderStageFlags stage_of(const gs_shader *s)
{
	return s->type == GS_SHADER_VERTEX ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
}

static VkDescriptorSetLayout set_layout(gs_device *d, gs_shader *s)
{
	if (s->set_layout) {
		return s->set_layout;
	}
	std::vector<VkDescriptorSetLayoutBinding> b;
	if (!s->uniforms.empty()) {
		b.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage_of(s), nullptr});
	}
	for (const gs_shader_param &p : s->params) {
		if (p.type == GS_SHADER_PARAM_TEXTURE) {
			b.push_back({p.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				     (uint32_t)std::max(p.array_count, 1), stage_of(s), nullptr});
		}
	}
	VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	ci.bindingCount = (uint32_t)b.size();
	ci.pBindings = b.data();
	vkCreateDescriptorSetLayout(d->dev, &ci, nullptr, &s->set_layout);
	return s->set_layout;
}

VkPipelineLayout vk_pipeline_cache::layout(gs_shader *vs, gs_shader *ps)
{
	for (const layout_entry &e : layouts) {
		if (e.vs == vs && e.ps == ps) {
			return e.layout;
		}
	}
	VkDescriptorSetLayout sets[2] = {set_layout(device, vs), set_layout(device, ps)};
	if (!sets[0] || !sets[1]) {
		return VK_NULL_HANDLE;
	}
	VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	ci.setLayoutCount = 2;
	ci.pSetLayouts = sets;
	VkPipelineLayout l = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(device->dev, &ci, nullptr, &l) == VK_SUCCESS) {
		layouts.push_back({vs, ps, l});
	}
	return l;
}

// Index into gs_vertex_buffer::attrs for an input semantic; -1 when unknown.
static int attr_index(const std::string &semantic)
{
	const char *m = semantic.c_str();
	if (!strcmp(m, "POSITION")) {
		return 0;
	}
	if (!strcmp(m, "NORMAL")) {
		return 1;
	}
	if (!strcmp(m, "TANGENT")) {
		return 2;
	}
	if (!strcmp(m, "COLOR") || !strcmp(m, "COLOR0")) {
		return 3;
	}
	if (!astrcmp_n(m, "TEXCOORD", 8)) {
		return 4 + (m[8] ? atoi(m + 8) : 0);
	}
	return -1;
}

static VkFormat input_format(int attr, uint8_t components)
{
	if (attr == 3) {
		return VK_FORMAT_R8G8B8A8_UNORM; // as D3D11
	}
	switch (components) {
	case 1:
		return VK_FORMAT_R32_SFLOAT;
	case 2:
		return VK_FORMAT_R32G32_SFLOAT;
	case 3:
		return VK_FORMAT_R32G32B32_SFLOAT;
	default:
		return VK_FORMAT_R32G32B32A32_SFLOAT; // points, normals, tangents read all four floats of a vec3, as D3D11
	}
}

static VkShaderModule create_module(gs_device *d, const gs_shader *s)
{
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = s->spirv.size() * sizeof(uint32_t);
	ci.pCode = s->spirv.data();
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(d->dev, &ci, nullptr, &m);
	return m;
}

static const VkBlendFactor blend_factors[] = {VK_BLEND_FACTOR_ZERO,
					      VK_BLEND_FACTOR_ONE,
					      VK_BLEND_FACTOR_SRC_COLOR,
					      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
					      VK_BLEND_FACTOR_SRC_ALPHA,
					      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
					      VK_BLEND_FACTOR_DST_COLOR,
					      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
					      VK_BLEND_FACTOR_DST_ALPHA,
					      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
					      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
static const VkBlendOp blend_ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
				      VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
static const VkCompareOp compare_ops[] = {VK_COMPARE_OP_NEVER,
					  VK_COMPARE_OP_LESS,
					  VK_COMPARE_OP_LESS_OR_EQUAL,
					  VK_COMPARE_OP_EQUAL,
					  VK_COMPARE_OP_GREATER_OR_EQUAL,
					  VK_COMPARE_OP_GREATER,
					  VK_COMPARE_OP_NOT_EQUAL,
					  VK_COMPARE_OP_ALWAYS};
// As D3D11: INCR/DECR wrap.
static const VkStencilOp stencil_ops[] = {VK_STENCIL_OP_KEEP,
					  VK_STENCIL_OP_ZERO,
					  VK_STENCIL_OP_REPLACE,
					  VK_STENCIL_OP_INCREMENT_AND_WRAP,
					  VK_STENCIL_OP_DECREMENT_AND_WRAP,
					  VK_STENCIL_OP_INVERT};

static bool has_stencil(VkFormat f)
{
	return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkStencilOpState stencil_side(const uint8_t s[4], bool write)
{
	VkStencilOpState o = {};
	o.compareOp = compare_ops[s[0]];
	o.failOp = stencil_ops[s[1]];
	o.depthFailOp = stencil_ops[s[2]];
	o.passOp = stencil_ops[s[3]];
	o.compareMask = 0xFF;
	o.writeMask = write ? 0xFF : 0;
	return o;
}

// Logs and returns false when the pass needs more than the device has.
static bool check_limits(gs_device *d, const vk_pipeline_key &k)
{
	const VkPhysicalDeviceLimits &l = d->limits;
	const uint32_t vs_out = (uint32_t)(k.vs->max_varying + 1) * 4, ps_in = (uint32_t)(k.ps->max_varying + 1) * 4;
	if (vs_out > l.maxVertexOutputComponents || ps_in > l.maxFragmentInputComponents) {
		blog(LOG_ERROR,
		     "[libobs-vulkan] pipeline: varyings need %u vertex output / %u fragment input components, the device "
		     "has %u / %u (COLOR, COLOR1, NORMAL and TANGENT use locations 16-19)",
		     vs_out, ps_in, l.maxVertexOutputComponents, l.maxFragmentInputComponents);
		return false;
	}
	const size_t inputs = k.vs->inputs.size();
	if (inputs > vk_max_inputs || inputs > l.maxVertexInputAttributes || inputs > l.maxVertexInputBindings) {
		blog(LOG_ERROR, "[libobs-vulkan] pipeline: %zu vertex inputs, at most %u supported", inputs,
		     std::min(vk_max_inputs, l.maxVertexInputAttributes));
		return false;
	}
	return true;
}

static VkPipeline create_pipeline(gs_device *d, const vk_pipeline_key &k, VkPipelineLayout layout)
{
	if (!layout || !check_limits(d, k)) {
		return VK_NULL_HANDLE;
	}
	VkShaderModule vsm = create_module(d, k.vs), psm = create_module(d, k.ps);
	VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
						     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vsm;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = psm;
	stages[1].pName = "main";

	// One binding per input location, fed from the vertex buffer array its semantic names.
	std::vector<VkVertexInputBindingDescription> bindings;
	std::vector<VkVertexInputAttributeDescription> attrs;
	for (uint32_t i = 0; i < (uint32_t)k.vs->inputs.size(); ++i) {
		const int a = attr_index(k.vs->inputs[i]);
		const uint32_t stride = a == 3 ? 4 : a < 3 ? 16 : k.components[i] * 4;
		bindings.push_back({i, stride, VK_VERTEX_INPUT_RATE_VERTEX});
		attrs.push_back({i, i, input_format(a, k.components[i]), 0});
	}
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = (uint32_t)bindings.size();
	vi.pVertexBindingDescriptions = bindings.data();
	vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
	vi.pVertexAttributeDescriptions = attrs.data();

	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = (VkPrimitiveTopology)k.topology;

	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	// Front faces are counter-clockwise on screen, as D3D11 (FrontCounterClockwise). With the negative-height
	// viewport, framebuffer coordinates are D3D's (y down), so the same winding is front-facing in both.
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = k.state.cull == GS_BACK    ? VK_CULL_MODE_BACK_BIT
		      : k.state.cull == GS_FRONT ? VK_CULL_MODE_FRONT_BIT
						 : VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Ignored while depth is VK_FORMAT_UNDEFINED; stencil is ignored for depth-only formats.
	VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = k.state.depth;
	ds.depthWriteEnable = k.state.depth_write;
	ds.depthCompareOp = compare_ops[k.state.depth_func];
	ds.stencilTestEnable = k.state.stencil;
	ds.front = stencil_side(k.state.front, k.state.stencil_write);
	ds.back = stencil_side(k.state.back, k.state.stencil_write);
	ds.maxDepthBounds = 1.0f;

	// As D3D11: one op for colour and alpha.
	VkPipelineColorBlendAttachmentState att = {};
	att.blendEnable = k.state.blend;
	att.srcColorBlendFactor = blend_factors[k.state.src_c];
	att.dstColorBlendFactor = blend_factors[k.state.dst_c];
	att.srcAlphaBlendFactor = blend_factors[k.state.src_a];
	att.dstAlphaBlendFactor = blend_factors[k.state.dst_a];
	att.colorBlendOp = att.alphaBlendOp = blend_ops[k.state.op];
	att.colorWriteMask = k.state.write_mask;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &att;

	const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
				      VK_DYNAMIC_STATE_STENCIL_REFERENCE};
	VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 3;
	dy.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &k.color;
	ri.depthAttachmentFormat = k.depth;
	ri.stencilAttachmentFormat = has_stencil(k.depth) ? k.depth : VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &dy;
	pi.layout = layout;

	VkPipeline p = VK_NULL_HANDLE;
	VkResult r = vsm && psm ? vkCreateGraphicsPipelines(d->dev, d->pipelines.cache, 1, &pi, nullptr, &p)
				: VK_ERROR_INITIALIZATION_FAILED;
	if (r != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] vkCreateGraphicsPipelines failed (%d)", (int)r);
		p = VK_NULL_HANDLE;
	}
	vkDestroyShaderModule(d->dev, vsm, nullptr); // a pipeline keeps no reference to its modules
	vkDestroyShaderModule(d->dev, psm, nullptr);
	return p;
}

VkPipeline vk_pipeline_cache::get(const vk_pipeline_key &key, VkPipelineLayout *out_layout)
{
	auto it = pipelines.find(key);
	if (it == pipelines.end()) {
		if (!cache) {
			VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
			vkCreatePipelineCache(device->dev, &ci, nullptr, &cache);
		}
		VkPipelineLayout l = layout(key.vs, key.ps);
		// A failure is stored too, so the same key is never compiled again.
		it = pipelines.emplace(key, entry{create_pipeline(device, key, l), l}).first;
	}
	if (out_layout) {
		*out_layout = it->second.layout;
	}
	return it->second.pipeline;
}

void vk_pipeline_cache::forget(gs_shader *s)
{
	if (!device) {
		return;
	}
	VkDevice dev = device->dev;
	for (auto it = pipelines.begin(); it != pipelines.end();) {
		if (it->first.vs == s || it->first.ps == s) {
			device->frames.defer_destroy(
				[dev, p = it->second.pipeline] { vkDestroyPipeline(dev, p, nullptr); });
			it = pipelines.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = layouts.begin(); it != layouts.end();) {
		if (it->vs == s || it->ps == s) {
			device->frames.defer_destroy(
				[dev, l = it->layout] { vkDestroyPipelineLayout(dev, l, nullptr); });
			it = layouts.erase(it);
		} else {
			++it;
		}
	}
	device->frames.defer_destroy([dev, l = s->set_layout, samplers = s->vk_samplers] {
		vkDestroyDescriptorSetLayout(dev, l, nullptr);
		for (VkSampler smp : samplers) {
			vkDestroySampler(dev, smp, nullptr);
		}
	});
	s->set_layout = VK_NULL_HANDLE;
	s->vk_samplers.clear();
	if (device->cur_vs == s) {
		device->cur_vs = nullptr;
	}
	if (device->cur_ps == s) {
		device->cur_ps = nullptr;
	}
}

void vk_pipelines_free(gs_device *d)
{
	vk_pipeline_cache &c = d->pipelines;
	for (auto &p : c.pipelines) {
		vkDestroyPipeline(d->dev, p.second.pipeline, nullptr);
	}
	for (auto &l : c.layouts) {
		vkDestroyPipelineLayout(d->dev, l.layout, nullptr);
	}
	c.pipelines.clear();
	c.layouts.clear();
	if (c.cache) {
		vkDestroyPipelineCache(d->dev, c.cache, nullptr);
		c.cache = VK_NULL_HANDLE;
	}
	if (d->default_sampler) {
		vkDestroySampler(d->dev, d->default_sampler, nullptr);
		d->default_sampler = VK_NULL_HANDLE;
	}
}

/* ---- draw ------------------------------------------------------------------------------------------------------- */

static VkSampler shader_sampler(gs_device *d, gs_shader *s, size_t index)
{
	if (index >= s->samplers.size()) {
		if (!d->default_sampler) {
			gs_sampler_info info = {};
			info.filter = GS_FILTER_POINT;
			info.max_anisotropy = 1;
			d->default_sampler = vk_create_sampler(d, info);
		}
		return d->default_sampler;
	}
	if (s->vk_samplers.size() != s->samplers.size()) {
		s->vk_samplers.assign(s->samplers.size(), VK_NULL_HANDLE);
	}
	if (!s->vk_samplers[index]) {
		s->vk_samplers[index] = vk_create_sampler(d, s->samplers[index]);
	}
	return s->vk_samplers[index];
}

// Uploads the shader's uniforms (when dirty or not yet in this frame's arena) and writes its descriptor set.
// Sampled textures are registered with the tracker into `b`. False (logged) when the draw cannot go ahead.
static bool write_set(gs_device *d, gs_shader *s, VkDescriptorSet &set, vk_barriers &b)
{
	set = VK_NULL_HANDLE;
	const bool ps = s->type == GS_SHADER_PIXEL;
	std::vector<VkDescriptorImageInfo> images;
	std::vector<VkWriteDescriptorSet> writes;
	size_t unit = 0;
	for (gs_shader_param &p : s->params) {
		if (p.type != GS_SHADER_PARAM_TEXTURE) {
			continue;
		}
		gs_texture *t = p.texture ? p.texture : ps && unit < GS_MAX_TEXTURES ? d->cur_textures[unit] : nullptr;
		gs_sampler_state *ss = p.next_sampler                 ? p.next_sampler
				       : ps && unit < GS_MAX_TEXTURES ? d->cur_samplers[unit]
								      : nullptr;
		++unit;
		// shortcut: an unset texture fails the draw (D3D11 samples zero); needs a dummy texture to match.
		if (!t || !t->image) {
			if (!p.warned) {
				blog(LOG_ERROR, "[libobs-vulkan] device_draw: texture '%s' is not set; draw skipped",
				     p.name.c_str());
				p.warned = true;
			}
			return false;
		}
		// Sampling the render target would need SHADER_READ and COLOR_ATTACHMENT layouts at once.
		if (d->render_target && t->image == d->render_target->image) {
			if (!p.feedback_warned) {
				blog(LOG_ERROR,
				     "[libobs-vulkan] device_draw: texture '%s' is the current render target; draw skipped",
				     p.name.c_str());
				p.feedback_warned = true;
			}
			return false;
		}
		const vk_access sampled = {
			ps ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		d->tracker.use_image(t->image, d->graphics.family, sampled, b);
		const VkDescriptorImageInfo info = {ss ? ss->sampler : shader_sampler(d, s, p.sampler),
						    p.srgb && t->srgb_view ? t->srgb_view : t->view,
						    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		// shortcut: every element of a texture array gets the same texture (no core effect uses arrays; Task 8).
		for (int i = 0; i < std::max(p.array_count, 1); ++i) {
			images.push_back(info);
		}
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstBinding = p.binding;
		w.descriptorCount = (uint32_t)std::max(p.array_count, 1);
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes.push_back(w);
	}

	VkDescriptorBufferInfo ubo = {};
	if (!s->uniforms.empty()) {
		if (s->dirty || s->ubo_value != d->graphics.next_value) {
			vk_buffer arena;
			if (!vk_arena_upload(d, s->uniforms.data(), s->uniforms.size(),
					     d->limits.minUniformBufferOffsetAlignment, arena)) {
				return false;
			}
			s->ubo = arena.buffer;
			s->ubo_offset = arena.offset;
			s->ubo_value = d->graphics.next_value;
			s->dirty = false;
		}
		ubo = {s->ubo, s->ubo_offset, s->uniforms.size()};
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &ubo;
		writes.push_back(w);
	}
	if (writes.empty()) {
		return true;
	}

	set = alloc_set(d, s->set_layout);
	if (!set) {
		blog(LOG_ERROR, "[libobs-vulkan] device_draw: descriptor set allocation failed");
		return false;
	}
	size_t img = 0;
	for (VkWriteDescriptorSet &w : writes) {
		w.dstSet = set;
		if (w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			w.pImageInfo = &images[img];
			img += w.descriptorCount;
		}
	}
	vkUpdateDescriptorSets(d->dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
	return true;
}

// As D3D11's UpdateViewProjMatrix: libobs's view matrix (Z negated: right-handed), times the projection, transposed.
static void update_viewproj(gs_device *d, gs_shader *vs)
{
	if (!vs->viewproj) {
		return;
	}
	struct matrix4 view, vp;
	matrix4_identity(&view);
	if (gs_get_context()) {
		gs_matrix_get(&view);
	}
	view.x.z = -view.x.z;
	view.y.z = -view.y.z;
	view.z.z = -view.z.z;
	view.t.z = -view.t.z;
	matrix4_mul(&vp, &view, &d->proj);
	matrix4_transpose(&vp, &vp);
	gs_shader_set_matrix4(vs->viewproj, &vp);
}

static VkPrimitiveTopology topology(enum gs_draw_mode mode)
{
	switch (mode) {
	case GS_POINTS:
		return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case GS_LINES:
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case GS_LINESTRIP:
		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case GS_TRIS:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	default:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	}
}

// Never waits on the GPU: everything it needs comes from the frame's arena and descriptor pools.
static void draw(gs_device *d, enum gs_draw_mode mode, uint32_t start, uint32_t count)
{
	gs_shader *vs = d->cur_vs, *ps = d->cur_ps;
	gs_texture *rt = d->render_target;
	gs_zstencil_buffer *zs = d->zstencil;
	gs_vertex_buffer *vb = d->cur_vb;
	gs_index_buffer *ib = d->cur_ib;
	const char *error = !vs                          ? "no vertex shader"
			    : !ps                        ? "no pixel shader"
			    : !vb && !count              ? "no vertex buffer"
			    : !rt                        ? "no render target"
			    : !vs->inputs.empty() && !vb ? "the vertex shader needs a vertex buffer"
							 : nullptr;
	if (error) {
		blog(LOG_ERROR, "[libobs-vulkan] device_draw: %s", error);
		return;
	}
	if (!rt->image) {
		return; // swapchain image not acquired: nothing to draw into, as a lost D3D11 present
	}
	if (!rt->rt_view) {
		blog(LOG_ERROR, "[libobs-vulkan] device_draw: texture is not a render target");
		return;
	}
	if (zs && (zs->width < rt->width || zs->height < rt->height)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_draw: depth buffer %ux%u is smaller than the %ux%u target",
		     zs->width, zs->height, rt->width, rt->height);
		return;
	}
	if (d->viewport.cx <= 0 || d->viewport.cy <= 0) {
		return; // as D3D11, an empty viewport draws nothing
	}
	if (!vs->device) {
		vs->device = d; // created without a device (tests); forget() needs it
	}
	if (!ps->device) {
		ps->device = d;
	}

	if (gs_get_context()) {
		gs_effect_t *effect = gs_get_effect();
		if (effect) {
			gs_effect_update_params(effect);
		}
	}
	update_viewproj(d, vs);

	const bool srgb = d->framebuffer_srgb && rt->rt_srgb_view;
	const VkImageView view = srgb ? rt->rt_srgb_view : rt->rt_view;
	vk_pipeline_key key;
	key.vs = vs;
	key.ps = ps;
	key.color = srgb ? rt->rt_srgb_format : rt->rt_format;
	key.depth = zs ? zs->format : VK_FORMAT_UNDEFINED;
	key.state = d->state;
	// State that has no effect stays at its defaults, so it never makes another pipeline.
	const vk_state def;
	if (!zs) {
		key.state.depth = def.depth;
		key.state.depth_write = def.depth_write;
		key.state.depth_func = def.depth_func;
	}
	if (!has_stencil(key.depth)) {
		key.state.stencil = def.stencil;
		key.state.stencil_write = def.stencil_write;
		memcpy(key.state.front, def.front, sizeof(def.front));
		memcpy(key.state.back, def.back, sizeof(def.back));
	}
	if (!key.state.blend) {
		key.state.src_c = def.src_c;
		key.state.dst_c = def.dst_c;
		key.state.src_a = def.src_a;
		key.state.dst_a = def.dst_a;
		key.state.op = def.op;
	}
	key.topology = (uint8_t)topology(mode);
	std::vector<const vk_buffer *> inputs;
	for (size_t i = 0; i < vs->inputs.size() && i < vk_max_inputs; ++i) {
		const int a = attr_index(vs->inputs[i]);
		if (a < 0 || (size_t)a >= vb->attrs.size() || (!vb->attrs[a].buffer && !vb->dynamic) ||
		    (a == 0   ? !vb->data->points
		     : a == 1 ? !vb->data->normals
		     : a == 2 ? !vb->data->tangents
		     : a == 3 ? !vb->data->colors
			      : false)) {
			blog(LOG_ERROR, "[libobs-vulkan] device_draw: the vertex buffer has no %s data",
			     vs->inputs[i].c_str());
			return;
		}
		key.components[i] = a >= 4 ? (uint8_t)vb->data->tvarray[a - 4].width : 4;
		inputs.push_back(&vb->attrs[a]);
	}
	VkPipelineLayout layout;
	VkPipeline pipeline = d->pipelines.get(key, &layout);
	if (!pipeline) {
		return; // creation failed and was logged once
	}

	if ((vb && !vk_upload_dynamic(d, vb)) || (ib && !vk_upload_dynamic(d, ib))) {
		return;
	}
	VkDescriptorSet sets[2] = {};
	vk_barriers b;
	if (!write_set(d, vs, sets[vs->set], b) || !write_set(d, ps, sets[ps->set], b)) {
		// The tracker already counts the textures registered so far as transitioned: record that.
		if (!b.images.empty()) {
			vk_end_rendering(d);
			vk_record_barriers(vk_frame_cmd(d), b);
		}
		return;
	}

	VkCommandBuffer cmd = vk_frame_cmd(d);
	const VkImageView zview = zs ? zs->view : VK_NULL_HANDLE;
	if (!b.images.empty() || d->rendering_view != view || d->rendering_depth != zview) {
		vk_end_rendering(d);
		d->tracker.use_image(rt->image, d->graphics.family,
				     {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
				     b);
		if (zs) {
			d->tracker.use_image(zs->image, d->graphics.family,
					     {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
						      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
						      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, zs->aspect},
					     b);
		}
		vk_record_barriers(cmd, b);

		VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		ca.imageView = view;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // device_clear is a transfer clear before the instance
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {rt->width, rt->height}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		if (zs) {
			da.imageView = zview;
			da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			ri.pDepthAttachment = &da;
			ri.pStencilAttachment = has_stencil(zs->format) ? &da : nullptr;
		}
		vkCmdBeginRendering(cmd, &ri);
		d->rendering_view = view;
		d->rendering_depth = zview;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	// Negative height (Vulkan 1.1 core): clip-space +Y is up as in D3D, so D3D's projection matrices apply as-is.
	const gs_rect &v = d->viewport;
	VkViewport vp = {(float)v.x, (float)(v.y + v.cy), (float)v.cx, -(float)v.cy, 0.0f, 1.0f};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	VkRect2D sc = {{0, 0}, {rt->width, rt->height}};
	if (d->scissor_enabled) {
		const int x0 = std::clamp(d->scissor.x, 0, (int)rt->width);
		const int y0 = std::clamp(d->scissor.y, 0, (int)rt->height);
		const int x1 = std::clamp(d->scissor.x + d->scissor.cx, x0, (int)rt->width);
		const int y1 = std::clamp(d->scissor.y + d->scissor.cy, y0, (int)rt->height);
		sc = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
	}
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0); // as D3D11's OMSetDepthStencilState(state, 0)
	for (uint32_t i = 0; i < (uint32_t)inputs.size(); ++i) {
		vkCmdBindVertexBuffers(cmd, i, 1, &inputs[i]->buffer, &inputs[i]->offset);
	}
	for (uint32_t i = 0; i < 2; ++i) {
		if (sets[i]) {
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, i, 1, &sets[i], 0,
						nullptr);
		}
	}
	if (ib) {
		vkCmdBindIndexBuffer(cmd, ib->buf.buffer, ib->buf.offset,
				     ib->type == GS_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, count ? count : (uint32_t)ib->num, 1, start, 0, 0);
	} else {
		vkCmdDraw(cmd, count ? count : (uint32_t)vb->data->num, 1, start, 0);
	}

	// As D3D11: a next sampler applies to one draw.
	for (gs_shader *s : {vs, ps}) {
		for (gs_shader_param &p : s->params) {
			p.next_sampler = nullptr;
		}
	}
}

/* ---- exports ------------------------------------------------------------------------------------------------- */

extern "C" {

EXPORT void device_draw(gs_device_t *device, enum gs_draw_mode draw_mode, uint32_t start_vert, uint32_t num_verts)
{
	draw(device, draw_mode, start_vert, num_verts);
}

EXPORT void device_begin_scene(gs_device_t *device)
{
	// As D3D11: textures loaded by unit do not carry over between scenes.
	for (gs_texture *&t : device->cur_textures) {
		t = nullptr;
	}
}

EXPORT void device_load_vertexshader(gs_device_t *device, gs_shader_t *vertshader)
{
	if (vertshader && vertshader->type != GS_SHADER_VERTEX) {
		blog(LOG_ERROR, "[libobs-vulkan] device_load_vertexshader: not a vertex shader");
		return;
	}
	device->cur_vs = vertshader;
}

EXPORT void device_load_pixelshader(gs_device_t *device, gs_shader_t *pixelshader)
{
	if (pixelshader && pixelshader->type != GS_SHADER_PIXEL) {
		blog(LOG_ERROR, "[libobs-vulkan] device_load_pixelshader: not a pixel shader");
		return;
	}
	device->cur_ps = pixelshader;
	// As D3D11: loading a pixel shader binds its own sampler states, replacing device_load_samplerstate's.
	for (gs_sampler_state *&s : device->cur_samplers) {
		s = nullptr;
	}
}

EXPORT gs_shader_t *device_get_vertex_shader(const gs_device_t *device)
{
	return device->cur_vs;
}

EXPORT gs_shader_t *device_get_pixel_shader(const gs_device_t *device)
{
	return device->cur_ps;
}

EXPORT void device_enable_framebuffer_srgb(gs_device_t *device, bool enable)
{
	device->framebuffer_srgb = enable;
}

EXPORT bool device_framebuffer_srgb_enabled(gs_device_t *device)
{
	return device->framebuffer_srgb;
}

EXPORT void device_set_cull_mode(gs_device_t *device, enum gs_cull_mode mode)
{
	device->state.cull = (uint8_t)mode;
}

EXPORT enum gs_cull_mode device_get_cull_mode(const gs_device_t *device)
{
	return (enum gs_cull_mode)device->state.cull;
}

EXPORT void device_enable_blending(gs_device_t *device, bool enable)
{
	device->state.blend = enable;
}

EXPORT void device_enable_depth_test(gs_device_t *device, bool enable)
{
	device->state.depth = enable;
}

EXPORT void device_enable_stencil_test(gs_device_t *device, bool enable)
{
	device->state.stencil = enable;
}

EXPORT void device_enable_stencil_write(gs_device_t *device, bool enable)
{
	device->state.stencil_write = enable;
}

EXPORT void device_enable_color(gs_device_t *device, bool red, bool green, bool blue, bool alpha)
{
	device->state.write_mask = (red ? VK_COLOR_COMPONENT_R_BIT : 0) | (green ? VK_COLOR_COMPONENT_G_BIT : 0) |
				   (blue ? VK_COLOR_COMPONENT_B_BIT : 0) | (alpha ? VK_COLOR_COMPONENT_A_BIT : 0);
}

EXPORT void device_blend_function(gs_device_t *device, enum gs_blend_type src, enum gs_blend_type dest)
{
	device->state.src_c = device->state.src_a = (uint8_t)src;
	device->state.dst_c = device->state.dst_a = (uint8_t)dest;
}

EXPORT void device_blend_function_separate(gs_device_t *device, enum gs_blend_type src_c, enum gs_blend_type dest_c,
					   enum gs_blend_type src_a, enum gs_blend_type dest_a)
{
	device->state.src_c = (uint8_t)src_c;
	device->state.dst_c = (uint8_t)dest_c;
	device->state.src_a = (uint8_t)src_a;
	device->state.dst_a = (uint8_t)dest_a;
}

EXPORT void device_blend_op(gs_device_t *device, enum gs_blend_op_type op)
{
	device->state.op = (uint8_t)op;
}

EXPORT void device_depth_function(gs_device_t *device, enum gs_depth_test test)
{
	device->state.depth_func = (uint8_t)test;
}

EXPORT void device_stencil_function(gs_device_t *device, enum gs_stencil_side side, enum gs_depth_test test)
{
	if (side & GS_STENCIL_FRONT) {
		device->state.front[0] = (uint8_t)test;
	}
	if (side & GS_STENCIL_BACK) {
		device->state.back[0] = (uint8_t)test;
	}
}

EXPORT void device_stencil_op(gs_device_t *device, enum gs_stencil_side side, enum gs_stencil_op_type fail,
			      enum gs_stencil_op_type zfail, enum gs_stencil_op_type zpass)
{
	for (uint8_t *s : {(side & GS_STENCIL_FRONT) ? device->state.front : nullptr,
			   (side & GS_STENCIL_BACK) ? device->state.back : nullptr}) {
		if (s) {
			s[1] = (uint8_t)fail;
			s[2] = (uint8_t)zfail;
			s[3] = (uint8_t)zpass;
		}
	}
}

EXPORT void device_set_viewport(gs_device_t *device, int x, int y, int width, int height)
{
	device->viewport = {x, y, width, height};
}

EXPORT void device_get_viewport(const gs_device_t *device, struct gs_rect *rect)
{
	*rect = device->viewport;
}

EXPORT void device_set_scissor_rect(gs_device_t *device, const struct gs_rect *rect)
{
	device->scissor_enabled = rect != nullptr;
	if (rect) {
		device->scissor = *rect;
	}
}

// As D3D11: D3D clip space, depth [0, 1]; the viewport's negative height supplies Vulkan's Y flip.
EXPORT void device_ortho(gs_device_t *device, float left, float right, float top, float bottom, float zNear, float zFar)
{
	matrix4 *dst = &device->proj;
	const float rml = right - left, bmt = bottom - top, fmn = zFar - zNear;
	vec4_zero(&dst->x);
	vec4_zero(&dst->y);
	vec4_zero(&dst->z);
	vec4_zero(&dst->t);
	dst->x.x = 2.0f / rml;
	dst->t.x = (left + right) / -rml;
	dst->y.y = 2.0f / -bmt;
	dst->t.y = (bottom + top) / bmt;
	dst->z.z = 1.0f / fmn;
	dst->t.z = zNear / -fmn;
	dst->t.w = 1.0f;
}

// As D3D11 (gs_perspective is built on this in libobs; there is no device_perspective export).
EXPORT void device_frustum(gs_device_t *device, float left, float right, float top, float bottom, float zNear,
			   float zFar)
{
	matrix4 *dst = &device->proj;
	const float rml = right - left, bmt = bottom - top, fmn = zFar - zNear, nearx2 = 2.0f * zNear;
	vec4_zero(&dst->x);
	vec4_zero(&dst->y);
	vec4_zero(&dst->z);
	vec4_zero(&dst->t);
	dst->x.x = nearx2 / rml;
	dst->z.x = (left + right) / -rml;
	dst->y.y = nearx2 / -bmt;
	dst->z.y = (bottom + top) / bmt;
	dst->z.z = zFar / fmn;
	dst->t.z = (zNear * zFar) / -fmn;
	dst->z.w = 1.0f;
}

EXPORT void device_projection_push(gs_device_t *device)
{
	device->proj_stack.push_back(device->proj);
}

EXPORT void device_projection_pop(gs_device_t *device)
{
	if (!device->proj_stack.empty()) {
		device->proj = device->proj_stack.back();
		device->proj_stack.pop_back();
	}
}
}
