// cod4mirror — first-person FX / tag mirroring (Phase 4).
//
// Hooks CG_DObjGetWorldBoneMatrix (0x00433F00) and reflects every
// world-space tag matrix that was queried against &cgs->viewModelPose.
// This is the iw3xo v32 "tag_mirror" port: brass/casing ejection,
// muzzle flash, dynamic light attach — anything that reads a view-bound
// tag position via this engine routine — gets mirrored across the camera
// right axis so it visually matches the RTT-mirrored viewmodel.
//
// World entities (other players, vehicles, etc.) also flow through the
// same function. They are filtered out by checking pose ==
// &cgs->viewModelPose. World tag results are returned unchanged.
//
// The install pattern is the same as iw3xo: copy the original 8-byte
// prologue (sub esp,0x30; push ebx; mov ebx,[esp+0x38]) into a runtime
// trampoline, then patch the original function with a JMP to our naked
// stub. The stub marshals the engine's __usercall convention into a
// __cdecl call to tag_replacement, which calls the original via the
// trampoline and then post-processes the outputs.
#pragma once

namespace cod4mirror::fx_mirror
{
	// Install the inline JMP hook at 0x00433F00. Idempotent — safe to
	// call multiple times; only the first call patches the function.
	// Returns true if the hook is now active (or was already active),
	// false if memory allocation / page protection failed.
	bool install();
}
