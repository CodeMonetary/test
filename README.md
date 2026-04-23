# cod4-worldflip

A `d3d9.dll` proxy/wrapper for **Call of Duty 4: Modern Warfare (2007)** multiplayer client
(`iw3mp.exe`) that horizontally mirrors the **3D world view only**, while leaving the
**view-model (weapon + hands)** and the **2D HUD** rendered in their normal orientation.

Works with:
- Vanilla CoD4 1.7 (stock `iw3mp.exe`).
- CoD4X (any current patch) — runs at the D3D9 layer, so the CoD4X client patches don't interfere.
- Compatible with demo replays (`demo <name>`) on both builds — the flip is applied at the
  rendering layer, independent of the game's demo / netcode logic.

## How it works

CoD4's renderer issues per-draw state through D3D9. Three rendering passes are relevant:

| Pass          | Detection                                     | Flip behavior |
|---------------|-----------------------------------------------|---------------|
| World (3D)    | perspective projection, full depth range      | **flipped**   |
| View-model    | perspective projection, shrunk depth range (`MinZ..MaxZ` not `0..1`) | unflipped |
| 2D / HUD      | orthographic projection (`m[3][3] == 1`)      | unflipped     |

The proxy hooks the device methods it cares about
(`SetViewport`, `SetVertexShaderConstantF`, `SetRenderState`, `Draw*`) and:

1. Detects the current pass from viewport depth range + projection matrix shape.
2. On "world" passes: negates column 0 of the projection matrix before it reaches the
   vertex shader (`m[0][0] *= -1`, `m[1][0] *= -1`, `m[2][0] *= -1`, `m[3][0] *= -1`)
   and swaps the current `D3DRS_CULLMODE` (CW↔CCW) so triangle winding stays correct.
3. On "view-model" and "2D" passes: leaves matrices and cull state untouched.

Everything else (sky, particles, FX, decals, shadows, post-processing) inherits the world
projection automatically, so no per-effect code is needed.

## Install

1. Copy `d3d9.dll` from `dist/` into the same directory as `iw3mp.exe`
   (typically `C:\Program Files (x86)\Activision\Call of Duty 4 - Modern Warfare\`).
2. Optional: copy `cod4-worldflip.ini` next to it and edit it to taste.
3. Launch `iw3mp.exe` normally (or through CoD4X launcher). On load, a log file
   `cod4-worldflip.log` is written next to `iw3mp.exe` — useful if something goes wrong.

To **uninstall**: delete `d3d9.dll`. The game will fall back to the system d3d9.dll.

## Config (`cod4-worldflip.ini`)

```ini
[worldflip]
; Master enable. If 0 the DLL is pure pass-through (zero overhead).
enabled = 1

; Toggle hotkey (virtual-key code, hex). Default = F11 (0x7A).
; Common: 0x7A=F11, 0x7B=F12, 0x2D=Insert, 0x76=F7
toggle_vk = 0x7A

; Show an on-screen debug banner ("WORLDFLIP ON" + pass counters).
; Useful for verifying that pass detection is correct.
debug_banner = 0

; Write a log file with per-second pass statistics.
log = 1
```

## Building from source (Linux / mingw-w64)

```bash
sudo apt-get install -y gcc-mingw-w64-i686 g++-mingw-w64-i686
make
# produces dist/d3d9.dll
```

Or on Windows with MSVC, use `cl.exe` — see `src/*` for the translation-unit list.

## Known limitations

- Crosshair rendering (if it is a screen-space 2D element via the HUD pass): unaffected.
- If you run the game at a heavily modded resolution or with ENB/ReShade already installed,
  chain the DLLs (rename one of them) — two `d3d9.dll`s can't coexist in the same folder.
- Shadow maps inherit the flipped projection automatically; no separate handling.
