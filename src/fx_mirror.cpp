// cod4mirror — first-person tag mirror (Phase 4).
//
// See fx_mirror.h for the design overview. This file is a near-direct
// port of iw3xo's tag_mirror namespace from src/components/modules/_renderer.cpp
// (v31-v32 final), retargeted at vanilla iw3mp.exe (CoD4 1.7 MP).
//
// Major differences vs. iw3xo:
//   * No iw3xo dvars / utilities — we read mirror config via cod4mirror
//     dvar handles owned by mirror.cpp, exported through accessors.
//   * Engine bindings live in engine.h (cgs base, &viewModelPose, refdef
//     vieworg/viewaxis pointers — verified by binary scan of iw3mp.exe).
//   * No cg_s struct mirror — we read refdef.vieworg / viewaxis / pose via
//     fixed absolute addresses, which avoids dragging in iw3xo's structs.hpp.
#include "fx_mirror.h"
#include "engine.h"
#include "logger.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// Mirror config accessors — implemented in mirror.cpp so all dvar
// state lives in one place. Forward-declared here at file scope (with C
// linkage) so the linker resolves them across translation units.
extern "C" int   fx_mirror_enabled();    // r_mirrorViewmodel_mirrorFx
extern "C" int   fx_mirror_axis_mode();  // r_mirrorViewmodel_mirrorFxAxis (0/1/2)
extern "C" float fx_mirror_dist();       // r_mirrorViewmodel_mirrorFxDist (units, default 64)
extern "C" int   fx_mirror_log_left();   // r_mirrorViewmodel_mirrorFxLog
extern "C" void  fx_mirror_log_dec();
extern "C" int   fx_mirror_axis_idx();   // r_mirrorViewmodel_mirrorFxAxisIdx (0/1/2, default 1)
extern "C" int   fx_mirror_active();     // (rtt || full) ? 1 : 0

namespace cod4mirror::fx_mirror
{
	namespace
	{
		static unsigned char* g_trampoline = nullptr;

