# cod4mirror

Proxy `d3d9.dll` для **CoD4 1.7 MP** (`iw3mp.exe`, ванильный, без iw3xo / CoD4X).
Цель — портировать mirror-viewmodel и full-screen mirror функциональность из iw3xo в standalone DLL который кладётся в папку с игрой.

## Статус: Phase 1 (scaffold)

На сейчас собирается чистый passthrough proxy DLL:

- ✓ DllMain загружает настоящий `C:\Windows\System32\d3d9.dll`
- ✓ Все экспорты d3d9 (Direct3DCreate9, D3DPERF_*, DebugSetLevel, …) форвардятся
- ✓ `IDirect3D9` и `IDirect3DDevice9` обёрнуты в wrapper-классы (passthrough)
- ✗ Mirror logic ещё не подключена — игра запускается **визуально как ванильная**

После того как Phase 1 проверен на твоей машине (запускается, нет крашей, FPS не упал) — Phase 2/3/4 добавят hooks, dvar-ы, и mirror-логику без изменения структуры этого DLL.

Phase порядок:

| phase | что добавляется |
|-------|-----------------|
| 1 | scaffold: proxy + wrapper классы (passthrough) — игра запускается без изменений |
| 2 | хуки на engine functions (`Dvar_RegisterInt` 0x56C600, регистрация dvar-ов, console output) |
| 3 | RTT mirror viewmodel + tonemap-source injection (порт `mirror_rtt` из iw3xo) |
| 4 | `CG_DObjGetWorldBoneMatrix` хук (FX mirror) + `r_fullMirror` (mode 1/2) |

## Как собрать

Требуется: Visual Studio 2017/2019/2022 + [premake5](https://premake.github.io/download).

```
cd cod4mirror
premake5 vs2022
```

Откроется `build\cod4mirror.sln`. В Visual Studio:

1. Configuration: `Release`
2. Platform: `Win32` (важно — игра 32-битная)
3. Build → Build Solution (Ctrl+Shift+B)

Результат: `build\bin\Win32\Release\d3d9.dll` (~30-50 KB на Phase 1).

## Как установить

1. Найти папку с `iw3mp.exe` (например `C:\Games\Call of Duty 4\`).
2. **Backup** существующего `d3d9.dll` если он там есть (на ванильной установке его нет — это нормально).
3. Скопировать собранный `d3d9.dll` в эту папку (рядом с `iw3mp.exe`).
4. Запустить `iw3mp.exe`.

## Как удалить

Удалить (или переименовать) `d3d9.dll` из папки с игрой. Игра автоматически возьмёт системный `C:\Windows\System32\d3d9.dll`.

## Phase 1 verification

Скорее всего ничего видимого не должно произойти — игра запускается, играется как обычно. Чтобы убедиться что наш proxy реально загружен:

- Поставь breakpoint / Sleep в `DllMain` и убедись что иконка процесса показывает `d3d9.dll` рядом с iw3mp.exe в Process Explorer
- Или временно добавь `MessageBoxA(0, "cod4mirror loaded", "test", 0);` в `DllMain` `DLL_PROCESS_ATTACH`

Если игра не запускается / краш на старте на Phase 1 — это значит что либо name-mangling экспортов сломан (проверь .def файл), либо неверная архитектура (должно быть Win32, не x64).

## Архитектура

```
iw3mp.exe                  ← запуск
   │ LoadLibrary("d3d9.dll")
   ▼
cod4mirror d3d9.dll        ← наш proxy
   │ LoadLibrary("C:\Windows\System32\d3d9.dll")
   ▼
Real d3d9.dll              ← создаёт настоящие D3D объекты

iw3mp.exe → Direct3DCreate9() → cod4mirror::Direct3DCreate9
                                   → real Direct3DCreate9
                                   → real IDirect3D9*
                                   → wrap в cod4mirror::D3D9Object
                                   → return wrapper

iw3mp.exe → IDirect3D9::CreateDevice() → D3D9Object::CreateDevice
                                            → real CreateDevice
                                            → real IDirect3DDevice9*
                                            → wrap в cod4mirror::D3D9Device
                                            → return wrapper

iw3mp.exe → device->Present() → D3D9Device::Present
                                  → m_orig->Present()    (passthrough в Phase 1)
                                  → (в Phase 3 здесь добавится mirror composite)
```

## Файлы

- `src/dllmain.cpp` — DllMain + 13 экспортов d3d9
- `src/d3d9_object.h/.cpp` — IDirect3D9 wrapper, override CreateDevice
- `src/d3d9_device.h/.cpp` — IDirect3DDevice9 wrapper, 119 методов passthrough
- `src/d3d9.def` — список экспортов для линкера (без mangling)
- `premake5.lua` — конфиг сборки

## Целевой бинарь iw3mp.exe

Известные офсеты в ванильной `iw3mp.exe` 1.7 (PE timestamp 2008-06-19):

| функция | адрес | пролог |
|---------|-------|--------|
| `Dvar_RegisterInt` | `0x0056C600` | — |
| `CG_DObjGetWorldBoneMatrix` | `0x00433F00` | `83 EC 30 53 8B 5C 24 38` (8 байт) |
| `cgs->refdef.vieworg` | TBD (Phase 3) | — |
| `cgs->refdef.viewaxis[0..2]` | TBD (Phase 3) | — |
| `cgs->viewModelPose` | TBD (Phase 3) | — |

Подтверждены через `objdump`/strings на iw3mp.exe приложенном пользователем.

## Лицензия

TBD — внутренний инструмент. Не для распространения без согласования.
