#include "std_include.hpp"
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace components
{
	volatile bool _renderer::mirror_viewmodel_active = false;
	volatile int  _renderer::mirror_vscf_follow_remaining = 0;
	volatile int  _renderer::mirror_dump_frames_remaining = 0;
	int           _renderer::mirror_dump_frame_counter   = 0;
	static int    s_mirror_log_frame_counter             = 0;
	static FILE*  s_mirror_dump_file                      = nullptr;
	static char   s_mirror_dump_path[512]                 = {0};
	static int    s_mirror_dump_rs_count                  = 0;
	static int    s_mirror_dump_vscf_count                = 0;
	static int    s_mirror_dump_pscf_count                = 0;
	static int    s_mirror_dump_draw_count                = 0;

	bool _renderer::mirror_dump_active()
	{
		return s_mirror_dump_file != nullptr && mirror_dump_frames_remaining > 0;
	}

	void _renderer::mirror_dump_write(const char* fmt, ...)
	{
		if (!s_mirror_dump_file) return;
		va_list va; va_start(va, fmt);
		vfprintf(s_mirror_dump_file, fmt, va);
		va_end(va);
	}

	void _renderer::mirror_dump_open(int frames)
	{
		if (s_mirror_dump_file) { fclose(s_mirror_dump_file); s_mirror_dump_file = nullptr; }

		const auto* base = game::Dvar_FindVar("fs_basepath");
		const char* basepath = (base && base->current.string) ? base->current.string : ".";
		std::time_t t = std::time(nullptr);
		std::tm lt; localtime_s(&lt, &t);
		std::snprintf(s_mirror_dump_path, sizeof(s_mirror_dump_path),
			"%s\\main\\mirror_dump_%04d%02d%02d_%02d%02d%02d.txt",
			basepath,
			1900 + lt.tm_year, 1 + lt.tm_mon, lt.tm_mday,
			lt.tm_hour, lt.tm_min, lt.tm_sec);
		fopen_s(&s_mirror_dump_file, s_mirror_dump_path, "w");
		if (s_mirror_dump_file) setvbuf(s_mirror_dump_file, nullptr, _IONBF, 0);
		if (!s_mirror_dump_file)
		{
			game::Com_PrintMessage(0, utils::va("[mirror_dump] FAILED to open %s\n", s_mirror_dump_path), 0);
			mirror_dump_frames_remaining = 0;
			return;
		}
		mirror_dump_frame_counter = 0;
		s_mirror_dump_rs_count = 0;
		s_mirror_dump_vscf_count = 0;
		s_mirror_dump_pscf_count = 0;
		s_mirror_dump_draw_count = 0;
		mirror_dump_frames_remaining = frames;

		const int method = dvars::r_mirrorViewmodel_method ? dvars::r_mirrorViewmodel_method->current.integer : 0;
		const int cfix   = dvars::r_mirrorViewmodel_cullFix ? dvars::r_mirrorViewmodel_cullFix->current.integer : 0;
		fprintf(s_mirror_dump_file,
			"=== iw3xo r_mirrorViewmodel dump ===\n"
			"time        : %04d-%02d-%02d %02d:%02d:%02d\n"
			"frames      : %d (requested)\n"
			"method      : %d\n"
			"cullFix     : %d\n"
			"=============================================\n\n",
			1900 + lt.tm_year, 1 + lt.tm_mon, lt.tm_mday,
			lt.tm_hour, lt.tm_min, lt.tm_sec,
			frames, method, cfix);
		fflush(s_mirror_dump_file);

		game::Com_PrintMessage(0, utils::va("[mirror_dump] capturing %d frame(s) -> %s\n", frames, s_mirror_dump_path), 0);
	}

	void _renderer::mirror_dump_close()
	{
		if (!s_mirror_dump_file) return;
		fprintf(s_mirror_dump_file,
			"\n=== summary ===\n"
			"SetRenderState calls     : %d\n"
			"SetVertexShaderConstantF : %d\n"
			"SetPixelShaderConstantF  : %d\n"
			"DrawPrim/IndexedPrim     : %d\n",
			s_mirror_dump_rs_count, s_mirror_dump_vscf_count, s_mirror_dump_pscf_count, s_mirror_dump_draw_count);
		fclose(s_mirror_dump_file);
		s_mirror_dump_file = nullptr;
		mirror_dump_frames_remaining = 0;
		game::Com_PrintMessage(0, utils::va("[mirror_dump] finished -> %s\n", s_mirror_dump_path), 0);
	}

	const char* _renderer::mirror_dump_renderstate_name(unsigned int s)
	{
		switch (s) {
		case D3DRS_ZENABLE:              return "ZENABLE";
		case D3DRS_FILLMODE:             return "FILLMODE";
		case D3DRS_SHADEMODE:            return "SHADEMODE";
		case D3DRS_ZWRITEENABLE:         return "ZWRITEENABLE";
		case D3DRS_ALPHATESTENABLE:      return "ALPHATESTENABLE";
		case D3DRS_LASTPIXEL:            return "LASTPIXEL";
		case D3DRS_SRCBLEND:             return "SRCBLEND";
		case D3DRS_DESTBLEND:            return "DESTBLEND";
		case D3DRS_CULLMODE:             return "CULLMODE";
		case D3DRS_ZFUNC:                return "ZFUNC";
		case D3DRS_ALPHAREF:             return "ALPHAREF";
		case D3DRS_ALPHAFUNC:            return "ALPHAFUNC";
		case D3DRS_DITHERENABLE:         return "DITHERENABLE";
		case D3DRS_ALPHABLENDENABLE:     return "ALPHABLENDENABLE";
		case D3DRS_FOGENABLE:            return "FOGENABLE";
		case D3DRS_SPECULARENABLE:       return "SPECULARENABLE";
		case D3DRS_FOGCOLOR:             return "FOGCOLOR";
		case D3DRS_STENCILENABLE:        return "STENCILENABLE";
		case D3DRS_STENCILFUNC:          return "STENCILFUNC";
		case D3DRS_STENCILREF:           return "STENCILREF";
		case D3DRS_STENCILMASK:          return "STENCILMASK";
		case D3DRS_STENCILWRITEMASK:     return "STENCILWRITEMASK";
		case D3DRS_TEXTUREFACTOR:        return "TEXTUREFACTOR";
		case D3DRS_CLIPPING:             return "CLIPPING";
		case D3DRS_LIGHTING:             return "LIGHTING";
		case D3DRS_AMBIENT:              return "AMBIENT";
		case D3DRS_COLORVERTEX:          return "COLORVERTEX";
		case D3DRS_NORMALIZENORMALS:     return "NORMALIZENORMALS";
		case D3DRS_CLIPPLANEENABLE:      return "CLIPPLANEENABLE";
		case D3DRS_POINTSIZE:            return "POINTSIZE";
		case D3DRS_MULTISAMPLEANTIALIAS: return "MULTISAMPLEANTIALIAS";
		case D3DRS_MULTISAMPLEMASK:      return "MULTISAMPLEMASK";
		case D3DRS_COLORWRITEENABLE:     return "COLORWRITEENABLE";
		case D3DRS_BLENDOP:              return "BLENDOP";
		case D3DRS_SCISSORTESTENABLE:    return "SCISSORTESTENABLE";
		case D3DRS_SLOPESCALEDEPTHBIAS:  return "SLOPESCALEDEPTHBIAS";
		case D3DRS_TWOSIDEDSTENCILMODE:  return "TWOSIDEDSTENCILMODE";
		case D3DRS_CCW_STENCILFAIL:      return "CCW_STENCILFAIL";
		case D3DRS_CCW_STENCILZFAIL:     return "CCW_STENCILZFAIL";
		case D3DRS_CCW_STENCILPASS:      return "CCW_STENCILPASS";
		case D3DRS_CCW_STENCILFUNC:      return "CCW_STENCILFUNC";
		case D3DRS_COLORWRITEENABLE1:    return "COLORWRITEENABLE1";
		case D3DRS_COLORWRITEENABLE2:    return "COLORWRITEENABLE2";
		case D3DRS_COLORWRITEENABLE3:    return "COLORWRITEENABLE3";
		case D3DRS_BLENDFACTOR:          return "BLENDFACTOR";
		case D3DRS_SRGBWRITEENABLE:      return "SRGBWRITEENABLE";
		case D3DRS_DEPTHBIAS:            return "DEPTHBIAS";
		case D3DRS_SEPARATEALPHABLENDENABLE:return "SEPARATEALPHABLENDENABLE";
		case D3DRS_SRCBLENDALPHA:        return "SRCBLENDALPHA";
		case D3DRS_DESTBLENDALPHA:       return "DESTBLENDALPHA";
		case D3DRS_BLENDOPALPHA:         return "BLENDOPALPHA";
		default:                          return "?";
		}
	}

	void mirror_dump_inc_rs()   { s_mirror_dump_rs_count++; }
	void mirror_dump_inc_vscf() { s_mirror_dump_vscf_count++; }
	void mirror_dump_inc_pscf() { s_mirror_dump_pscf_count++; }
	void mirror_dump_inc_draw() { s_mirror_dump_draw_count++; }

	/* ---------------------------------------------------------- */
	/* ------------ create dynamic rendering buffers ------------ */

	// R_AllocDynamicIndexBuffer
	int alloc_dynamic_index_buffer(IDirect3DIndexBuffer9** ib, int size_in_bytes, const char* buffer_name, bool load_for_renderer)
	{
		if (!load_for_renderer) 
		{
			return 0;
		}
		
		if (HRESULT hr = game::get_device()->CreateIndexBuffer(size_in_bytes, (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY), D3DFMT_INDEX16, D3DPOOL_DEFAULT, ib, nullptr);
					hr < 0)
		{
			const char* msg = utils::function<const char* __stdcall(HRESULT)>(0x685F98)(hr); // R_ErrorDescription
			msg = utils::va("DirectX didn't create a 0x%.8x dynamic index buffer: %s\n", size_in_bytes, msg);

			utils::function<void(const char*)>(0x576A30)(msg); // Sys_Error
		}

		game::Com_PrintMessage(0, utils::va("D3D9: Created Indexbuffer (%s) of size: 0x%.8x\n", buffer_name, size_in_bytes), 0);
		return 0;
	}

	// R_InitDynamicIndexBufferState
	void init_dynamic_index_buffer_state(game::GfxIndexBufferState* ib, int index_count, const char* buffer_name, bool load_for_renderer)
	{
		ib->used = 0;
		ib->total = index_count;

		alloc_dynamic_index_buffer(&ib->buffer, 2 * index_count, buffer_name, load_for_renderer);
	}

	// R_AllocDynamicVertexBuffer
	char* alloc_dynamic_vertex_buffer(IDirect3DVertexBuffer9** vb, std::uint32_t size_in_bytes, const char* buffer_name, bool load_for_renderer)
	{
		if (!load_for_renderer)
		{
			return nullptr;
		}

		if (HRESULT hr = game::get_device()->CreateVertexBuffer(size_in_bytes, (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY), 0, D3DPOOL_DEFAULT, vb, nullptr);
					hr < 0)
		{
			const char* msg = utils::function<const char* __stdcall(HRESULT)>(0x685F98)(hr); // R_ErrorDescription
			msg = utils::va("DirectX didn't create a 0x%.8x dynamic vertex buffer: %s\n", size_in_bytes, msg);

			utils::function<void(const char*)>(0x576A30)(msg); // Sys_Error
		}

		game::Com_PrintMessage(0, utils::va("D3D9: Created Vertexbuffer (%s) of size: 0x%.8x\n", buffer_name, size_in_bytes), 0);
		return nullptr;
	}

	// R_InitDynamicVertexBufferState
	void init_dynamic_vertex_buffer_state(game::GfxVertexBufferState* vb, std::uint32_t bytes, const char* buffer_name, bool load_for_renderer)
	{
		vb->used = 0;
		vb->total = static_cast<int>(bytes);
		vb->verts = nullptr;

		alloc_dynamic_vertex_buffer(&vb->buffer, bytes, buffer_name, load_for_renderer);
	}

	// R_InitTempSkinBuf :: Temp skin buffer within backenddata
	void init_temp_skin_buf(std::uint32_t bytes)
	{
		auto* back_end_data = reinterpret_cast<game::GfxBackEndData*>(0xCC9F600);

		for (auto i = 0; i < 2; ++i)
		{
			back_end_data[i].tempSkinBuf = (char*)VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
			game::Com_PrintMessage(0, utils::va("Allocated tempSkinBuffer of size: 0x%.8x\n", bytes), 0);
		}
	}

	// R_CreateDynamicBuffers
	void create_dynamic_buffers()
	{
		auto& gfx_buf = *reinterpret_cast<game::GfxBuffers*>(0xD2B0840);
		const auto& r_loadForRenderer = game::Dvar_FindVar("r_loadForRenderer")->current.enabled;

		// default size in bytes
		std::uint32_t dynamic_vb_size = 1 * 1048576;
		std::uint32_t skinned_cache_size = 4 * 1048576;
		std::uint32_t temp_skin_size = 4 * 1048576;
		std::uint32_t dynamic_ib_size = 2 * 1048576;
		std::uint32_t pretess_ib_size = 2 * 1048576;

		// get size in bytes from dvars
		if (dvars::r_buf_dynamicVertexBuffer)
		{
			dynamic_vb_size = dvars::r_buf_dynamicVertexBuffer->current.integer * 1048576;
		}

		if (dvars::r_buf_skinnedCacheVb)
		{
			skinned_cache_size = dvars::r_buf_skinnedCacheVb->current.integer * 1048576;
		}

		if (dvars::r_buf_tempSkin)
		{
			temp_skin_size = dvars::r_buf_tempSkin->current.integer * 1048576;
		}

		if (dvars::r_buf_dynamicIndexBuffer)
		{
			dynamic_ib_size = dvars::r_buf_dynamicIndexBuffer->current.integer * 1048576;
		}

		if (dvars::r_buf_preTessIndexBuffer)
		{
			pretess_ib_size = dvars::r_buf_preTessIndexBuffer->current.integer * 1048576;
		}


		init_dynamic_vertex_buffer_state(&gfx_buf.dynamicVertexBufferPool[0], dynamic_vb_size, "dynamicVertexBufferPool", r_loadForRenderer);
		gfx_buf.dynamicVertexBuffer = gfx_buf.dynamicVertexBufferPool;


		init_dynamic_vertex_buffer_state(&gfx_buf.skinnedCacheVbPool[0], skinned_cache_size, "skinnedCacheVbPool", r_loadForRenderer);
		init_dynamic_vertex_buffer_state(&gfx_buf.skinnedCacheVbPool[1], skinned_cache_size, "skinnedCacheVbPool", r_loadForRenderer);
		init_temp_skin_buf(temp_skin_size);


		init_dynamic_index_buffer_state(&gfx_buf.dynamicIndexBufferPool[0], dynamic_ib_size, "dynamicIndexBufferPool", r_loadForRenderer);
		gfx_buf.dynamicIndexBuffer = gfx_buf.dynamicIndexBufferPool;


		init_dynamic_index_buffer_state(&gfx_buf.preTessIndexBufferPool[0], pretess_ib_size, "preTessIndexBufferPool", r_loadForRenderer);
		init_dynamic_index_buffer_state(&gfx_buf.preTessIndexBufferPool[1], pretess_ib_size, "preTessIndexBufferPool", r_loadForRenderer);
		gfx_buf.preTessBufferFrame = 0;
		gfx_buf.preTessIndexBuffer = gfx_buf.preTessIndexBufferPool;
	}

	/* ---------------------------------------------------------- */
	/* ---------- Alloc dynamic vertices (smodelCache) ---------- */

	// R_AllocDynamicVertexBuffer
	void alloc_dynamic_vertex_buffer()
	{
		auto& gfx_buf = *reinterpret_cast<game::GfxBuffers*>(0xD2B0840);

		// default size in bytes
		std::uint32_t smodel_cache_vb_size = 8 * 1048576;

		// get size in bytes from dvar
		if (dvars::r_buf_smodelCacheVb)
		{
			smodel_cache_vb_size = dvars::r_buf_smodelCacheVb->current.integer * 1048576;
		}

		if (const auto& r_loadForRenderer = game::Dvar_FindVar("r_loadForRenderer"); 
						r_loadForRenderer && r_loadForRenderer->current.enabled)
		{
			if (HRESULT hr = game::get_device()->CreateVertexBuffer(smodel_cache_vb_size, (D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY), 0, D3DPOOL_DEFAULT, &gfx_buf.smodelCacheVb, nullptr);
						hr < 0)
			{
				const char* msg = utils::function<const char* __stdcall(HRESULT)>(0x685F98)(hr); // R_ErrorDescription
				msg = utils::va("DirectX didn't create a 0x%.8x dynamic vertex buffer: %s\n", smodel_cache_vb_size, msg);

				utils::function<void(const char*)>(0x576A30)(msg); // Sys_Error
			}

			game::Com_PrintMessage(0, utils::va("D3D9: Created Vertexbuffer (smodelCacheVb) of size: 0x%.8x\n", smodel_cache_vb_size), 0);
		}
	}

	/* ---------------------------------------------------------- */
	/* ---------- Alloc dynamic indices (smodelCache) ----------- */

	void init_smodel_indices()
	{
		auto& gfx_buf = *reinterpret_cast<game::GfxBuffers*>(0xD2B0840);

		// default size in bytes
		std::uint32_t smodel_cache_ib_size = 2 * 1048576;

		// get size in bytes from dvar
		if (dvars::r_buf_smodelCacheIb)
		{
			smodel_cache_ib_size = dvars::r_buf_smodelCacheIb->current.integer * 1048576;
		}

		gfx_buf.smodelCache.used = 0;
		gfx_buf.smodelCache.total = static_cast<int>(smodel_cache_ib_size / 2); // why half?

		const auto mem_reserve = VirtualAlloc(nullptr, 0x200000u, MEM_RESERVE, PAGE_READWRITE);
		const auto mem_commit = VirtualAlloc(mem_reserve, 0x200000u, MEM_COMMIT, PAGE_READWRITE);

		if (!mem_commit || !mem_reserve)
		{
			if (!mem_commit && mem_reserve)
			{
				VirtualFree(mem_reserve, 0, MEM_RESET);
			}

			const char* msg = utils::va("r_init_smodel_indices :: Unable to allocate 0x%.8x bytes. Out of memory?\n", smodel_cache_ib_size);
			utils::function<void(const char*)>(0x576A30)(msg); // Sys_Error
		}

		game::Com_PrintMessage(0, utils::va("Allocated smodelCache (smodelCacheIb) of size: 0x%.8x\n", smodel_cache_ib_size), 0);
		gfx_buf.smodelCache.indices = static_cast<std::uint16_t*>(mem_reserve);
	}

	__declspec(naked) void init_smodel_indices_stub()
	{
		const static uint32_t retn_addr = 0x5F5E7D;
		__asm
		{
			call	init_smodel_indices;
			pop     edi;
			jmp		retn_addr;
		}
	}

	/* ---------------------------------------------------------- */
	/* ------ change warning limits to fit new buffer size ------ */

	__declspec(naked) void r_warn_temp_skin_size_limit_stub()
	{
		const static uint32_t mb_size = 1048576;
		const static uint32_t retn_addr = 0x643948;
		__asm
		{
			// replace 'cmp edx, 480000h'

			push	eax;
			mov		eax, dvars::r_buf_tempSkin;
			mov		eax, dword ptr[eax + 12];	// current->integer
			imul	eax, mb_size;				// get size in bytes

			cmp		edx, eax;					// warning limit compare
			pop		eax;

			jmp		retn_addr;
		}
	}

	__declspec(naked) void r_warn_max_skinned_cache_vertices_limit_stub()
	{
		const static uint32_t mb_size = 1048576;
		const static uint32_t retn_addr = 0x643822;
		__asm
		{
			// replace 'cmp edx, 480000h'

			push	eax;
			mov		eax, dvars::r_buf_skinnedCacheVb;
			mov		eax, dword ptr[eax + 12];	// current->integer
			imul	eax, mb_size;				// get size in bytes

			cmp		edx, eax;					// warning limit compare
			pop		eax;

			jmp		retn_addr;
		}
	}

	// ---------------------

	// called from _common::register_additional_dvars (R_Init->R_RegisterDvars)
	// these need to be re-registered on vid_restart because they are latched
	void _renderer::register_dvars()
	{
		// do not register dvars with the "default" register functions here. (creates duplicates)

		dvars::r_buf_skinnedCacheVb = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_skinnedCacheVb",
			/* desc		*/ "Size of skinnedCache Vertexbuffer (Size * 2 will be allocated) in Megabytes. Default : 4 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 4, // 24
			/* minVal	*/ 4,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_smodelCacheVb = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_smodelCacheVb",
			/* desc		*/ "Size of smodelCache Vertexbuffer in Megabytes. Default : 8 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 8, // 24
			/* minVal	*/ 8,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_smodelCacheIb = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_smodelCacheIb",
			/* desc		*/ "Size of smodelCache Indexbuffer in Megabytes. Default : 2 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 2, // 4
			/* minVal	*/ 2,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_tempSkin = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_tempSkin",
			/* desc		*/ "Size of tempSkin buffer in Megabytes. Default : 4 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 4, // 16
			/* minVal	*/ 4,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_dynamicVertexBuffer = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_dynamicVertexBuffer",
			/* desc		*/ "Size of dynamic Vertexbuffer in Megabytes. Default : 1 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 1, // 2
			/* minVal	*/ 1,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_dynamicIndexBuffer = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_dynamicIndexBuffer",
			/* desc		*/ "Size of dynamic Indexbuffer in Megabytes. Default : 2 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 2, // 4
			/* minVal	*/ 2,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);

		dvars::r_buf_preTessIndexBuffer = game::Dvar_RegisterIntWrapper(
			/* name		*/ "r_buf_preTessIndexBuffer",
			/* desc		*/ "Size of preTess Indexbuffer (Size * 2 will be allocated) in Megabytes. Default : 2 \n! DISABLE r_fastSkin if you are changing this dvar or the client will crash when hitting the old limit.",
			/* default	*/ 2, // 4
			/* minVal	*/ 2,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved | game::dvar_flags::latched);
	}


	/* ---------------------------------------------------------- */

	void codesampler_error([[maybe_unused]] game::MaterialShaderArgument* arg, [[maybe_unused]] game::GfxCmdBufSourceState* source, game::GfxCmdBufState* state, const char* sampler, [[maybe_unused]] int droptype, [[maybe_unused]] const char* msg, ...)
	{
		if (!sampler || !state || !state->material || !state->technique) 
		{
			return;
		}

		game::Com_PrintMessage(0, utils::va(
			"^1Tried to use sampler <%s> when it isn't valid!\n"
			"^7[Passdump]\n"
			"|-> Material ----- %s\n"
			"|-> Technique ----	%s\n"
			"|-> RenderTarget - %s\n"
			"|-> Not setting sampler using R_SetSampler!\n",
			sampler, state->material->info.name, state->technique->name, game::get_rendertarget_string(state->renderTargetId)), 0);
	}

	// R_SetupPassPerObjectArgs
	__declspec(naked) void codesampler_error01_stub()
	{
		const static uint32_t retn_addr = 0x64BD0F; // offset after call to R_SetSampler
		__asm
		{
			// - already pushed args -
			// push    fmt
			// push    msg
			// push    dropType

			mov     eax, [esp + 0x8];	// move sampler string into eax
			push	eax;				// decreased esp by 4
			mov     eax, [esp + 0x14];	// move GfxCmdBufState* into eax (now at 14h)
			push	eax;				// GfxCmdBufState*
			push	ebx;				// GfxCmdBufSourceState*
			push	edi;				// MaterialShaderArgument*

#if DEBUG
			call	codesampler_error;	// only dump info on debug builds
#endif
			add		esp, 28;

			mov     eax, [esp + 0x14];
			mov     ecx, [esp + 0x24];
			movzx   esi, word ptr[edi + 2];
			push    eax;
			push    ecx;
			push    ebx;
			mov     eax, ebp;

			jmp		retn_addr;
		}
	}

	// R_SetPassShaderStableArguments
	__declspec(naked) void codesampler_error02_stub()
	{
		const static uint32_t retn_addr = 0x64C36C; // offset after call to R_SetSampler
		__asm
		{
			// skip error and R_SetSampler
			add		esp, 12;

			mov     eax, [esp + 0x20];
			movzx   esi, word ptr[edi + 2];
			push    eax;
			mov     eax, [esp + 0x18];
			push    ebx;
			push    ebp;

			//call    R_SetSampler
			jmp		retn_addr;
		}
	}

	void cubemap_shot_f_sync_msg()
	{
		if (const auto& r_smp_backend = game::Dvar_FindVar("r_smp_backend");
						r_smp_backend && r_smp_backend->current.enabled)
		{
			game::Com_PrintMessage(0, "^1Error: ^7r_smp_backend must be set to 0!", 0);
		}

		if (const auto& r_aaSamples = game::Dvar_FindVar("r_aaSamples");
						r_aaSamples && r_aaSamples->current.integer != 1)
		{
			game::Com_PrintMessage(0, "^1Error: ^7r_aaSamples must be set to 1 (Disable AA)!", 0);
		}
	}

	__declspec(naked) void cubemap_shot_f_stub()
	{
		const static uint32_t R_SyncRenderThread_func = 0x5F6070;
		const static uint32_t retn_addr = 0x475411;
		__asm
		{
			call	R_SyncRenderThread_func;

			pushad;
			call	cubemap_shot_f_sync_msg;
			popad;

			jmp		retn_addr;
		}
	}


	


	/* ---------------------------------------------------------- */

	bool disable_prepass = false;

	// *
	// mat->techniqueSet->remappedTechniqueSet->techniques[type]
	bool _renderer::is_valid_technique_for_type(const game::Material* mat, const game::MaterialTechniqueType type)
	{
		if (	mat
			 && mat->techniqueSet
			 && mat->techniqueSet->remappedTechniqueSet
			 && mat->techniqueSet->remappedTechniqueSet->techniques[type])
		{
			return true;
		}

		return false;
	}


	// *
	// return remappedTechnique for technique_type if valid, stock technique otherwise
	void _renderer::switch_technique(game::switch_material_t* swm, game::Material* material)
	{
		if (material)
		{
			swm->technique = nullptr;

			if (is_valid_technique_for_type(material, swm->technique_type))
			{
				swm->technique = material->techniqueSet->remappedTechniqueSet->techniques[swm->technique_type];
			}

			swm->switch_technique = true;
			return;
		}

		// return stock technique if the above failed
		swm->technique = swm->current_technique;
	}


	// *
	// return remappedTechnique for technique_type if valid, stock technique otherwise
	void _renderer::switch_technique(game::switch_material_t* swm, const char* material_name)
	{
		

		if (const auto	material = game::Material_RegisterHandle(material_name, 3); 
						material)
		{
			swm->technique = nullptr;

			if (is_valid_technique_for_type(material, swm->technique_type))
			{
				swm->technique = material->techniqueSet->remappedTechniqueSet->techniques[swm->technique_type];
			}

			swm->switch_technique = true;
			return;
		}

		// return stock technique if the above failed
		swm->technique = swm->current_technique;
	}


	//*
	// return new material if valid, stock material otherwise
	void _renderer::switch_material(game::switch_material_t* swm, const char* material_name)
	{
		if (const auto	material = game::Material_RegisterHandle(material_name, 3); 
						material)
		{
			swm->material = material;
			swm->technique = nullptr;

			if (is_valid_technique_for_type(material, swm->technique_type))
			{
				swm->technique = material->techniqueSet->remappedTechniqueSet->techniques[swm->technique_type];
			}

			swm->switch_material = true;
			return;
		}

		// return stock material if the above failed
		swm->material = swm->current_material;
	}

	// *
	// :>
	int R_SetMaterial(game::MaterialTechniqueType techType, game::GfxCmdBufSourceState* src, game::GfxCmdBufState* state, game::GfxDrawSurf drawSurf)
	{
		game::switch_material_t mat = {};

		mat.current_material = game::rgp->sortedMaterials[drawSurf.fields.materialSortedIndex & 2047u];
		mat.current_technique = mat.current_material->techniqueSet->remappedTechniqueSet->techniques[techType];

		mat.material		  = mat.current_material;
		mat.technique		  = mat.current_technique;
		mat.technique_type	  = techType;

		disable_prepass		  = false; // always reset

		if (mat.current_material)
		{
			if (components::active.daynight_cycle)
			{
				daynight_cycle::overwrite_sky_material(&mat);
			}

			if (components::active.rtx)
			{
				if (!rtx::r_set_material_stub(&mat, state))
				{
					disable_prepass = true;
				}
			}

			// wireframe xmodels
			if (dvars::r_wireframe_xmodels && dvars::r_wireframe_xmodels->current.integer)
			{
				if (utils::starts_with(mat.current_material->info.name, "mc/"))
				{
					switch (dvars::r_wireframe_xmodels->current.integer)
					{
					case 1: // SHADED
						mat.technique_type = game::TECHNIQUE_WIREFRAME_SHADED;
						mat.switch_technique_type = true;
						break;

					case 2: // SOLID
						mat.technique_type = game::TECHNIQUE_WIREFRAME_SOLID;
						mat.switch_technique_type = true;
						break;

					case 3: // SHADED_WHITE
						disable_prepass = true;

						mat.technique_type	= game::TECHNIQUE_UNLIT;
						_renderer::switch_material(&mat, "iw3xo_showcollision_wire");
						break;

					case 4: // SOLID_WHITE
						mat.technique_type	= game::TECHNIQUE_UNLIT;
						_renderer::switch_material(&mat, "iw3xo_showcollision_wire");
						break;
					}
				}
			}

			// wireframe world
			if (dvars::r_wireframe_world && dvars::r_wireframe_world->current.integer)
			{
				if (utils::starts_with(mat.current_material->info.name, "wc/") && !utils::starts_with(mat.current_material->info.name, "wc/sky"))
				{
					switch (dvars::r_wireframe_world->current.integer)
					{
					case 1: // SHADED
						mat.technique_type = game::TECHNIQUE_WIREFRAME_SHADED;
						mat.switch_technique_type = true;
						break;

					case 2: // SOLID
						mat.technique_type = game::TECHNIQUE_WIREFRAME_SOLID;
						mat.switch_technique_type = true;
						break;

					case 3: // SHADED_WHITE
						disable_prepass = true;

						mat.technique_type	= game::TECHNIQUE_UNLIT;
						_renderer::switch_material(&mat, "iw3xo_showcollision_wire");
						break;

					case 4: // SOLID_WHITE
						mat.technique_type	= game::TECHNIQUE_UNLIT;
						_renderer::switch_material(&mat, "iw3xo_showcollision_wire");
						break;
					}
				}
			}


			// texcoord debugshader
			if (dvars::r_debugShaderTexcoord && dvars::r_debugShaderTexcoord->current.enabled)
			{
				if (utils::starts_with(mat.current_material->info.name, "mc/"))
				{
					disable_prepass = true;

					mat.technique_type = game::TECHNIQUE_UNLIT;
					_renderer::switch_technique(&mat, "debug_texcoords_dtex");
				}
				else if (utils::starts_with(mat.current_material->info.name, "wc/"))
				{
					disable_prepass = true;

					mat.technique_type = game::TECHNIQUE_UNLIT;
					_renderer::switch_technique(&mat, "debug_texcoords");
				}
			}

			if (!mat.switch_material && !mat.switch_technique && !mat.switch_technique_type)
			{
				if (state->origMaterial)
				{
					state->material = state->origMaterial;
				}
				if (state->origTechType)
				{
					state->techType = state->origTechType;
				}
			}
		}

		// save the original material
		state->origMaterial = state->material;

		// only switch to a different technique_type
		if (mat.switch_technique_type)
		{
			if (_renderer::is_valid_technique_for_type(mat.current_material, mat.technique_type))
			{
				_renderer::switch_technique(&mat, mat.current_material);
			}
		}

		// set stock or new material & technique
		state->material = mat.material;
		state->technique = mat.technique;


		if (!state->technique || (state->technique->flags & 1) != 0 && !game::rg->distortion)
		{
			return 0;
		}

		if (!mat.switch_material && !mat.switch_technique && !mat.switch_technique_type)
		{
			if ((mat.technique_type == game::TECHNIQUE_EMISSIVE || mat.technique_type == game::TECHNIQUE_UNLIT) && (state->technique->flags & 0x10) != 0 && !src->constVersions[4])
			{
				return 0;
			}
		}

		/*const auto& r_logFile = game::Dvar_FindVar("r_logFile");
		if (r_logFile && r_logFile->current.integer && mat.current_material)
		{
			const auto string = utils::va("R_SetMaterial( %s, %s, %i )\n", state->material->info.name, state->technique->name, mat.technique_type);
			const static uint32_t RB_LogPrint_func = 0x63CF40;
			__asm
			{
				pushad;
				mov		edx, string;
				call	RB_LogPrint_func;
				popad;
			}
		}*/

		state->origTechType = state->techType;
		state->techType = mat.technique_type;

		return 1;
	}


	__declspec(naked) void R_SetMaterial_stub()
	{
		const static uint32_t rtn_to_set_shadowable_light = 0x648F92;
		const static uint32_t retn_to_retn = 0x648F48;
		__asm
		{
			push	esi;		// techType
			call	R_SetMaterial;
			pop		esi;
			add     esp, 0x10;

			test    eax, eax;	// do not return BOOL if you test 4 byte sized registers :>
			jz      memes;
			jmp		rtn_to_set_shadowable_light;

		memes:
			jmp		retn_to_retn;
		}
	}

	__declspec(naked) void R_SetPrepassMaterial_stub()
	{
		const static uint32_t R_SetPrepassMaterial_func = 0x648DF0;
		const static uint32_t retn_addr = 0x648F41;
		__asm
		{
			push	eax;
			mov		al, disable_prepass;
			cmp		al, 1;
			pop		eax;

			// jump if true
			je		disable;
			call	R_SetPrepassMaterial_func;

		disable:
			jmp		retn_addr;
		}
	}

	__declspec(naked) void R_SetMaterial_Emissive_stub()
	{
		const static uint32_t retn_to_retn = 0x6490BF;
		__asm
		{
			push	esi;		// techType
			call	R_SetMaterial;
			pop		esi;
			add     esp, 10h;
			jmp		retn_to_retn;
		}
	}


	// no reason to dump the same shader multiple times over the lifespan of the current session
	std::unordered_set<std::string> r_dumped_shader_set;

	bool folder_ps_exists = false;
	bool folder_vs_exists = false;

	bool dumpedshader_contains(const std::unordered_set<std::string>& set, const std::string& s)
	{
		return set.contains(s);
	}

	void pixelshader_custom_constants(game::GfxCmdBufState* state)
	{
		// dump shaders at runtime ~> TODO: move that to its own function / hook
		if (dvars::r_dumpShaders && dvars::r_dumpShaders->current.enabled)
		{
			const auto base_path = game::Dvar_FindVar("fs_basepath");
			if (!base_path) 
			{
				return;
			}

			const std::string file_path = base_path->current.string + "\\iw3xo\\shader_dump\\"s;

			if (state && state->pass)
			{
				if (state->pass->vertexShader && state->pass->vertexShader->name)
				{
					// check if shader was already dumped
					if (!dumpedshader_contains(r_dumped_shader_set, "vs_"s + state->pass->vertexShader->name))
					{
						if (!folder_vs_exists)
						{
							std::filesystem::create_directories(file_path + "vertexShader\\");
							folder_vs_exists = true;
						}

						const std::uint16_t bin_size = state->pass->vertexShader->prog.loadDef.programSize;
						std::ofstream outfile(file_path + "vertexShader\\" + "vs_" + state->pass->vertexShader->name, std::ofstream::binary);

						outfile.write(reinterpret_cast<char*>(state->pass->vertexShader->prog.loadDef.program), bin_size * 4);
						outfile.close();

						r_dumped_shader_set.emplace("vs_"s + state->pass->vertexShader->name);
					}
				}

				if (state->pass->pixelShader && state->pass->pixelShader->name)
				{
					// check if shader was already dumped
					if (!dumpedshader_contains(r_dumped_shader_set, "ps_"s + state->pass->pixelShader->name))
					{
						if (!folder_ps_exists)
						{
							std::filesystem::create_directories(file_path + "pixelShader\\");
							folder_ps_exists = true;
						}

						const std::uint16_t bin_size = state->pass->pixelShader->prog.loadDef.programSize;
						std::ofstream outfile(file_path + "pixelShader\\" + "ps_" + state->pass->pixelShader->name, std::ofstream::binary);

						outfile.write(reinterpret_cast<char*>(state->pass->pixelShader->prog.loadDef.program), bin_size * 4);
						outfile.close();

						r_dumped_shader_set.emplace("ps_"s + state->pass->pixelShader->name);
					}
				}
			}
		}

		if (state && state->pass)
		{
			// loop through all argument defs to find custom codeconsts
			for (auto arg = 0; arg < state->pass->perObjArgCount + state->pass->perPrimArgCount + state->pass->stableArgCount; arg++)
			{
				if (const auto  arg_def = &state->pass->args[arg]; 
								arg_def && arg_def->type == 5)
				{
					if (components::active.daynight_cycle)
					{
						daynight_cycle::set_pixelshader_constants(state, arg_def);
					}

					if (components::active.ocean)
					{
						ocean::set_pixelshader_constants(state, arg_def);
					}
				}
			}
		}
	}

	__declspec(naked) void R_SetPassPixelShaderStableArguments_stub()
	{
		__asm
		{
			// stock op's
			pop     edi;
			pop     esi;
			pop     ebp;
			pop     ebx;
			add     esp, 8;

			// GfxCmdBufState
			mov		edx, [esp + 0xC];

			pushad;
			push	edx;
			call	pixelshader_custom_constants;
			add		esp, 4;
			popad;

			retn;
		}
	}

	
	void vertexshader_custom_constants([[maybe_unused]] game::GfxCmdBufSourceState* source, game::GfxCmdBufState* state)
	{
		// fixup cod4 code constants
		if (state && state->pass)
		{
			// loop through all argument defs to find custom codeconsts
			for (auto arg = 0; arg < state->pass->perObjArgCount + state->pass->perPrimArgCount + state->pass->stableArgCount; arg++)
			{
				if (const auto  arg_def = &state->pass->args[arg]; 
								arg_def && arg_def->type == 3)
				{
					if (components::active.ocean)
					{
						ocean::set_vertexshader_constants(state, arg_def);
					}
				}
			}
		}
	}

	__declspec(naked) void R_SetVertexShaderConstantFromCode_stub()
	{
		__asm
		{
			// stock op's
			pop     edi;
			pop     esi;
			pop     ebp;
			pop     ebx;
			add     esp, 8;

			// GfxCmdBufState
			mov		ecx, [esp + 0x8];
			mov		edx, [esp + 0xC];

			pushad;
			push	edx; // state
			push	ecx; // source
			call	vertexshader_custom_constants;
			add		esp, 8;
			popad;

			retn;
		}
	}

	// *
	// *

