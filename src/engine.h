// cod4mirror — vanilla iw3mp.exe (CoD4 1.7 MP) engine bindings.
//
// Addresses confirmed against the user-supplied iw3mp.exe (PE timestamp
// 2008-06-19, 3.3 MB):
//
//   Dvar_RegisterInt     = 0x0056C600   __cdecl, 10 args
//   CG_DObjGetWorldBoneMatrix = 0x00433F00   __cdecl, prologue 8 bytes
//                                            (83 EC 30 53 8B 5C 24 38)
//
// Anything address-sensitive lives here so future versions of iw3mp can
// be ported by editing this file alone.
#pragma once
#include <cstdint>
#include <cstring>

namespace cod4mirror::engine
{
	// dvar struct (subset — we only touch 'current.integer' / 'current.value').
	// Real layout in iw3mp.exe is identical to iw3xo's reverse-engineered
	// version. We keep the offsets via a forward-declared opaque type and
	// access fields by raw byte offset to avoid mismatches.
	struct dvar_s; // opaque

	// dvar type enum values used by the unified Dvar_Register* function.
	enum DvarType : int
	{
		DVAR_TYPE_BOOL    = 0x0,
		DVAR_TYPE_FLOAT   = 0x1,
		DVAR_TYPE_FLOAT_2 = 0x2,
		DVAR_TYPE_FLOAT_3 = 0x3,
		DVAR_TYPE_FLOAT_4 = 0x4,
		DVAR_TYPE_INT     = 0x5,
		DVAR_TYPE_ENUM    = 0x6,
		DVAR_TYPE_STRING  = 0x7,
		DVAR_TYPE_COLOR   = 0x8,
	};

	// dvar flags. Only DVAR_FLAG_SAVED is interesting for our settings.
	enum DvarFlags : std::uint16_t
	{
		DVAR_FLAG_NONE      = 0x0,
		DVAR_FLAG_SAVED     = 0x1,
		DVAR_FLAG_LATCHED   = 0x2,
		DVAR_FLAG_CHEAT     = 0x4,
		DVAR_FLAG_READONLY  = 0x40,
	};

	// Unified register function pointer. Same backing function handles all
	// scalar types — caller picks which path by the (type) argument and which
	// of the {x,y,z,w} default slots are populated.
	using DvarRegisterFn = dvar_s* (__cdecl*)(
		const char* name, DvarType type, std::uint16_t flags,
		const char* description,
		int x, int y, int z, int w,        // defaults (int reinterpret for ints)
		int min_or_zero, int max_or_zero); // numeric range (int reinterpret)

	// vanilla iw3mp 1.7 (PE 2008-06-19)
	inline constexpr std::uintptr_t kAddr_Dvar_Register     = 0x0056C600;
	inline constexpr std::uintptr_t kAddr_CG_DObjGetWorldBoneMatrix = 0x00433F00;

	inline DvarRegisterFn Dvar_RegisterRaw()
	{
		return reinterpret_cast<DvarRegisterFn>(kAddr_Dvar_Register);
	}

	// Convenience wrappers (match iw3xo signatures from functions.hpp).
	inline dvar_s* Dvar_RegisterInt(const char* name, const char* desc,
		int def, int min, int max, std::uint16_t flags = DVAR_FLAG_SAVED)
	{
		return Dvar_RegisterRaw()(name, DVAR_TYPE_INT, flags, desc,
			def, 0, 0, 0, min, max);
	}

	inline dvar_s* Dvar_RegisterBool(const char* name, const char* desc,
		int def, std::uint16_t flags = DVAR_FLAG_SAVED)
	{
		return Dvar_RegisterRaw()(name, DVAR_TYPE_BOOL, flags, desc,
			def, 0, 0, 0, 0, 0);
	}

	// Float register reinterprets the int slots as float bits.
	inline dvar_s* Dvar_RegisterFloat(const char* name, const char* desc,
		float def, float min, float max, std::uint16_t flags = DVAR_FLAG_SAVED)
	{
		auto f2i = [](float f) -> int { int i; std::memcpy(&i, &f, 4); return i; };
		return Dvar_RegisterRaw()(name, DVAR_TYPE_FLOAT, flags, desc,
			f2i(def), 0, 0, 0, f2i(min), f2i(max));
	}

	// dvar field accessors — current.integer at offset 12, current.value at
	// offset 12 (union). Confirmed equal to iw3xo dvar_s layout.
	inline int read_dvar_int(dvar_s* d)
	{
		if (!d) return 0;
		return *reinterpret_cast<const int*>(reinterpret_cast<const std::uint8_t*>(d) + 12);
	}
	inline float read_dvar_float(dvar_s* d)
	{
		if (!d) return 0.0f;
		return *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(d) + 12);
	}
}
