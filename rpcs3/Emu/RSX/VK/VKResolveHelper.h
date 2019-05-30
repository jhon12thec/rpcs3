﻿#pragma once

#include "VKHelpers.h"
#include "VKCompute.h"
#include "VKOverlays.h"

namespace vk
{
	struct cs_resolve_base : compute_task
	{
		vk::viewable_image* multisampled;
		vk::viewable_image* resolve;

		u32 cs_wave_x = 1;
		u32 cs_wave_y = 1;

		cs_resolve_base()
		{}

		virtual ~cs_resolve_base()
		{}

		void build(const std::string& kernel, const std::string& format_prefix, int direction)
		{
			create();

			// TODO: Tweak occupancy
			switch (optimal_group_size)
			{
			default:
			case 64:
				cs_wave_x = 8;
				cs_wave_y = 8;
				break;
			case 32:
				cs_wave_x = 8;
				cs_wave_y = 4;
				break;
			}

			const std::pair<std::string, std::string> syntax_replace[] =
			{
				{ "%wx", std::to_string(cs_wave_x) },
				{ "%wy", std::to_string(cs_wave_y) },
			};

			m_src =
			"#version 430\n"
			"layout(local_size_x=%wx, local_size_y=%wy, local_size_z=1) in;\n"
			"\n";

			m_src = fmt::replace_all(m_src, syntax_replace);

			if (direction == 0)
			{
				m_src +=
				"layout(set=0, binding=0, " + format_prefix + ") uniform readonly restrict image2DMS multisampled;\n"
				"layout(set=0, binding=1) uniform writeonly restrict image2D resolve;\n";
			}
			else
			{
				m_src +=
				"layout(set=0, binding=0) uniform writeonly restrict image2DMS multisampled;\n"
				"layout(set=0, binding=1, " + format_prefix + ") uniform readonly restrict image2D resolve;\n";
			}

			m_src +=
			"\n"
			"void main()\n"
			"{\n"
			"	ivec2 resolve_size = imageSize(resolve);\n"
			"	ivec2 aa_size = imageSize(multisampled);\n"
			"	ivec2 sample_count = resolve_size / aa_size;\n"
			"\n"
			"	if (any(greaterThanEqual(gl_GlobalInvocationID.xy, uvec2(resolve_size)))) return;"
			"\n"
			"	ivec2 resolve_coords = ivec2(gl_GlobalInvocationID.xy);\n"
			"	ivec2 aa_coords = resolve_coords / sample_count;\n"
			"	ivec2 sample_loc = ivec2(resolve_coords % sample_count);\n"
			"	int sample_index = sample_loc.x + (sample_loc.y * sample_count.y);\n"
			+ kernel +
			"}\n";

			LOG_ERROR(RSX, "Compute shader:\n%s", m_src);
		}

		std::vector<std::pair<VkDescriptorType, u8>> get_descriptor_layout() override
		{
			return
			{
				{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 }
			};
		}

		void declare_inputs() override
		{
			std::vector<vk::glsl::program_input> inputs =
			{
				{
					::glsl::program_domain::glsl_compute_program,
					vk::glsl::program_input_type::input_type_texture,
					{}, {},
					0,
					"multisampled"
				},
				{
					::glsl::program_domain::glsl_compute_program,
					vk::glsl::program_input_type::input_type_texture,
					{}, {},
					1,
					"resolve"
				}
			};

			m_program->load_uniforms(inputs);
		}

		void bind_resources() override
		{
			auto msaa_view = multisampled->get_view(0xDEADBEEF, rsx::default_remap_vector);
			auto resolved_view = resolve->get_view(0xAAE4, rsx::default_remap_vector);
			m_program->bind_uniform({ VK_NULL_HANDLE, msaa_view->value, multisampled->current_layout }, "multisampled", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_descriptor_set);
			m_program->bind_uniform({ VK_NULL_HANDLE, resolved_view->value, resolve->current_layout }, "resolve", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_descriptor_set);
		}

		void run(VkCommandBuffer cmd, vk::viewable_image* msaa_image, vk::viewable_image* resolve_image)
		{
			verify(HERE), msaa_image->samples() > 1, resolve_image->samples() == 1;

			multisampled = msaa_image;
			resolve = resolve_image;

			const u32 invocations_x = align(resolve_image->width(), cs_wave_x) / cs_wave_x;
			const u32 invocations_y = align(resolve_image->height(), cs_wave_y) / cs_wave_y;

			compute_task::run(cmd, invocations_x, invocations_y);
		}
	};

	struct cs_resolve_task : cs_resolve_base
	{
		cs_resolve_task(const std::string& format_prefix)
		{
			std::string kernel =
			"	vec4 aa_sample = imageLoad(multisampled, aa_coords, sample_index);\n"
			"	imageStore(resolve, resolve_coords, aa_sample);\n";

			build(kernel, format_prefix, 0);
		}
	};