		// Recursion-safe replacement. Stack-frame state isolates nested calls
		// (parent-bone recursion or engine re-entry).
		extern "C" int __cdecl tag_replacement(void* pose, int bone_index,
			float* axis, void* obj, float* origin)
		{
			int result = 0;
			void* trampoline = g_trampoline;

			// Call the original via the trampoline using its __usercall:
			//   eax=pose, ecx=bone_index, esi=axis, [esp+0]=obj, [esp+4]=origin
			// (the function does `add esp, 8` itself, but this trampoline
			// simply runs the copied prologue then JMPs to the ORIGINAL+8,
			// so the engine cleans up the same way the regular call site
			// would have. We set up the args identically to a normal call.)
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
			if (!fx_mirror_enabled()) return result;
			if (!fx_mirror_active())  return result;

			// Filter: only mirror tag results queried against the viewmodel
			// pose. World-entity tag queries flow through here too and must
			// be left alone.
			if (pose != engine::viewModelPose()) return result;

			const float* vorg = engine::refdef_vieworg();
			const float* va_flat = engine::refdef_viewaxis();
			const float (*va)[3] = reinterpret_cast<const float(*)[3]>(va_flat);

			// Pick which view-axis row defines the mirror plane normal.
			// Reflection is symmetric in n vs -n, so axis[1] and -axis[1]
			// both work. Default is 1 (camera left/right). 0 / 2 are
			// available for A/B testing if the live viewaxis convention
			// differs from iw3xo's.
			int axis_idx = fx_mirror_axis_idx();
			if (axis_idx < 0 || axis_idx > 2) axis_idx = 1;
			const float* mirror_n = va[axis_idx];

			// Distance gate: only mirror tags within mirrorFxDist of the
			// camera. Viewmodel tags are within ~30-50 units; world FX in
			// melee range can be ~50-100 units, so the default (64) keeps
			// us scoped to the viewmodel pose by physical proximity even
			// before the pose-pointer filter. This is a defense-in-depth
			// guard in case some engine path calls the function with the
			// viewmodel pose pointer for a non-viewmodel bone.
			const float maxd = fx_mirror_dist();
			if (origin)
			{
				const float dx0 = origin[0] - vorg[0];
				const float dy0 = origin[1] - vorg[1];
				const float dz0 = origin[2] - vorg[2];
				const float d2  = dx0*dx0 + dy0*dy0 + dz0*dz0;
				if (d2 > maxd * maxd) return result;
			}

			float orig_in[3] = {0.0f, 0.0f, 0.0f};
			if (origin) { orig_in[0] = origin[0]; orig_in[1] = origin[1]; orig_in[2] = origin[2]; }

			// Reflect origin about plane(vorg, mirror_n).
			//   off       = origin - vorg
			//   off_refl  = off - 2 * dot(off, n) * n
			//   origin'   = vorg + off_refl
			if (origin)
			{
				const float dx = origin[0] - vorg[0];
				const float dy = origin[1] - vorg[1];
				const float dz = origin[2] - vorg[2];
				const float dot = dx * mirror_n[0] + dy * mirror_n[1] + dz * mirror_n[2];
				origin[0] -= 2.0f * dot * mirror_n[0];
				origin[1] -= 2.0f * dot * mirror_n[1];
				origin[2] -= 2.0f * dot * mirror_n[2];
			}

			// Three axis-mirror modes (matches iw3xo v31-v32):
			//   0 - origin only, axis untouched. Brass spawns at the
			//       mirrored position but flies in the original direction
			//       — looks like the casing exits the wrong side and
			//       drifts.
			//   1 - reflect all three axis rows. Mathematically a true
			//       mirror but produces a left-handed basis (det=-1);
			//       AxisToAngles assumes right-handedness and crashes on
			//       improper rotations.
			//   2 - "RH-mirror" (default): reflect rows 0 and 2, reflect-
			//       AND-negate row 1. Equivalent to reflect ∘ Rot(180° about
			//       row1); preserves det=+1, so AxisToAngles is happy.
			//       axis[0] (forward / brass-eject dir) and axis[2] (up)
			//       are correctly mirrored, axis[1] (side) is rotated 180°
			//       which is harmless for symmetric brass.
			const int mode = fx_mirror_axis_mode();
			if (axis && mode == 1)
			{
				for (int r = 0; r < 3; ++r)
				{
					float* row = axis + r * 3;
					const float dot = row[0] * mirror_n[0] + row[1] * mirror_n[1] + row[2] * mirror_n[2];
					row[0] -= 2.0f * dot * mirror_n[0];
					row[1] -= 2.0f * dot * mirror_n[1];
					row[2] -= 2.0f * dot * mirror_n[2];
				}
			}
			else if (axis && mode == 2)
			{
				// rows 0 and 2: plain reflect.
				for (int r = 0; r < 3; r += 2)
				{
					float* row = axis + r * 3;
					const float dot = row[0] * mirror_n[0] + row[1] * mirror_n[1] + row[2] * mirror_n[2];
					row[0] -= 2.0f * dot * mirror_n[0];
					row[1] -= 2.0f * dot * mirror_n[1];
					row[2] -= 2.0f * dot * mirror_n[2];
				}
				// row 1: reflect AND negate (= -reflect(row1)).
				float* row1 = axis + 3;
				const float dot = row1[0] * mirror_n[0] + row1[1] * mirror_n[1] + row1[2] * mirror_n[2];
				row1[0] = -(row1[0] - 2.0f * dot * mirror_n[0]);
				row1[1] = -(row1[1] - 2.0f * dot * mirror_n[1]);
				row1[2] = -(row1[2] - 2.0f * dot * mirror_n[2]);
			}

			// Optional one-shot diagnostic (decrements a dvar each line so
			// the user can dump N reflections via /r_mirrorViewmodel_mirrorFxLog N).
			const int log_left = fx_mirror_log_left();
			if (log_left > 0 && origin)
			{
				fx_mirror_log_dec();
				log::line("[fx_mirror] bone=%d  origin_in=(%.1f %.1f %.1f) -> "
					"refl=(%.1f %.1f %.1f)  mode=%d axis_idx=%d",
					bone_index,
					orig_in[0], orig_in[1], orig_in[2],
					origin[0], origin[1], origin[2],
					mode, axis_idx);
			}

			return result;
		}

