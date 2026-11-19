// Tiny "draw a solid rectangle" pipeline for dynamic rendering. Throwaway test code.
#pragma once
#include "spike_common.hpp"
#include "shaders/rect_vert.h"
#include "shaders/solid_frag.h"

namespace spike {

struct rect_pc {
	float rect[4]; // NDC x0 y0 x1 y1
	float color[4];
};

struct rect_pipeline {
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;

	void create(device &d, VkFormat color_format)
	{
		VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rect_pc)};
		VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pcr;
		VKC(d.t.vkCreatePipelineLayout(d.dev, &li, nullptr, &layout));

		VkShaderModule vs, fs;
		VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = sizeof(rect_vert_spv);
		mi.pCode = rect_vert_spv;
		VKC(d.t.vkCreateShaderModule(d.dev, &mi, nullptr, &vs));
		mi.codeSize = sizeof(solid_frag_spv);
		mi.pCode = solid_frag_spv;
		VKC(d.t.vkCreateShaderModule(d.dev, &mi, nullptr, &fs));

		VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
							 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
		st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		st[0].module = vs;
		st[0].pName = "main";
		st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		st[1].module = fs;
		st[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
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
		ri.pColorAttachmentFormats = &color_format;

		VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pi.pNext = &ri;
		pi.stageCount = 2;
		pi.pStages = st;
		pi.pVertexInputState = &vi;
		pi.pInputAssemblyState = &ia;
		pi.pViewportState = &vp;
		pi.pRasterizationState = &rs;
		pi.pMultisampleState = &ms;
		pi.pColorBlendState = &cb;
		pi.pDynamicState = &ds;
		pi.layout = layout;
		VKC(d.t.vkCreateGraphicsPipelines(d.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe));
		d.t.vkDestroyShaderModule(d.dev, vs, nullptr);
		d.t.vkDestroyShaderModule(d.dev, fs, nullptr);
	}

	// Draw a pixel-aligned rect [x0,x1) x [y0,y1) on a w x h target.
	void draw(device &d, VkCommandBuffer cb, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1,
		  const float color[4])
	{
		rect_pc pc;
		pc.rect[0] = 2.0f * x0 / w - 1.0f;
		pc.rect[1] = 2.0f * y0 / h - 1.0f;
		pc.rect[2] = 2.0f * x1 / w - 1.0f;
		pc.rect[3] = 2.0f * y1 / h - 1.0f;
		memcpy(pc.color, color, sizeof pc.color);
		d.t.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		VkViewport v{0, 0, (float)w, (float)h, 0, 1};
		VkRect2D sc{{0, 0}, {w, h}};
		d.t.vkCmdSetViewport(cb, 0, 1, &v);
		d.t.vkCmdSetScissor(cb, 0, 1, &sc);
		d.t.vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
				       sizeof pc, &pc);
		d.t.vkCmdDraw(cb, 4, 1, 0, 0);
	}

	void destroy(device &d)
	{
		if (pipe)
			d.t.vkDestroyPipeline(d.dev, pipe, nullptr);
		if (layout)
			d.t.vkDestroyPipelineLayout(d.dev, layout, nullptr);
		pipe = VK_NULL_HANDLE;
		layout = VK_NULL_HANDLE;
	}
};

} // namespace spike
