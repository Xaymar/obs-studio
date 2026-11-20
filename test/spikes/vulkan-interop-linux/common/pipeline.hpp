// Fullscreen-triangle pattern pipeline for dynamic rendering into one colour attachment format.
#pragma once
#include "vkutil.hpp"

struct PatternPipeline {
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
};

inline VkShaderModule make_module(Ctx &c, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m;
	VK_CHECK(vkCreateShaderModule(c.dev, &ci, nullptr, &m));
	return m;
}

inline PatternPipeline make_pattern_pipeline(Ctx &c, VkFormat fmt, const uint32_t *vs, size_t vs_bytes,
					     const uint32_t *fs, size_t fs_bytes)
{
	PatternPipeline p;
	VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
	VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	lci.pushConstantRangeCount = 1;
	lci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout(c.dev, &lci, nullptr, &p.layout));
	VkShaderModule v = make_module(c, vs, vs_bytes), f = make_module(c, fs, fs_bytes);
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
						 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = v;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = f;
	st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &fmt;
	VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gci.pNext = &ri;
	gci.stageCount = 2;
	gci.pStages = st;
	gci.pVertexInputState = &vi;
	gci.pInputAssemblyState = &ia;
	gci.pViewportState = &vp;
	gci.pRasterizationState = &rs;
	gci.pMultisampleState = &ms;
	gci.pColorBlendState = &cb;
	gci.pDynamicState = &ds;
	gci.layout = p.layout;
	VK_CHECK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &gci, nullptr, &p.pipe));
	vkDestroyShaderModule(c.dev, v, nullptr);
	vkDestroyShaderModule(c.dev, f, nullptr);
	return p;
}

inline void destroy_pipeline(Ctx &c, PatternPipeline &p)
{
	vkDestroyPipeline(c.dev, p.pipe, nullptr);
	vkDestroyPipelineLayout(c.dev, p.layout, nullptr);
}

// Clears the view to `clear`, then draws the pattern inside `rect` (the rest keeps the clear colour).
inline void clear_and_draw(VkCommandBuffer cb, PatternPipeline &p, VkImageView view, uint32_t w, uint32_t h,
			   VkClearColorValue clear, VkRect2D rect, int mode, int clear_mode = 0)
{
	VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	att.imageView = view;
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	// clear_mode 0: loadOp CLEAR, 1: loadOp DONT_CARE + vkCmdClearAttachments inside the rendering,
	// 2: loadOp CLEAR and no draw
	att.loadOp = clear_mode == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.clearValue.color = clear;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {w, h}};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cb, &ri);
	if (clear_mode == 1) {
		VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
		ca.clearValue.color = clear;
		VkClearRect cr{{{0, 0}, {w, h}}, 0, 1};
		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
	}
	VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &rect);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);
	vkCmdPushConstants(cb, p.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &mode);
	if (clear_mode != 2)
		vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRendering(cb);
}

// Pattern reference values, matching common/pattern.frag
inline uint32_t pat_a(int x, int y) { return (uint32_t)(x * 7 + y * 13); }
inline uint32_t pat_b(int x, int y) { return (uint32_t)(x * 3 + y * 5 + 100); }
inline bool in_rect(VkRect2D r, int x, int y)
{
	return x >= r.offset.x && y >= r.offset.y && x < r.offset.x + (int)r.extent.width &&
	       y < r.offset.y + (int)r.extent.height;
}
