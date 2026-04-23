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

		static bool is_valid_technique_for_type(const game::Material* mat, const game::MaterialTechniqueType type);
		static void switch_technique(game::switch_material_t* swm, game::Material* material);
		static void switch_technique(game::switch_material_t* swm, const char* material_name);
		static void switch_material(game::switch_material_t* swm, const char* material_name);
	};
}