	struct cs_unresolve_task : cs_resolve_base
	{
		cs_unresolve_task(const std::string& format_prefix)
		{
			std::string kernel =
			"	vec4 resolved_sample = imageLoad(resolve, resolve_coords);\n"
			"	imageStore(multisampled, aa_coords, sample_index, resolved_sample);\n";

			build(kernel, format_prefix, 1);
		}
	};

	struct depth_resolve_base : public overlay_pass
	{
		u8 samples_x = 1;
		u8 samples_y = 1;

		depth_resolve_base()
		{
			renderpass_config.set_depth_mask(true);
			renderpass_config.enable_depth_test(VK_COMPARE_OP_ALWAYS);
		}

		void build(const std::string& kernel, const std::string& extensions, bool stencil_texturing, bool input_is_multisampled)
		{
			vs_src =
				"#version 450\n"
				"#extension GL_ARB_separate_shader_objects : enable\n\n"
				"\n"
				"void main()\n"
				"{\n"
				"	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};\n"
				"	gl_Position = vec4(positions[gl_VertexIndex % 4], 0., 1.);\n"
				"}\n";

			fs_src =
				"#version 420\n"
				"#extension GL_ARB_separate_shader_objects : enable\n";
				fs_src += extensions +
				"\n"
				"layout(std140, set=0, binding=0) uniform static_data{ ivec4 regs[8]; };\n"
				"layout(set=0, binding=1) uniform sampler2D fs0;\n";

				if (stencil_texturing)
				{
					m_num_usable_samplers = 2;

					fs_src +=
					"layout(set=0, binding=2) uniform usampler2D fs1;\n";
				}

				fs_src +=
				"layout(pixel_center_integer) in vec4 gl_FragCoord;\n"
				"\n"
				"void main()\n"
				"{\n";
					fs_src += kernel +
				"}\n";

			if (input_is_multisampled)
			{
				auto sampler_loc = fs_src.find("sampler2D fs0");
				fs_src.insert(sampler_loc + 9, "MS");

				if (stencil_texturing)
				{
					sampler_loc = fs_src.find("sampler2D fs1");
					fs_src.insert(sampler_loc + 9, "MS");
				}
			}

			LOG_ERROR(RSX, "Resolve shader:\n%s", fs_src);
		}

		void update_uniforms(vk::glsl::program* /*program*/) override
		{
			m_ubo_offset = (u32)m_ubo.alloc<256>(8);
			auto dst = (s32*)m_ubo.map(m_ubo_offset, 128);
			dst[0] = samples_x;
			dst[1] = samples_y;
			m_ubo.unmap();
		}

		void update_sample_configuration(vk::image* msaa_image)
		{
			switch (msaa_image->samples())
			{
			case 1:
				fmt::throw_exception("MSAA input not multisampled!" HERE);
			case 2:
				samples_x = 2;
				samples_y = 1;
				break;
			case 4:
				samples_x = samples_y = 2;
				break;
			default:
				fmt::throw_exception("Unsupported sample count %d" HERE, msaa_image->samples());
			}
		}
	};

	struct depthonly_resolve : depth_resolve_base
	{
		depthonly_resolve()
		{
			build(
				"	ivec2 out_coord = ivec2(gl_FragCoord.xy);\n"
				"	ivec2 in_coord = (out_coord / regs[0].xy);\n"
				"	ivec2 sample_loc = out_coord % ivec2(regs[0].xy);\n"
				"	int sample_index = sample_loc.x + (sample_loc.y * regs[0].y);\n"
				"	float frag_depth = texelFetch(fs0, in_coord, sample_index).x;\n"
				"	gl_FragDepth = frag_depth;\n",
				"",
				false,
				true);
		}

		void run(vk::command_buffer& cmd, vk::viewable_image* msaa_image, vk::viewable_image* resolve_image, VkRenderPass render_pass)
		{
			update_sample_configuration(msaa_image);
			auto src_view = msaa_image->get_view(0xDEADBEEF, rsx::default_remap_vector);

			overlay_pass::run(
				cmd,
				(u16)resolve_image->width(), (u16)resolve_image->height(),
				resolve_image, src_view,
				render_pass);
		}
	};

	struct depthonly_unresolve : depth_resolve_base
	{
		depthonly_unresolve()
		{
			build(
				"	ivec2 pixel_coord = ivec2(gl_FragCoord.xy);\n"
				"	pixel_coord *= regs[0].xy;\n"
				"	pixel_coord.x += (gl_SampleID % regs[0].x);\n"
				"	pixel_coord.y += (gl_SampleID / regs[0].x);\n"
				"	float frag_depth = texelFetch(fs0, pixel_coord, 0).x;\n"
				"	gl_FragDepth = frag_depth;\n",
				"",
				false,
				false);
		}