#pragma warning(push)
#pragma warning(disable: 6385)
#pragma warning(disable: 6386)
	void InfinitePerspectiveMatrix(const float tan_half_fov_x, const float tan_half_fov_y, const float z_near, float(*mtx)[4])
	{
		(*mtx)[0]  = 0.99951172f / tan_half_fov_x;
		(*mtx)[5]  = 0.99951172f / tan_half_fov_y;
		(*mtx)[10] = 0.99951172f;
		(*mtx)[11] = 1.0f;
		(*mtx)[14] = 0.99951171875f * -z_near;
	}
#pragma warning(pop)
	
	// rewrite of CG_GetViewFov()
	float calculate_gunfov_with_zoom(float fov_val)
	{
		float calc_fov = 80.0f;
		const auto& cg_fovMin = game::Dvar_FindVar("cg_fovMin");

		unsigned int offhand_index = game::cgs->predictedPlayerState.offHandIndex;
		if ((game::cgs->predictedPlayerState.weapFlags & 2) == 0)
		{
			offhand_index = game::cgs->predictedPlayerState.weapon;
		}

		// #
		auto check_flags_and_fovmin = [&]() -> float
		{
			if ((game::cgs->predictedPlayerState.eFlags & 0x300) != 0)
			{
				calc_fov = 55.0f;
			}

			if (cg_fovMin->current.value - calc_fov >= 0.0f)
			{
				calc_fov = cg_fovMin->current.value;
			}

			return calc_fov;
		};

		
		const auto weapon = game::BG_WeaponNames[offhand_index];
		if (game::cgs->predictedPlayerState.pm_type == 5)
		{
			return check_flags_and_fovmin();
		}
		
		calc_fov = fov_val;
		if (weapon->aimDownSight)
		{
			if (game::cgs->predictedPlayerState.fWeaponPosFrac == 1.0f)
			{
				calc_fov = weapon->fAdsZoomFov;
				return check_flags_and_fovmin();
			}
			
			if (game::cgs->predictedPlayerState.fWeaponPosFrac != 0.0f)
			{
				float ads_factor = 0.0f;
				
				if (game::cgs->playerEntity.bPositionToADS)
				{
					const float w_pos_frac = game::cgs->predictedPlayerState.fWeaponPosFrac - (1.0f - weapon->fAdsZoomInFrac);
					if (w_pos_frac <= 0.0f)
					{
						return check_flags_and_fovmin();
					}

					ads_factor = w_pos_frac / weapon->fAdsZoomInFrac;
				}
				else
				{
					const float w_pos_frac = game::cgs->predictedPlayerState.fWeaponPosFrac - (1.0f - weapon->fAdsZoomOutFrac);
					if (w_pos_frac <= 0.0f)
					{
						return check_flags_and_fovmin();
					}

					ads_factor = w_pos_frac / weapon->fAdsZoomOutFrac;
				}
				
				if (ads_factor > 0.0f)
				{
					calc_fov = calc_fov - ads_factor * (calc_fov - weapon->fAdsZoomFov);
				}
			}
		}
		
		return check_flags_and_fovmin();
	}

	void set_gunfov(game::GfxViewParms* view_parms)
	{
		if (dvars::cg_fov_tweaks && dvars::cg_fov_tweaks->current.enabled)
		{
			// calc gun fov (includes weapon zoom)
			const float gun_fov = calculate_gunfov_with_zoom(dvars::cg_fov_gun->current.value);
			const float w_fov = 0.75f * tanf(gun_fov * 0.01745329238474369f * 0.5f);

			const float tan_half_x = (static_cast<float>(game::cgs->refdef.width) / static_cast<float>(game::cgs->refdef.height)) * w_fov;
			const float tan_half_y = w_fov;

			// calc projection matrix
			float proj_mtx[4][4] = {};
			InfinitePerspectiveMatrix(tan_half_x, tan_half_y, view_parms->zNear, proj_mtx);

			// only overwrite the projection matrix ;)
			memcpy(view_parms->projectionMatrix.m, proj_mtx, sizeof(game::GfxMatrix));
		}

		// --- r_mirrorViewmodel --- multi-method mirror with logging ---
		const int method = dvars::r_mirrorViewmodel_method ? dvars::r_mirrorViewmodel_method->current.integer : 0;
		const int log_level = dvars::r_mirrorViewmodel_log ? dvars::r_mirrorViewmodel_log->current.integer : 0;

		// Logging: print view_parms state (rate-limited: once per 60 calls)
		if (log_level >= 1 && (s_mirror_log_frame_counter % 60 == 0))
		{
			game::Com_PrintMessage(0, utils::va(
				"[mirror] SVP: depthHackNearClip=%.4f  zNear=%.4f  zFar=%.1f  "
				"origin=(%.1f %.1f %.1f)  proj[0][0]=%.6f\n",
				view_parms->depthHackNearClip, view_parms->zNear, view_parms->zFar,
				view_parms->origin[0], view_parms->origin[1], view_parms->origin[2],
				view_parms->projectionMatrix.m[0][0]), 0);
		}
		s_mirror_log_frame_counter++;

		// --- r_mirrorViewmodel dump: print full GfxViewParms BEFORE modification ---
		if (_renderer::mirror_dump_active())
		{
			_renderer::mirror_dump_write(
				"\n--- SVP call @ frame %d ---\n"
				"  depthHackNearClip=%.6f  zNear=%.6f  zFar=%.3f\n"
				"  origin=(%.3f %.3f %.3f)\n"
				"  axis[0]=(%.6f %.6f %.6f)\n"
				"  axis[1]=(%.6f %.6f %.6f)\n"
				"  axis[2]=(%.6f %.6f %.6f)\n",
				_renderer::mirror_dump_frame_counter,
				view_parms->depthHackNearClip, view_parms->zNear, view_parms->zFar,
				view_parms->origin[0], view_parms->origin[1], view_parms->origin[2],
				view_parms->axis[0][0], view_parms->axis[0][1], view_parms->axis[0][2],
				view_parms->axis[1][0], view_parms->axis[1][1], view_parms->axis[1][2],
				view_parms->axis[2][0], view_parms->axis[2][1], view_parms->axis[2][2]);

			_renderer::mirror_dump_write("  view (before):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->viewMatrix.m[r][0], view_parms->viewMatrix.m[r][1],
				view_parms->viewMatrix.m[r][2], view_parms->viewMatrix.m[r][3]);

			_renderer::mirror_dump_write("  proj (before):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->projectionMatrix.m[r][0], view_parms->projectionMatrix.m[r][1],
				view_parms->projectionMatrix.m[r][2], view_parms->projectionMatrix.m[r][3]);

			_renderer::mirror_dump_write("  viewProj (before):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->viewProjectionMatrix.m[r][0], view_parms->viewProjectionMatrix.m[r][1],
				view_parms->viewProjectionMatrix.m[r][2], view_parms->viewProjectionMatrix.m[r][3]);
		}

		// Determine if this call is for the viewmodel scene
		const bool is_viewmodel_dhnc = (view_parms->depthHackNearClip != 0.0f);
		const bool is_viewmodel_znear = (view_parms->zNear > 0.0f && view_parms->zNear < 1.0f);

		// Select mirror method. Each method modifies different matrices / axes to
		// test where in the transform pipeline we can safely apply the mirror.
		// Gated by depthHackNearClip != 0 (always true for viewmodel scene per logs).
		const bool gate = is_viewmodel_dhnc;
		_renderer::mirror_viewmodel_active = (method != 0 && gate);

		if (method != 0 && gate) switch (method)
		{
		case 1: // flip clip-space X (negate projection column 0). Original approach.
			view_parms->projectionMatrix.m[0][0] = -view_parms->projectionMatrix.m[0][0];
			view_parms->projectionMatrix.m[1][0] = -view_parms->projectionMatrix.m[1][0];
			view_parms->projectionMatrix.m[2][0] = -view_parms->projectionMatrix.m[2][0];
			view_parms->projectionMatrix.m[3][0] = -view_parms->projectionMatrix.m[3][0];
			break;
		case 2: // flip view-projection first row (mirror in model/world space pre-view).
			// row-vector convention: VP' = scale(-1,1,1) * VP. Keeps clip-space X positive.
			view_parms->viewProjectionMatrix.m[0][0] = -view_parms->viewProjectionMatrix.m[0][0];
			view_parms->viewProjectionMatrix.m[0][1] = -view_parms->viewProjectionMatrix.m[0][1];
			view_parms->viewProjectionMatrix.m[0][2] = -view_parms->viewProjectionMatrix.m[0][2];
			view_parms->viewProjectionMatrix.m[0][3] = -view_parms->viewProjectionMatrix.m[0][3];
			break;
		case 3: // flip view matrix first row AND view-projection first row together.
			view_parms->viewMatrix.m[0][0] = -view_parms->viewMatrix.m[0][0];
			view_parms->viewMatrix.m[0][1] = -view_parms->viewMatrix.m[0][1];
			view_parms->viewMatrix.m[0][2] = -view_parms->viewMatrix.m[0][2];
			view_parms->viewMatrix.m[0][3] = -view_parms->viewMatrix.m[0][3];
			view_parms->viewProjectionMatrix.m[0][0] = -view_parms->viewProjectionMatrix.m[0][0];
			view_parms->viewProjectionMatrix.m[0][1] = -view_parms->viewProjectionMatrix.m[0][1];
			view_parms->viewProjectionMatrix.m[0][2] = -view_parms->viewProjectionMatrix.m[0][2];
			view_parms->viewProjectionMatrix.m[0][3] = -view_parms->viewProjectionMatrix.m[0][3];
			break;
		case 4: // negate camera right axis (axis[0]). Equivalent to physically mirrored camera.
			view_parms->axis[0][0] = -view_parms->axis[0][0];
			view_parms->axis[0][1] = -view_parms->axis[0][1];
			view_parms->axis[0][2] = -view_parms->axis[0][2];
			break;
		case 5: // flip view matrix COLUMN 0 (mirror X in view space, pre-projection).
			view_parms->viewMatrix.m[0][0] = -view_parms->viewMatrix.m[0][0];
			view_parms->viewMatrix.m[1][0] = -view_parms->viewMatrix.m[1][0];
			view_parms->viewMatrix.m[2][0] = -view_parms->viewMatrix.m[2][0];
			view_parms->viewMatrix.m[3][0] = -view_parms->viewMatrix.m[3][0];
			view_parms->viewProjectionMatrix.m[0][0] = -view_parms->viewProjectionMatrix.m[0][0];
			view_parms->viewProjectionMatrix.m[1][0] = -view_parms->viewProjectionMatrix.m[1][0];
			view_parms->viewProjectionMatrix.m[2][0] = -view_parms->viewProjectionMatrix.m[2][0];
			view_parms->viewProjectionMatrix.m[3][0] = -view_parms->viewProjectionMatrix.m[3][0];
			break;
		case 6: // flip everything (axis + all matrices) - strongest mirror attempt
			view_parms->axis[0][0] = -view_parms->axis[0][0];
			view_parms->axis[0][1] = -view_parms->axis[0][1];
			view_parms->axis[0][2] = -view_parms->axis[0][2];
			view_parms->projectionMatrix.m[0][0] = -view_parms->projectionMatrix.m[0][0];
			view_parms->projectionMatrix.m[1][0] = -view_parms->projectionMatrix.m[1][0];
			view_parms->projectionMatrix.m[2][0] = -view_parms->projectionMatrix.m[2][0];
			view_parms->projectionMatrix.m[3][0] = -view_parms->projectionMatrix.m[3][0];
			view_parms->viewProjectionMatrix.m[0][0] = -view_parms->viewProjectionMatrix.m[0][0];
			view_parms->viewProjectionMatrix.m[1][0] = -view_parms->viewProjectionMatrix.m[1][0];
			view_parms->viewProjectionMatrix.m[2][0] = -view_parms->viewProjectionMatrix.m[2][0];
			view_parms->viewProjectionMatrix.m[3][0] = -view_parms->viewProjectionMatrix.m[3][0];
			break;
		case 7: // flip only viewProjection column 0 (shader-facing matrix, clip-space X)
			view_parms->viewProjectionMatrix.m[0][0] = -view_parms->viewProjectionMatrix.m[0][0];
			view_parms->viewProjectionMatrix.m[1][0] = -view_parms->viewProjectionMatrix.m[1][0];
			view_parms->viewProjectionMatrix.m[2][0] = -view_parms->viewProjectionMatrix.m[2][0];
			view_parms->viewProjectionMatrix.m[3][0] = -view_parms->viewProjectionMatrix.m[3][0];
			break;
		default: break;
		}

		if (log_level >= 1 && method != 0 && gate)
		{
			game::Com_PrintMessage(0, utils::va(
				"[mirror] APPLIED method=%d  dhnc=%.4f  zNear=%.4f\n",
				method, view_parms->depthHackNearClip, view_parms->zNear), 0);
		}

		// --- r_mirrorViewmodel dump: print GfxViewParms AFTER modification ---
		if (_renderer::mirror_dump_active())
		{
			_renderer::mirror_dump_write(
				"  -> method=%d  gate=%d  active=%d\n",
				method, (int)gate, (int)_renderer::mirror_viewmodel_active);

			_renderer::mirror_dump_write("  view (after):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->viewMatrix.m[r][0], view_parms->viewMatrix.m[r][1],
				view_parms->viewMatrix.m[r][2], view_parms->viewMatrix.m[r][3]);

			_renderer::mirror_dump_write("  proj (after):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->projectionMatrix.m[r][0], view_parms->projectionMatrix.m[r][1],
				view_parms->projectionMatrix.m[r][2], view_parms->projectionMatrix.m[r][3]);

			_renderer::mirror_dump_write("  viewProj (after):\n");
			for (int r = 0; r < 4; ++r) _renderer::mirror_dump_write(
				"    % .6f  % .6f  % .6f  % .6f\n",
				view_parms->viewProjectionMatrix.m[r][0], view_parms->viewProjectionMatrix.m[r][1],
				view_parms->viewProjectionMatrix.m[r][2], view_parms->viewProjectionMatrix.m[r][3]);
		}
	}
	
	__declspec(naked) void R_SetViewParmsForScene_stub()
	{
		const static uint32_t retn_addr = 0x5FAA0B;
		__asm
		{
			pushad;
			push	edi; // viewParms
			call	set_gunfov;
			add		esp, 4;
			popad;
			
			// stock op's
			lea     ecx, [edi + 0xC0];
			jmp		retn_addr;
		}
	}

	// =====================================================================
	// fx_mirror (v26): mirror first-person FX (muzzleflash, brass, etc.)
	// when the viewmodel mirror pipeline is active.
	//
	// CoD4 cgame spawns first-person FX through FX_SpawnOrientedEffect at
	// 0x4A14B0 with origin/axis at world positions of view-bound tags
	// (tag_flash, tag_brass). They render in the world projection (no
	// dhp), so the off-screen RTT viewmodel mirror does NOT capture them.
	// They appear at the "real" right-hand barrel position even though
	// the gun is visually flipped to the left.
	//
	// Strategy: pre-hook FX_SpawnOrientedEffect; when |origin - vieworg|
	// is small (first-person FX) AND a mirror mode is active, reflect
	// origin and each axis row across the plane through the camera origin
	// with normal = camera right axis (refdef.viewaxis[1]). World FX are
	// far from the camera and unaffected.
	// =====================================================================
	namespace fx_mirror
	{
		static const uint32_t FX_SPAWN_ORIENTED_ADDR = 0x4A14B0;
		static unsigned char* g_trampoline = nullptr;

		extern "C" void __cdecl fx_orient_pre_hook(int /*markentnum*/, float* axis,
			void* /*def*/, int /*msec*/, float* origin)
		{
			if (!axis || !origin) return;

			const int mirror_fx = (dvars::r_mirrorViewmodel_mirrorFx
				? dvars::r_mirrorViewmodel_mirrorFx->current.integer : 0);
			if (!mirror_fx) return;

			const int rtt_on = (dvars::r_mirrorViewmodel_rtt
				? dvars::r_mirrorViewmodel_rtt->current.integer : 0);
			const int method = (dvars::r_mirrorViewmodel_method
				? dvars::r_mirrorViewmodel_method->current.integer : 0);
			if (!rtt_on && !method) return; // mirror feature off entirely

			if (!game::cgs) return;

			const float* vorg = game::cgs->refdef.vieworg;
			const float dx = origin[0] - vorg[0];
			const float dy = origin[1] - vorg[1];
			const float dz = origin[2] - vorg[2];
			const float d2 = dx * dx + dy * dy + dz * dz;

			const float maxd = (dvars::r_mirrorViewmodel_mirrorFxDist
				? dvars::r_mirrorViewmodel_mirrorFxDist->current.value : 64.0f);
			if (d2 > maxd * maxd) return; // world FX, leave alone

			const float* right = game::cgs->refdef.viewaxis[1];

			// Reflect origin around plane through vieworg with normal = right.
			// off' = off - 2 * dot(off, right) * right
			const float dot_o = dx * right[0] + dy * right[1] + dz * right[2];
			origin[0] -= 2.0f * dot_o * right[0];
			origin[1] -= 2.0f * dot_o * right[1];
			origin[2] -= 2.0f * dot_o * right[2];

			// Reflect each of the three axis rows (3 floats each) about right.
			// The result is a left-handed basis, which is exactly what we
			// want for a visual mirror (sprite/oriented FX render correctly
			// when their basis is reflected).
			for (int r = 0; r < 3; ++r)
			{
				float* row = axis + r * 3;
				const float dot_a = row[0] * right[0] + row[1] * right[1] + row[2] * right[2];
				row[0] -= 2.0f * dot_a * right[0];
				row[1] -= 2.0f * dot_a * right[1];
				row[2] -= 2.0f * dot_a * right[2];
			}

			const int log_left = (dvars::r_mirrorViewmodel_mirrorFxLog
				? dvars::r_mirrorViewmodel_mirrorFxLog->current.integer : 0);
			if (log_left > 0 && dvars::r_mirrorViewmodel_mirrorFxLog)
			{
				dvars::r_mirrorViewmodel_mirrorFxLog->current.integer = log_left - 1;
				game::Com_PrintMessage(0, utils::va(
					"[fx_mirror] reflected: origin=(%.1f %.1f %.1f) d=%.1f\n",
					origin[0], origin[1], origin[2], (float)sqrt((double)d2)), 0);
			}
		}

		// Detour stub. Original calling convention (__usercall):
		//   ecx     = markentnum
		//   edx     = axis (float* to 3x3)
		//   [esp+4] = def (FxEffectDef*)
		//   [esp+8] = msec_begin (int)
		//   [esp+12]= origin (float* to vec3)
		// Caller cleans up the 3 stack args after the call (cdecl-like).
		__declspec(naked) void fx_spawn_oriented_stub()
		{
			__asm
			{
				pushad;                  // saves: edi(+0) esi(+4) ebp(+8) esp(+12) ebx(+16) edx(+20) ecx(+24) eax(+28)

				// Pull our register args out of the saved frame.
				mov     eax, [esp + 24]; // markentnum (ecx)
				mov     ebx, [esp + 20]; // axis       (edx)

				// Stack args, accounting for pushad's 32 bytes:
				//   [esp+32] = original return addr
				//   [esp+36] = def
				//   [esp+40] = msec_begin
				//   [esp+44] = origin
				push    [esp + 44];      // origin
				push    [esp + 44];      // msec_begin (now +44 after one push)
				push    [esp + 44];      // def
				push    ebx;             // axis
				push    eax;             // markentnum
				call    fx_orient_pre_hook;
				add     esp, 20;

				popad;

				// Run the original first 5 bytes via the trampoline, which
				// then jumps to FX_SPAWN_ORIENTED_ADDR + 5 (continues normal
				// execution; ECX/EDX/stack args are untouched relative to
				// what the caller set up).
				jmp     dword ptr [g_trampoline];
			}
		}

		void install()
		{
			if (g_trampoline) return;

			// Allocate executable trampoline: original 5 bytes + JMP rel32 = 10 bytes.
			g_trampoline = static_cast<unsigned char*>(VirtualAlloc(
				nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!g_trampoline) return;

			// Copy the original first 5 bytes BEFORE installing the hook.
			memcpy(g_trampoline, reinterpret_cast<const void*>(FX_SPAWN_ORIENTED_ADDR), 5);

			// Append JMP rel32 -> FX_SPAWN_ORIENTED_ADDR + 5
			const intptr_t jmp_from = reinterpret_cast<intptr_t>(g_trampoline) + 5;
			const intptr_t jmp_to   = static_cast<intptr_t>(FX_SPAWN_ORIENTED_ADDR + 5);
			g_trampoline[5] = 0xE9;
			*reinterpret_cast<int32_t*>(g_trampoline + 6) =
				static_cast<int32_t>(jmp_to - (jmp_from + 5));

			FlushInstructionCache(GetCurrentProcess(), g_trampoline, 16);

			// Install the hook (overwrites first 5 bytes of the original
			// with JMP fx_spawn_oriented_stub).
			utils::hook(FX_SPAWN_ORIENTED_ADDR, fx_spawn_oriented_stub, HOOK_JUMP)
				.install()->quick();
		}
	}

	// =====================================================================
	// tag_mirror (v27): mirror first-person tag positions/orientations
	// returned by CG_DObjGetWorldBoneMatrix (0x433F00) when called with
	// the viewmodel pose. cgame's first-person muzzleflash, brass ejection,
	// dynamic light attach, etc. all read view-bound tag positions
	// (tag_flash, tag_brass, ...) through this function and then spawn
	// FX/lights at those world coordinates. By mirroring the output for
	// queries against &cgs->viewModelPose only, we move all view-attached
	// tag-derived spawns to the mirrored side without touching any world
	// entity tag query.
	//
	// Calling convention (__usercall):
	//   eax     = cpose_t* pose
	//   ecx     = int bone_index
	//   esi     = float* axis (out, 3x3)
	//   [esp+4] = DObj_s* obj
	//   [esp+8] = float* origin (out, vec3)
	// Caller cleans 2 stack args (cdecl-like).
	// =====================================================================
	namespace tag_mirror
	{
		static const uint32_t CG_DOBJ_GET_WORLD_BONE_MATRIX_ADDR = 0x433F00;
		static unsigned char* g_trampoline = nullptr;

		// Recursion-safe replacement: the naked stub at 0x433F00 marshals
		// the __usercall args into a normal __cdecl call to this C
		// function, which itself calls the original via the trampoline,
		// then post-processes the outputs. All state lives on the stack
		// frame, so nested calls (CG_DObjGetWorldBoneMatrix calling itself
		// for parent bones, or the engine re-entering the function from
		// inside another viewmodel computation) cannot clobber each other.
		extern "C" int __cdecl tag_replacement(void* pose, int bone_index,
			float* axis, void* obj, float* origin)
		{
			int result = 0;
			void* trampoline = g_trampoline;

			// Call the original via the trampoline using its native
			// __usercall convention (eax=pose, ecx=bone, esi=axis,
			// stack args obj/origin pushed in reverse order).
			// ESI is callee-saved per MSVC's __asm contract; we must
			// preserve it ourselves since we use it as an input register.
			__asm
			{
				push    esi;
				push    origin;
				push    obj;
				mov     esi, axis;
				mov     ecx, bone_index;
				mov     eax, pose;
				call    trampoline;
				add     esp, 8;
				mov     result, eax;
				pop     esi;
			}

			if (!result) return result;

			const int mirror_fx = (dvars::r_mirrorViewmodel_mirrorFx
				? dvars::r_mirrorViewmodel_mirrorFx->current.integer : 0);
			if (!mirror_fx) return result;

			const int rtt_on = (dvars::r_mirrorViewmodel_rtt
				? dvars::r_mirrorViewmodel_rtt->current.integer : 0);
			const int method = (dvars::r_mirrorViewmodel_method
				? dvars::r_mirrorViewmodel_method->current.integer : 0);
			if (!rtt_on && !method) return result;

			if (!game::cgs) return result;

			// Filter: only mirror tag results that were queried against the
			// viewmodel pose. World entities (other players, vehicles, etc.)
			// also flow through this function and must NOT be mirrored.
			if (pose != static_cast<void*>(&game::cgs->viewModelPose)) return result;

			const float* vorg  = game::cgs->refdef.vieworg;
			const float* right = game::cgs->refdef.viewaxis[1];

			if (origin)
			{
				const float dx = origin[0] - vorg[0];
				const float dy = origin[1] - vorg[1];
				const float dz = origin[2] - vorg[2];
				const float dot = dx * right[0] + dy * right[1] + dz * right[2];
				origin[0] -= 2.0f * dot * right[0];
				origin[1] -= 2.0f * dot * right[1];
				origin[2] -= 2.0f * dot * right[2];
			}

			// Reflecting the 3x3 axis produces a left-handed (det = -1)
			// basis. Many engine consumers (AxisToAngles, FX axis-aligned
			// sprite math, etc.) assume right-handedness and crash or
			// blow up on improper rotations. Origin mirroring alone is
			// sufficient for camera-billboard FX (muzzleflash sprite,
			// tracer line) — they spawn at the mirrored position and
			// face the camera regardless. Only mirror the axis when the
			// user explicitly opts in via r_mirrorViewmodel_mirrorFxAxis.
			const int mirror_axis = (dvars::r_mirrorViewmodel_mirrorFxAxis
				? dvars::r_mirrorViewmodel_mirrorFxAxis->current.integer : 0);
			if (axis && mirror_axis)
			{
				for (int r = 0; r < 3; ++r)
				{
					float* row = axis + r * 3;
					const float dot = row[0] * right[0] + row[1] * right[1] + row[2] * right[2];
					row[0] -= 2.0f * dot * right[0];
					row[1] -= 2.0f * dot * right[1];
					row[2] -= 2.0f * dot * right[2];
				}
			}

			const int log_left = (dvars::r_mirrorViewmodel_mirrorFxLog
				? dvars::r_mirrorViewmodel_mirrorFxLog->current.integer : 0);
			if (log_left > 0 && dvars::r_mirrorViewmodel_mirrorFxLog && origin)
			{
				dvars::r_mirrorViewmodel_mirrorFxLog->current.integer = log_left - 1;
				game::Com_PrintMessage(0, utils::va(
					"[tag_mirror] viewmodel tag reflected: origin=(%.1f %.1f %.1f)\n",
					origin[0], origin[1], origin[2]), 0);
			}

			return result;
		}

		// Naked stub installed at 0x433F00. Caller's __usercall convention:
		//   eax = pose, ecx = bone_index, esi = axis (out, 3x3),
		//   [esp+0] = ret addr, [esp+4] = obj, [esp+8] = origin (out, vec3)
		// Caller does `add esp, 8` after the call (cdecl-like).
		//
		// We translate this into a __cdecl call to tag_replacement, which
		// has its own stack frame and is therefore recursion-safe. After
		// tag_replacement returns the original function's return value in
		// EAX, we balance our pushed args with `add esp, 20` and `ret 0`,
		// leaving the caller's `obj`/`origin` on the stack so the caller's
		// own `add esp, 8` cleans up correctly.
		__declspec(naked) void getbonematrix_stub()
		{
			__asm
			{
				// Push args in reverse order for cdecl call.
				// At entry: [esp+0]=ret, [esp+4]=obj, [esp+8]=origin.
				push    [esp + 8];   // origin
				push    [esp + 8];   // obj (was at +4, now at +8 after push)
				push    esi;         // axis
				push    ecx;         // bone_index
				push    eax;         // pose
				call    tag_replacement;
				add     esp, 20;     // clean our 5 pushed args
				ret     0;           // return to caller; caller cleans its 2 stack args
			}
		}

		void install()
		{
			if (g_trampoline) return;

			g_trampoline = static_cast<unsigned char*>(VirtualAlloc(
				nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!g_trampoline) return;

			memcpy(g_trampoline,
				reinterpret_cast<const void*>(CG_DOBJ_GET_WORLD_BONE_MATRIX_ADDR), 5);

			const intptr_t jmp_from = reinterpret_cast<intptr_t>(g_trampoline) + 5;
			const intptr_t jmp_to   = static_cast<intptr_t>(CG_DOBJ_GET_WORLD_BONE_MATRIX_ADDR + 5);
			g_trampoline[5] = 0xE9;
			*reinterpret_cast<int32_t*>(g_trampoline + 6) =
				static_cast<int32_t>(jmp_to - (jmp_from + 5));

			FlushInstructionCache(GetCurrentProcess(), g_trampoline, 16);

			utils::hook(CG_DOBJ_GET_WORLD_BONE_MATRIX_ADDR,
				getbonematrix_stub, HOOK_JUMP).install()->quick();
		}
	}

	_renderer::_renderer()
	{
		/*
		* Increase the amount of skinned vertices (bone controlled meshes) per frame.
		*      (R_MAX_SKINNED_CACHE_VERTICES | TEMP_SKIN_BUF_SIZE) Warnings
		*           'r_fastSkin' or 'r_skinCache' needs to be disabled or
		*			  the client will crash if you hit an unkown limit
		*/

		// Create dynamic rendering buffers
		utils::hook(0x5F3EC2, create_dynamic_buffers, HOOK_CALL).install()->quick();
		utils::hook(0x5F3EA9, alloc_dynamic_vertex_buffer, HOOK_CALL).install()->quick();

		// Alloc dynamic indices (smodelCache)
		utils::hook::nop(0x5F5D97, 7); // clear
		utils::hook(0x5F5D97, init_smodel_indices_stub, HOOK_JUMP).install()->quick();

		// Change 'R_WARN_TEMP_SKIN_BUF_SIZE' warning limit to new buffer size
		utils::hook::nop(0x643942, 6); // clear
		utils::hook(0x643942, r_warn_temp_skin_size_limit_stub, HOOK_JUMP).install()->quick();

		// Change 'R_WARN_MAX_SKINNED_CACHE_VERTICES' warning limit to new buffer size
		utils::hook::nop(0x64381C, 6); // clear
		utils::hook(0x64381C, r_warn_max_skinned_cache_vertices_limit_stub, HOOK_JUMP).install()->quick();


		/* ---------------------------------------------------------- */

		// hook "Com_Error(1, "Tried to use '%s' when it isn't valid\n", codeSampler)" to skip the R_SetSampler call
		utils::hook(0x64BCF1, codesampler_error01_stub, HOOK_JUMP).install()->quick(); // R_SetupPassPerObjectArgs
		utils::hook(0x64C350, codesampler_error02_stub, HOOK_JUMP).install()->quick(); // R_SetPassShaderStableArguments (not really used)

		// fix cubemapshot_f (needs disabled AA, disabled r_smp_backend and game-resolution > then cubemapshot resolution)
		utils::hook::nop(0x47549E, 3);				// start with suffix "_rt" and not with junk memory
		utils::hook::set<BYTE>(0x4754D5 + 2, 0xB0); // end on "_dn" + 1 instead of "_bk" (6 images)
		utils::hook(0x47540C, cubemap_shot_f_stub, HOOK_JUMP).install()->quick(); // dvar info

		/* ---------------------------------------------------------- */

		static std::vector r_wireframe_enum =
		{
			"NONE",
			"SHADED",
			"SOLID",
			"SHADED_WHITE",
			"SOLID_WHITE"
		};

		dvars::r_wireframe_world = game::Dvar_RegisterEnum(
			/* name		*/ "r_wireframe_world",
			/* desc		*/ "Draw world objects using their wireframe technique",
			/* default	*/ 0,
			/* enumSize	*/ r_wireframe_enum.size(),
			/* enumData */ r_wireframe_enum.data(),
			/* flags	*/ game::dvar_flags::none);

		dvars::r_wireframe_xmodels = game::Dvar_RegisterEnum(
			/* name		*/ "r_wireframe_xmodels",
			/* desc		*/ "Draw xmodels using their wireframe technique",
			/* default	*/ 0,
			/* enumSize	*/ r_wireframe_enum.size(),
			/* enumData */ r_wireframe_enum.data(),
			/* flags	*/ game::dvar_flags::none);

		dvars::r_debugShaderTexcoord = game::Dvar_RegisterBool(
			/* name		*/ "r_debugShaderTexcoord",
			/* desc		*/ "Show surface UV's / Texcoords",
			/* default	*/ false,
			/* flags	*/ game::dvar_flags::none);


		// hook R_SetMaterial
		utils::hook(0x648F86, R_SetMaterial_stub, HOOK_JUMP).install()->quick();
		utils::hook(0x648F3C, R_SetPrepassMaterial_stub, HOOK_JUMP).install()->quick();
		utils::hook(0x6490B7, R_SetMaterial_Emissive_stub, HOOK_JUMP).install()->quick();


		// custom pixelshader code constants
		utils::hook::nop(0x64BEDB, 7);
		utils::hook(0x64BEDB, R_SetPassPixelShaderStableArguments_stub, HOOK_JUMP).install()->quick();

		// custom vertexshader code constants / per object :: R_SetPassShaderObjectArguments
		utils::hook::nop(0x64BD22, 7);
		utils::hook(0x64BD22, R_SetVertexShaderConstantFromCode_stub, HOOK_JUMP).install()->quick();

		// separate world and viewmodel fov
		utils::hook::nop(0x5FAA05, 6);
		utils::hook(0x5FAA05, R_SetViewParmsForScene_stub, HOOK_JUMP).install()->quick();

		dvars::cg_fov_tweaks = game::Dvar_RegisterBool(
			/* name		*/ "cg_fov_tweaks",
			/* desc		*/ "Enable gun fov tweaks",
			/* default	*/ false,
			/* flags	*/ game::dvar_flags::saved);
		
		dvars::cg_fov_gun = game::Dvar_RegisterFloat(
			/* name		*/ "cg_fov_gun",
			/* desc		*/ "Adjust gun fov separately (wont effect world fov)",
			/* default	*/ 65.0f,
			/* minVal	*/ 20.0f,
			/* maxVal	*/ 160.0f,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_method = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_method",
			/* desc		*/ "Mirror viewmodel method: 0=off, 1=proj col0, 2=VP row0, 3=view+VP row0, 4=axis[0], 5=view+VP col0, 6=axis+proj+VP col0, 7=VP col0 only",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 7,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_cullFix = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_cullFix",
			/* desc		*/ "Cull-mode override during mirrored viewmodel: 0=off, 1=swap CW<->CCW, 2=force CCW, 3=force CW, 4=force NONE",
			/* default	*/ 2,
			/* minVal	*/ 0,
			/* maxVal	*/ 4,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_log = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_log",
			/* desc		*/ "Log mirror diagnostics: 0=off, 1=per-scene summary, 2=verbose (incl. cull state)",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 2,
			/* flags	*/ game::dvar_flags::none);

		dvars::r_mirrorViewmodel_flipVSCF = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_flipVSCF",
			/* desc		*/ "Flip viewProjection column 0 at VSCF upload (0=off, 1=flip only the depth-hack proj upload (gun matrix), 2=flip dhp upload + r_mirrorViewmodel_flipFollow following matrix uploads). v8: independent of method/vm_active; gated purely on c2[3] signature.",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 2,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_flipFollow = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_flipFollow",
			/* desc		*/ "Number of matrix VSCF uploads after the depth-hack proj to also flip when r_mirrorViewmodel_flipVSCF=2. Catches per-mesh / lighting matrices in the gun draw block. Window disarms on next std-proj upload.",
			/* default	*/ 32,
			/* minVal	*/ 0,
			/* maxVal	*/ 1024,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_flipAxis = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_flipAxis",
			/* desc		*/ "v11: which 4-float subset of the targeted register block to negate. 0..3 = negate row N (4 contiguous floats: locals[N*4..N*4+3] = register cN entirely - row N if storage is row-major). 4..7 = negate column N (locals[N], locals[N+4], locals[N+8], locals[N+12] = first/2nd/3rd/4th element of each register - row N if storage is column-major / D3D9 default). 8 = full negation (all 16 floats; clip.w sign flips -> gun clipped behind camera; diagnostic). 9 = row 0 + col 0 combined (v10 axis=2). For a clean clip.x mirror, exactly ONE of values 0..7 should produce horizontal flip - which one identifies the matrix layout.",
			/* default	*/ 4,
			/* minVal	*/ 0,
			/* maxVal	*/ 9,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_flipReg = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_flipReg",
			/* desc		*/ "v11: which register start to apply the flip to when flipVSCF fires. 0 = c0-c3 (the depth-hack proj/wvp, default). 4 = c4-c7 (per-mesh world matrix). 24 = c24-c27 (alt matrix uploaded only during DHP #2 / hands pass). Use this to test which register set the gun vertex shader actually uses for clip-space transform - if c0-c3 flip with all flipAxis values 0..7 produces no clean mirror, try flipReg 4 or 24.",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 64,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_rtt = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_rtt",
			/* desc		*/ "v12: render-to-texture mirror mode. 0 = off (use matrix-flip path: r_mirrorViewmodel_method / flipVSCF). 1 = on. When on, the viewmodel is rendered unflipped to an off-screen texture, then composited back onto the back-buffer with horizontally inverted UV. This produces a pixel-perfect mirror with NO handedness inversion (tangents/normals/cull stay correct). Disables matrix-flip path when active.",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 1,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_rttBlend = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_rttBlend",
			/* desc		*/ "v13: blend mode for compositing the off-screen viewmodel texture onto the back-buffer. 0 = SRCALPHA/INVSRCALPHA (relies on gun shader writing alpha=0 outside the gun mesh; black artifacts if it doesn't). 1 = SRCALPHA/INVSRCALPHA + ALPHATEST > 0 (discards cleared alpha=0 pixels). 2 = additive ONE/ONE (gun adds on top of world; cleared regions add 0; brightness may double where gun overlaps world). 3 = additive ONE/ONE + ALPHATEST > 0 (only gun pixels add).",
			/* default	*/ 2,
			/* minVal	*/ 0,
			/* maxVal	*/ 3,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_rttEarlyComposite = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_rttEarlyComposite",
			/* desc		*/ "v20: composite the mirrored viewmodel before the HUD (lets the HUD draw on top of the gun instead of being covered). Detection signal is the engine's final post-FX pixel-shader constant (PSCF c7 = -0.066,-0.066,-0.066,2.773585) which fires once per frame immediately before the first HUD ortho upload. 0 = composite at EndScene only (gun covers HUD). 1 = composite at the post-FX/HUD boundary (HUD on top of gun, default). 2 = composite at both points (diagnostic).",
			/* default	*/ 1,
			/* minVal	*/ 0,
			/* maxVal	*/ 2,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_compositeSrgb = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_compositeSrgb",
			/* desc		*/ "v24: gamma encoding for the off-screen viewmodel composite. The engine renders the world with a tonemap pass that bakes an sRGB-like gamma curve into the back-buffer; the gun is rendered to a plain off-screen RT and bypasses that pass, which produces a slightly warm/desaturated 'sandy' tint when the linear gun pixels are written straight onto the gamma-encoded back-buffer. Modes select sampler/renderstate sRGB flags during composite. 0 = SRGBTEXTURE=0, SRGBWRITE=0 (legacy, raw linear -> raw bb, sandy tint). 1 = SRGBTEXTURE=0, SRGBWRITE=1 (linear -> hardware sRGB encode on write, default). 2 = SRGBTEXTURE=1, SRGBWRITE=0 (sRGB sample -> raw write). 3 = SRGBTEXTURE=1, SRGBWRITE=1 (sRGB sample -> sRGB write).",
			/* default	*/ 1,
			/* minVal	*/ 0,
			/* maxVal	*/ 3,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_rttTonemapInject = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_rttTonemapInject",
			/* desc		*/ "v25: lighting-correct mirrored gun by compositing the off-screen viewmodel into the engine's tonemap SOURCE texture before the final tonemap-output fullscreen draw. This makes the engine apply the same film/contrast/color-grade curve to the gun that it already applies to the world, avoiding the warm 'sandy' tint seen when the gun is pasted onto the back-buffer after tonemap. 0 = legacy v23/v24 behavior (composite after tonemap to bb). 1 = inject into tonemap source when the PSCF c5/c6/c7 fingerprint is available, fallback to bb composite if injection fails (default).",
			/* default	*/ 1,
			/* minVal	*/ 0,
			/* maxVal	*/ 1,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_mirrorFx = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_mirrorFx",
			/* desc		*/ "v26: mirror first-person weapon FX (muzzleflash, brass ejection, etc.) so they line up with the mirrored viewmodel. Pre-hooks FX_SpawnOrientedEffect; when the spawn origin is within r_mirrorViewmodel_mirrorFxDist of the camera AND a mirror mode is active, reflects origin/axis across the plane through the camera with normal = camera right axis. World FX (far from camera) are unaffected. 0 = off (default), 1 = on.",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 1,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_mirrorFxDist = game::Dvar_RegisterFloat(
			/* name		*/ "r_mirrorViewmodel_mirrorFxDist",
			/* desc		*/ "v26: distance threshold (in world units, from camera origin) below which an FX spawn is considered first-person and gets mirrored. Defaults to 64 to capture muzzleflash/brass attached at view tags without affecting world FX (smoke, world muzzleflashes from other players, etc.).",
			/* default	*/ 64.0f,
			/* minVal	*/ 0.0f,
			/* maxVal	*/ 4096.0f,
			/* flags	*/ game::dvar_flags::saved);

		dvars::r_mirrorViewmodel_mirrorFxLog = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_mirrorFxLog",
			/* desc		*/ "v26: log next N first-person FX reflections to console (decremented on each event). Useful for verifying which FX are picked up by the hook.",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 1024,
			/* flags	*/ game::dvar_flags::none);

		dvars::r_mirrorViewmodel_mirrorFxAxis = game::Dvar_RegisterInt(
			/* name		*/ "r_mirrorViewmodel_mirrorFxAxis",
			/* desc		*/ "v27a: also reflect the 3x3 axis matrix (orientation) of mirrored viewmodel tag results, in addition to the origin. Reflecting the axis produces a left-handed (det = -1) basis which can crash the engine in some FX paths (AxisToAngles, axis-aligned sprite math, etc.). Origin-only mirroring is sufficient for camera-billboard FX (muzzleflash sprite, brass spawn position, tracer line origin) which look correct because they billboard to the camera. 0 = mirror origin only (default, safe). 1 = mirror origin + axis (full reflection, may crash on shooting depending on weapon FX).",
			/* default	*/ 0,
			/* minVal	*/ 0,
			/* maxVal	*/ 1,
			/* flags	*/ game::dvar_flags::saved);

		// Install the FX mirror detour. Safe even when r_mirrorViewmodel_mirrorFx
		// is 0 because the pre-hook bails immediately in that case.
		fx_mirror::install();

		// v27: install the tag-mirror detour on CG_DObjGetWorldBoneMatrix.
		// This catches first-person tag-derived FX (muzzleflash, brass,
		// dynamic light attach, ...) that don't go through the v26
		// FX_SpawnOrientedEffect path. Gated by the same r_mirrorViewmodel_mirrorFx
		// dvar; the post-hook bails when the dvar is 0.
		tag_mirror::install();

		// increase fps cap to 125 for menus and loadscreen
		utils::hook::set<BYTE>(0x500174 + 2, 8);
		utils::hook::set<BYTE>(0x500177 + 2, 8);


		command::add("mirror_dump", "<frames>", "Capture <frames> frames of full mirror diagnostics to main/mirror_dump_<timestamp>.txt (default 3). Suggested bind: /bind F10 \"mirror_dump 3\".", [](command::params params)
		{
			int frames = 3;
			if (params.length() >= 2)
			{
				frames = atoi(params[1]);
				if (frames < 1)   frames = 1;
				if (frames > 120) frames = 120;
			}
			_renderer::mirror_dump_open(frames);
		});

		command::add("dumpreflections", "", "", [this](command::params)
		{
			const auto gfx = game::DB_FindXAssetHeader(game::ASSET_TYPE_GFXWORLD, utils::va("maps/mp/%s.d3dbsp", game::Dvar_FindVar("ui_mapname")->current.string)).gfxWorld;

			for (size_t i = 0; i < gfx->reflectionProbeCount; i++)
			{
				auto* probe = gfx->reflectionProbes[i].reflectionImage->texture.cubemap;
				for (auto j = 0; j < 6; j++)
				{
					const auto& base_path = game::Dvar_FindVar("fs_basepath");
					std::string file_path = base_path->current.string + "\\iw3xo\\reflection_cubes\\"s;
					std::filesystem::create_directories(file_path);

					IDirect3DSurface9* surface = nullptr;
					probe->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &surface);

					const char* str = utils::va("%s\\probe%d_side%d.png", file_path.c_str(), i, j);

					D3DXSaveSurfaceToFileA(str, D3DXIMAGE_FILEFORMAT::D3DXIFF_PNG, surface, nullptr, nullptr);
				}
			}
		});
	}
}