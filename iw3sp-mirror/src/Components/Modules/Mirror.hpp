// Mirror.hpp — port of iw3xo v38.2 mirror functionality to iw3sp.exe.
//
// Self-contained header. Only depends on:
//   * IDirect3DDevice9 / d3d9.h / d3dx9 (for D3DXAssembleShader)
//   * Dvars::r_*               (registered in Dvars.cpp / CommonPatch.cpp)
//   * Dvars::Functions::Dvar_FindVar  (already exposed by Dvars.hpp)
//   * Game::Com_PrintMessage   (already exposed by Functions.hpp)
//
// Public API:
//   * Mirror::BeginScene(dev)
//   * Mirror::EndScene(dev)
//   * Mirror::Reset()
//   * Mirror::Present()
//   * Mirror::SetVertexShaderConstantF(dev, StartRegister, pConstantData, Vector4fCount, /*out*/ &outData)
//   * Mirror::SetPixelShaderConstantF(dev, StartRegister, pConstantData, Vector4fCount)
//   * Mirror::OnDrawPrimitive_Post(dev)
//   * Mirror::OnDrawIndexedPrimitive_Post(dev)
//   * Mirror::OnSetRenderState(dev, State, Value)  // observe HUD-start
//
// Stage A scope: r_fullMirror, r_fullMirrorDepth, r_mirrorViewmodel_rtt (+ knobs).
//   Stage B (FX mirror) and Stage C (HUD mirror) are intentionally NOT in this file.
#pragma once

#include <d3d9.h>

namespace Game { struct usercmd_s; }

namespace Mirror
{
	// ----- per-frame hooks -----
	void OnBeginScene(IDirect3DDevice9* dev);
	void OnEndScene(IDirect3DDevice9* dev);
	void OnReset();
	void OnPresent();

	// ----- input mirror hooks (r_mirrorInput) -----
	//   OnMouseDelta : called from RawMouse::CL_MouseEvent BEFORE the
	//                  delta is accumulated into mouseDx/mouseDy.  When
	//                  r_mirrorInput == 1, dx is negated in-place so
	//                  right mouse motion produces left yaw rotation.
	void OnMouseDelta(int* dx, int* dy);
	//   OnUserCmd    : called from Gamepad::CL_MouseMove AFTER the
	//                  engine has populated the usercmd_s for this
	//                  frame.  When r_mirrorInput == 1, cmd->rightmove
	//                  is negated so the A/D strafe bindings swap
	//                  (A walks right, D walks left).  forwardmove,
	//                  upmove, pitchmove, yawmove are untouched.
	void OnUserCmd(Game::usercmd_s* cmd);

	// ----- constant-upload hooks -----
	//   pConstantData     : the values the engine wants to upload.
	//   *outConstantData  : (out) pointer to use for the actual upload.
	//                       Returns either pConstantData (no change) or
	//                       an internal scratch buffer (when matrix-flip
	//                       mode is enabled). Lifetime: until next call.
	void OnSetVertexShaderConstantF(IDirect3DDevice9* dev,
		UINT StartRegister, const float* pConstantData, UINT Vector4fCount,
		const float** outConstantData);

	// Called BEFORE the actual SetPixelShaderConstantF passthrough.
	void OnSetPixelShaderConstantF(IDirect3DDevice9* dev,
		UINT StartRegister, const float* pConstantData, UINT Vector4fCount);

	// ----- draw hooks -----
	//   Called AFTER the underlying DrawPrimitive / DrawIndexedPrimitive
	//   returns. Used to flush deferred composites and fullscreen flips.
	void OnDrawPrimitive_Post(IDirect3DDevice9* dev);
	void OnDrawIndexedPrimitive_Post(IDirect3DDevice9* dev);

	// Called from SetRenderState BEFORE forwarding. Used to detect the
	// HUD-start 4-state signature (CULL=1, ABE=1, SB=2, ATE 0->1) and
	// trigger blur-gated fullscreen flip / depth flip.
	void OnSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE State, DWORD Value);

	// ----- dvar registration helper -----
	//   Called once from CommonPatch::OnDvarInit. Registers every
	//   mirror-related dvar. Safe to call multiple times (Dvar_RegisterX
	//   handles re-registration gracefully).
	void RegisterDvars();
}