		void run(vk::command_buffer& cmd, vk::viewable_image* msaa_image, vk::viewable_image* resolve_image, VkRenderPass render_pass)
		{
			renderpass_config.set_multisample_state(msaa_image->samples(), 0xFFFF, true, false, false);
			renderpass_config.set_multisample_shading_rate(1.f);
			update_sample_configuration(msaa_image);

			auto src_view = resolve_image->get_view(0xAAE4, rsx::default_remap_vector);

			overlay_pass::run(
				cmd,
				(u16)msaa_image->width(), (u16)msaa_image->height(),
				msaa_image, src_view,
				render_pass);
		}
	};

	struct depthstencil_resolve_AMD : depth_resolve_base
	{
		depthstencil_resolve_AMD()
		{
			renderpass_config.enable_stencil_test(
				VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,  // Always replace
				VK_COMPARE_OP_ALWAYS,                                                 // Always pass
				0xFF,                                                                 // Full write-through
				0);                                                                   // Unused

			m_num_usable_samplers = 2;

			build(
				"	ivec2 out_coord = ivec2(gl_FragCoord.xy);\n"
				"	ivec2 in_coord = (out_coord / regs[0].xy);\n"
				"	ivec2 sample_loc = out_coord % ivec2(regs[0].xy);\n"
				"	int sample_index = sample_loc.x + (sample_loc.y * regs[0].y);\n"
				"	float frag_depth = texelFetch(fs0, in_coord, sample_index).x;\n"
				"	uint frag_stencil = texelFetch(fs1, in_coord, sample_index).x;\n"
				"	gl_FragDepth = frag_depth;\n"
				"	gl_FragStencilRefARB = int(frag_stencil);\n",

				"#extension GL_ARB_shader_stencil_export : enable\n",

				true,
				true);
		}

		void run(vk::command_buffer& cmd, vk::viewable_image* msaa_image, vk::viewable_image* resolve_image, VkRenderPass render_pass)
		{
			update_sample_configuration(msaa_image);
			auto depth_view = msaa_image->get_view(0xDEADBEEF, rsx::default_remap_vector, VK_IMAGE_ASPECT_DEPTH_BIT);
			auto stencil_view = msaa_image->get_view(0xDEADBEEF, rsx::default_remap_vector, VK_IMAGE_ASPECT_STENCIL_BIT);

			overlay_pass::run(
				cmd,
				(u16)resolve_image->width(), (u16)resolve_image->height(),
				resolve_image, { depth_view, stencil_view },
				render_pass);
		}
	};

	struct depthstencil_unresolve_AMD : depth_resolve_base
	{
		depthstencil_unresolve_AMD()
		{
			renderpass_config.enable_stencil_test(
				VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,  // Always replace
				VK_COMPARE_OP_ALWAYS,                                                 // Always pass
				0xFF,                                                                 // Full write-through
				0);                                                                   // Unused

			m_num_usable_samplers = 2;

			build(
				"	ivec2 pixel_coord = ivec2(gl_FragCoord.xy);\n"
				"	pixel_coord *= regs[0].xy;\n"
				"	pixel_coord.x += (gl_SampleID % regs[0].x);\n"
				"	pixel_coord.y += (gl_SampleID / regs[0].x);\n"
				"	float frag_depth = texelFetch(fs0, pixel_coord, 0).x;\n"
				"	uint frag_stencil = texelFetch(fs1, pixel_coord, 0).x;\n"
				"	gl_FragDepth = frag_depth;\n"
				"	gl_FragStencilRefARB = int(frag_stencil);\n",

				"#extension GL_ARB_shader_stencil_export : enable\n",

				true,
				false);
		}

		void run(vk::command_buffer& cmd, vk::viewable_image* msaa_image, vk::viewable_image* resolve_image, VkRenderPass render_pass)
		{
			renderpass_config.set_multisample_state(msaa_image->samples(), 0xFFFF, true, false, false);
			renderpass_config.set_multisample_shading_rate(1.f);
			update_sample_configuration(msaa_image);

			auto depth_view = resolve_image->get_view(0xAAE4, rsx::default_remap_vector, VK_IMAGE_ASPECT_DEPTH_BIT);
			auto stencil_view = resolve_image->get_view(0xAAE4, rsx::default_remap_vector, VK_IMAGE_ASPECT_STENCIL_BIT);

			overlay_pass::run(
				cmd,
				(u16)msaa_image->width(), (u16)msaa_image->height(),
				msaa_image, { depth_view, stencil_view },
				render_pass);
		}
	};

	void resolve_image(vk::command_buffer& cmd, vk::viewable_image* dst, vk::viewable_image* src);
	void unresolve_image(vk::command_buffer& cmd, vk::viewable_image* dst, vk::viewable_image* src);
	void reset_resolve_resources();
	void clear_resolve_helpers();
}
