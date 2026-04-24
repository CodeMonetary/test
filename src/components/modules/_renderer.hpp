#pragma once

namespace components
{
	class _renderer final : public component
	{
	public:
		_renderer();
		const char* get_name() override { return "_renderer"; };

		static void register_dvars();

		// set when the viewmodel is being submitted with r_mirrorViewmodel active
		// consumed by d3d9ex::SetRenderState to invert D3DRS_CULLMODE on viewmodel draws
		static volatile bool mirror_viewmodel_active;

		// r_mirrorViewmodel v8: armed when a depth-hack proj VSCF is uploaded; decremented
		// per non-dhp matrix VSCF; cleared on std-proj VSCF. Used by SetVertexShaderConstantF
		// to also flip the lighting / per-mesh constants that follow the gun matrix.
		static volatile int  mirror_vscf_follow_remaining;

		// r_mirrorViewmodel dump : when > 0, hooks write detailed frame data to a file.
		// Set via `mirror_dump <N>` console command. Decremented by d3d9ex::Present.
		static volatile int  mirror_dump_frames_remaining;
		static int           mirror_dump_frame_counter;
		static void          mirror_dump_open(int frames);
		static void          mirror_dump_close();
		static bool          mirror_dump_active();
		static void          mirror_dump_write(const char* fmt, ...);
		static const char*   mirror_dump_renderstate_name(unsigned int state);

		static bool is_valid_technique_for_type(const game::Material* mat, const game::MaterialTechniqueType type);
		static void switch_technique(game::switch_material_t* swm, game::Material* material);
		static void switch_technique(game::switch_material_t* swm, const char* material_name);
		static void switch_material(game::switch_material_t* swm, const char* material_name);
	};
}