		// Naked stub installed at 0x00433F00. Caller's __usercall:
		//   eax = pose, ecx = bone_index, esi = axis (out, 3x3),
		//   [esp+0] = ret addr, [esp+4] = obj, [esp+8] = origin (out, vec3)
		// Caller does `add esp, 8` after the call.
		__declspec(naked) void getbonematrix_stub()
		{
			__asm
			{
				// Push __cdecl args in reverse order. Stack layout at entry:
				//   [esp+0]=ret, [esp+4]=obj, [esp+8]=origin
				push    [esp + 8];   // origin
				push    [esp + 8];   // obj   (was at +4, now +8 after one push)
				push    esi;         // axis
				push    ecx;         // bone_index
				push    eax;         // pose
				call    tag_replacement;
				add     esp, 20;     // clean our 5 pushed args
				ret     0;           // return; caller cleans its 2 stack args
			}
		}
	}

	bool install()
	{
		if (g_trampoline) return true; // already installed

		const std::uintptr_t target = engine::kAddr_CG_DObjGetWorldBoneMatrix;

		// Prologue at 0x433F00 is exactly:
		//   83 EC 30           sub  esp, 0x30
		//   53                 push ebx
		//   8B 5C 24 38        mov  ebx, [esp+0x38]
		// = 8 bytes, 3 complete instructions. Copying only 5 bytes (the
		// minimum needed to fit a JMP rel32) would split `mov ebx, [esp+0x38]`
		// across the trampoline boundary and crash on first call. We must
		// copy 8 bytes and JMP to target+8.
		constexpr std::size_t kPrologueBytes = 8;
		constexpr std::size_t kTrampolineBytes = kPrologueBytes + 5; // + JMP rel32

		g_trampoline = static_cast<unsigned char*>(VirtualAlloc(
			nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!g_trampoline)
		{
			log::line("[fx_mirror] VirtualAlloc failed (err=%lu) — hook NOT installed",
				GetLastError());
			return false;
		}

		// Copy the original prologue.
		std::memcpy(g_trampoline,
			reinterpret_cast<const void*>(target),
			kPrologueBytes);

		// Append JMP rel32 -> target + kPrologueBytes.
		const std::intptr_t jmp_from = reinterpret_cast<std::intptr_t>(g_trampoline) + kPrologueBytes;
		const std::intptr_t jmp_to   = static_cast<std::intptr_t>(target + kPrologueBytes);
		g_trampoline[kPrologueBytes] = 0xE9;
		*reinterpret_cast<std::int32_t*>(g_trampoline + kPrologueBytes + 1) =
			static_cast<std::int32_t>(jmp_to - (jmp_from + 5));

		FlushInstructionCache(GetCurrentProcess(), g_trampoline, 32);

		// Patch the original function with `JMP rel32 -> getbonematrix_stub`.
		// First 5 bytes of the original become E9 + rel32; the remaining 3
		// bytes of the original prologue become dead but unreachable, so
		// they're harmless.
		DWORD old_protect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(target), 5,
			PAGE_EXECUTE_READWRITE, &old_protect))
		{
			log::line("[fx_mirror] VirtualProtect failed (err=%lu) — hook NOT installed",
				GetLastError());
			VirtualFree(g_trampoline, 0, MEM_RELEASE);
			g_trampoline = nullptr;
			return false;
		}

		auto* p = reinterpret_cast<unsigned char*>(target);
		const std::intptr_t patch_jmp_to = reinterpret_cast<std::intptr_t>(&getbonematrix_stub);
		p[0] = 0xE9;
		*reinterpret_cast<std::int32_t*>(p + 1) =
			static_cast<std::int32_t>(patch_jmp_to - (static_cast<std::intptr_t>(target) + 5));

		DWORD dummy = 0;
		VirtualProtect(reinterpret_cast<void*>(target), 5, old_protect, &dummy);
		FlushInstructionCache(GetCurrentProcess(),
			reinterpret_cast<void*>(target), 5);

		log::line("[fx_mirror] hook installed: 0x%08X -> stub @ 0x%p (trampoline @ 0x%p)",
			(unsigned)target, &getbonematrix_stub, g_trampoline);
		return true;
	}
}
