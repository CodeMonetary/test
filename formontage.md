# iw3x.dll — гайд для монтажа (актуальная версия v38.2)

Краткий справочник по mirror-фичам `iw3x.dll` для записи и монтажа фрагов в CoD4 1.7 MP. Включает все актуальные dvar'ы (v33 depth-fix, v35 HUD-mirror, **v36 r_fullMirrorDepth**, **v37.2 filmtweak-aware tonemap detect**, **v38.2 r_blur ghost fix**) и рекомендованные пресеты.

---

## 1. Что такое `iw3x.dll`

Сторонний клиентский DLL для CoD4 1.7 MP (`iw3mp.exe`). Грузится через лоадер (`iw3xo-mp.exe` / `iw3xo.exe`), хукает D3D9 и часть функций движка. **Никакие игровые файлы не модифицируются** — всё через runtime-хуки и включается dvar'ами на лету.

Архитектурно DLL = **proxy `d3d9.dll`** + хуки на отдельные функции движка. Загружается вместо системного `d3d9.dll`, перехватывает `IDirect3DDevice9` методы (DrawPrimitive, EndScene, SetPixelShaderConstantF и т.д.) — отсюда возможность ловить tonemap-pass, HUD-pass, гун-pass по сигнатурам shader-constant'ов и менять рендер на лету.

### Что умеет (для монтажа важно знать)

- **Mirror viewmodel** — оружие в левой руке, через render-to-texture (без инверсии нормалей и cull, гун выглядит правильно).
- **Mirror FX** — гильзы, muzzleflash, трассеры зеркалятся вместе с оружием.
- **r_fullMirror 1/2** — полный flip всего кадра (мир + гун, либо мир + гун + HUD).
- **r_hudMirror (v35)** — отдельное зеркалирование HUD для совмещения с ReShade Flip.fx.
- **r_fullMirrorDepth (v36)** — флип main depth-stencil после r_fullMirror, чтобы ReShade MXAO/SSAO не рисовал ghost-тени гуна на стене.
- **r_mirrorViewmodel_depthFix (v33)** — точечный depth-fix для гуна (тоже про MXAO).
- **filmtweak-aware tonemap detect (v37.2, прозрачно)** — гун следует за `r_filmTweakBrightness/Contrast/Desaturation`, `r_contrast`, `r_desaturation` на любых значениях. Нет dvar'а — работает само.
- **r_blur ghost fix (v38.2, прозрачно)** — флип BB и captura HUD теперь срабатывают ПОСЛЕ блюр-композита движка, а не посередине. Раньше при `r_blur 1` + `r_fullMirror 1` (или `r_hudMirror 1`) был "призрак" поверх флипа. Нет dvar'а — работает само.
- Прочее (для справки): map exporting, day/night cycle, RTX Remix support, custom movement, devgui (`/devgui`).

Подробности и release-сборки upstream: https://github.com/xoxor4d/iw3xo-dev (этот форк = upstream + патчи v32–v38.2).

---

## 2. TL;DR — пресеты для монтажа

### A. Леворукий геймплей (без эффекта "перевёрнутой камеры")
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
```
Оружие, руки, гильзы, muzzleflash, трассеры — всё на левой стороне. Мир и HUD — нормальные.

### B. "Перевёрнутая камера" + читаемый HUD (рекомендация для нарезок)
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
/r_fullMirror 1
/r_fullMirrorDepth 1            // default; включает фикс ghost-MXAO
```
Мир и FX отражены, оружие за счёт двойного flip оказывается визуально **справа**, HUD остаётся в нормальной ориентации. Идеально для эффекта "записал нормально, смонтировал зеркально" без потери информации с UI.

> С ReShade MXAO/SSAO/RTGI **обязательно** держать `r_fullMirrorDepth 1` (это default), иначе на стенах будут плавать "призрачные" тени гуна и геометрии в зеркально-неправильных позициях. Без ReShade `r_fullMirrorDepth` silently no-op'ится — оставлять 1 безопасно всегда.

### C. Зеркало мира + читаемый HUD через ReShade Flip.fx
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
/r_hudMirror 1
```
Используется в связке с ReShade `Flip.fx`: ReShade зеркалит весь финальный кадр, а `r_hudMirror 1` пред-зеркалит HUD внутри движка → на экране HUD читается нормально, мир + гун зеркальные. См. секцию 6 (известное ограничение про killstreak-баннер).

### D. Brute-force — flip всего кадра вместе с HUD
```
/r_fullMirror 2
/r_fullMirrorDepth 1            // default
```
Один горизонтальный flip всего back-buffer'а в EndScene. Самый простой способ. Минус — HUD-цифры будут зеркальными.

### E. С motion-blur (v38.2)
Любой из пресетов A–D **+** включить блюр:
```
/r_blur 1                       // или больше — например 2/3
```
В v38.2 ghost-полоса при `r_blur 1` + `r_fullMirror 1` / `r_hudMirror 1` устранена — флип и HUD-capture теперь срабатывают **после** блюр-композита движка. Никаких дополнительных dvar'ов выставлять не нужно.

### F. Cinematic filmtweak / contrast / desaturation (v37.2)
Любой из пресетов A–D **+** настроить кривую:
```
/r_filmTweakEnable 1
/r_filmTweakBrightness 0.05
/r_filmTweakContrast 1.2
/r_filmTweakDesaturation 0.15
// или (старый стиль)
/r_contrast 1.2
/r_desaturation 0.15
```
В v37.2 PSCF c7 fingerprint расширен + добавлен RGB-gated cache PS-pointer'а → tonemap-pass детектится на любых значениях filmtweak'а, и гун (через `rttTonemapInject 1` default) проходит ту же tonemap-кривую что мир. Никаких dvar'ов из нашей DLL для этого не нужно.

### G. Выключить всё
```
/r_fullMirror 0
/r_fullMirrorDepth 0
/r_hudMirror 0
/r_mirrorViewmodel_rtt 0
/r_mirrorViewmodel_mirrorFx 0
```

---

## 3. Полный список команд по mirror

### 3.1 Базовый пайплайн (mirror viewmodel)

| dvar | range / default | назначение |
|------|---|---|
| `r_mirrorViewmodel_rtt` | 0/1, default 0 | **Главный выключатель.** Рендерит гун в off-screen RT, композ-ит обратно с UV-flip. Корректные нормали/cull. |
| `r_mirrorViewmodel_rttTonemapInject` | 0/1, default **1** | Инжектит зеркальный гун в tonemap-source до финального tonemap-прохода → правильный свет / без "песочного" тинта / следует за filmtweak. Оставлять 1. |
| `r_mirrorViewmodel_rttEarlyComposite` | 0/1/2, default **1** | Когда композ-ить: 0=на EndScene (гун закрывает HUD), 1=pre-HUD (HUD рисуется поверх гана; default), 2=оба (диагностика). |
| `r_mirrorViewmodel_compositeSrgb` | 0..3, default **1** | sRGB флаги при композ-ите. 1 = linear sample → sRGB write (норма). Менять только если видны цветовые артефакты. |
| `r_mirrorViewmodel_rttBlend` | 0..3, default **2** | Blend для композ-итa. 2 = ONE/ONE additive (default). 1/3 = с alpha-test, если шейдер пишет alpha=0. |
| `r_mirrorViewmodel_depthFix` | 0..3, default **2** | v33: чинит ReShade MXAO bleed-through **на гане**. 2 = одна штамп-проход (default). 0 = выкл если не используете depth-reading post-fx. |
| `r_mirrorViewmodel_clearRttDepth` | 0/1, default **1** | v33: очищает off-screen depth в конце кадра, чтобы ReShade Generic Depth не подобрал его и не нарисовал phantom AO в правой исходной позиции гана. |

### 3.2 FX зеркало (гильзы / muzzleflash / трассеры)

| dvar | range / default | назначение |
|------|---|---|
| `r_mirrorViewmodel_mirrorFx` | 0/1, default 0 | Включатель FX-зеркала. Отражает first-person FX в радиусе `mirrorFxDist` от камеры. Мировые FX не трогаются. |
| `r_mirrorViewmodel_mirrorFxAxis` | 0/1/**2** | Как отражать ориентацию tag'а (от этого зависит направление вылета гильзы). **2** = right-handed mirror (рекомендуется). 0 = только origin (гильзы дрейфуют при повороте). 1 = full mirror (left-handed, может крашить `AxisToAngles`, не использовать). |
| `r_mirrorViewmodel_mirrorFxDist` | float, default 64.0 | Радиус (в юнитах) внутри которого FX считается first-person. 64 ловит muzzleflash/гильзы на тэгах вью-модели. |
| `r_mirrorViewmodel_mirrorFxAxisIdx` | 0/1/2, default **1** | Какая ось `viewaxis` — нормаль плоскости отражения. 1 = right (правильно). Не трогать. |
| `r_mirrorViewmodel_mirrorFxLog` | int, default 0 | Залогировать следующие N FX-отражений в консоль (для дебага). |

### 3.3 Полное зеркало картинки

| dvar | range / default | назначение |
|------|---|---|
| `r_fullMirror` | 0/1/2, default 0 | **Полно-экранный mirror.** 1 = flip перед HUD (мир+гун зеркальны, HUD нормальный; в паре с `r_mirrorViewmodel_rtt 1` гун визуально оказывается справа). 2 = flip в EndScene (всё включая HUD). |
| `r_fullMirrorDepth` | 0/1, default **1** | **v36.** Дополнительный flip main depth-stencil после каждого color-flip'а `r_fullMirror`. Без него ReShade MXAO/SSAO считает occlusion на исходной (нефлипнутой) геометрии и накладывает на флипнутый color → видно "призрак" гуна и геометрии на стенах в зеркально-неправильных позициях. С ним AO ложится правильно. No-op без ReShade — безопасно держать `1` всегда. |
| `r_hudMirror` | 0/1, default 0 | **v35.** Отдельное зеркало HUD через capture в HUD-RTT + composite с UV-flip. Дизайн: совмещение с ReShade `Flip.fx`. Не трогает мир/гун/post-FX. См. секцию 6 (known limitation). |

### 3.4 Engine-команды релевантные монтажу (не из нашей DLL, но используются вместе)

| dvar / cmd | примечание |
|------|---|
| `r_blur` | motion-blur. **v38.2 совместим** с `r_fullMirror 1` и `r_hudMirror 1` — ghost-полоса устранена. |
| `r_filmTweakEnable` / `r_filmTweakBrightness` / `r_filmTweakContrast` / `r_filmTweakDesaturation` / `r_filmTweakInvert` / `r_filmTweakDarkTint` / `r_filmTweakLightTint` | cinematic post-FX curve. **v37.2 совместим** с `r_mirrorViewmodel_rtt 1` — гун проходит ту же кривую, "песочного" тинта нет на любых значениях. |
| `r_contrast` / `r_desaturation` | старые контраст/десатурация. То же — v37.2 покрывает. |
| `r_dof_enable` / `r_dof_tweak` | depth-of-field. **НЕ совместим** с `r_mirrorViewmodel_rtt 1` — гун пропадает (см. секцию 6). Держать **0**. |

### 3.5 Старый matrix-flip путь (legacy — не использовать)

До RTT был matrix-flip путь — менял знак столбца projection-матрицы. Производил left-handed геометрию, ломал нормали. Оставлен только для отладки. **Не использовать если включён `r_mirrorViewmodel_rtt 1`.**

| dvar | назначение |
|------|---|
| `r_mirrorViewmodel_method` | Паттерн matrix-flip (0..7). |
| `r_mirrorViewmodel_flipVSCF` | Триггер flip'а на VSCF-аплоаде. |
| `r_mirrorViewmodel_flipFollow` | Сколько последующих matrix uploads тоже флипать. |
| `r_mirrorViewmodel_flipAxis` | Какая 4-флоат группа флипается (0..9). |
| `r_mirrorViewmodel_flipReg` | Какой shader-register начало (0/4/24). |
| `r_mirrorViewmodel_cullFix` | Компенсация инвертированного cull-mode. |

### 3.6 Диагностика / сервисные

| команда / dvar | назначение |
|------|---|
| `r_mirrorViewmodel_log` | 0=off, 1=per-scene, 2=verbose. Включить если что-то сломалось. |
| `/mirror_dump <frames>` | Захватывает N кадров (1..120, default 3) в файл `main/mirror_dump_<timestamp>.txt`. Удобный bind: `/bind F10 "mirror_dump 3"`. |
| `/mirror_func_bytes <hex_addr> [count]` | Печатает первые N (default 32) байт по адресу — для дебага хуков. Пример: `/mirror_func_bytes 0x433F00 32`. |

---

## 4. Workflow для монтажа

### 4.1 Геймплей с оружием в левой руке (без full-mirror)
1. Включить пресет A (см. §2).
2. Записать демо как обычно — оружие, гильзы, вспышка, трассеры на левой стороне.
3. Демки воспроизводятся одинаково с любым набором mirror-dvar'ов: можно записать без mirror и проиграть с mirror.

### 4.2 Эффект "перевёрнутой камеры" с читаемым HUD
1. Воспроизвести демо.
2. Включить пресет B (см. §2).
3. На записи мир будет зеркальный, оружие справа (двойной flip), HUD читаемый.
4. Если используете ReShade с MXAO/SSAO — `r_fullMirrorDepth 1` (default) обязательно: иначе на стенах поплывут призрачные тени.
5. Также рекомендуется включить `r_mirrorViewmodel_depthFix 2` (default) и `r_mirrorViewmodel_clearRttDepth 1` (default) — это пара v33-фиксов, которые убирают phantom AO на гане отдельно от v36.

### 4.3 Связка с ReShade Flip.fx
1. Включить `Flip.fx` в ReShade overlay.
2. В игре — пресет C (см. §2). HUD пред-зеркалится в движке, ReShade флипает всё → HUD нормальный, мир+гун зеркальные.
3. Альтернатива без ReShade: пресет D (`r_fullMirror 2` + `r_fullMirrorDepth 1`) — но HUD будет зеркальным.

### 4.4 Cinematic-look (filmtweak, motion-blur)
1. Включить любой mirror-пресет (A/B/C/D).
2. Дополнить пресет F (filmtweak) и/или пресет E (r_blur). Они независимы — можно вместе.
3. Никаких dvar'ов из нашей DLL дополнительно выставлять не нужно. v37.2 и v38.2 — прозрачные фиксы.

### 4.5 Скриншоты
Команда игры `/screenshot` ловит финальный back-buffer, поэтому работает корректно со всеми режимами mirror (то что вы видите — то и сохраняется).

---

## 5. Как это работает (упрощённо)

**Mirror viewmodel (`r_mirrorViewmodel_rtt 1`).** Гун ловится по сигнатуре `depthHackNearClip` projection-матрицы → переключается RT на off-screen color+depth → гун рисуется туда → на финальном tonemap-проходе (PSCF c7 fingerprint) RT композ-ится в tonemap-source с горизонтально-флипнутыми UV → tonemap pass пишет всё на BB. Геометрия не трогается, нормали/cull остаются правильными.

**FX mirror (`r_mirrorViewmodel_mirrorFx 1`).** Хук на `CG_DObjGetWorldBoneMatrix` (адрес `0x433F00`, 8-байт trampoline, v29). После того как движок прочитал position тэга — отражаем `origin` через плоскость камеры с нормалью = `viewaxis[1]` (right), и `axis` в зависимости от mode (RH-mirror в mode 2 сохраняет det=+1, brass eject правильно зеркалится).

**`r_fullMirror`.** На сигнатуре PSCF c7 ставит `g_pending_fullmirror_flip` → после следующего draw'а (= tonemap output) `StretchRect(BB → flip_tex)` + draw fullscreen quad с UV(1→0) обратно в BB. Mode 1 = до HUD, mode 2 = в EndScene.

**`r_fullMirrorDepth` (v36).** После каждого `do_fullscreen_flip` запускает 2-pass pixel-shader пайплайн который флипает main depth-stencil:
- Pass 1: читает main DSV (как INTZ-текстуру) с зеркальными UV → пишет в scratch INTZ surface.
- Pass 2: читает scratch INTZ с прямыми UV → пишет обратно в main DSV.

Pixel shader ассемблируется в runtime через `D3DXAssembleShader`:
```
ps_2_0
dcl_2d s0
dcl t0.xy
texld r0, t0, s0
mov oC0, r0           ; color masked out via COLORWRITE=0
mov oDepth, r0.x      ; main payload — копия depth-значения
```
ReShade EndScene/Present hook срабатывает после нашего EndScene → MXAO видит уже флипнутую depth и считает occlusion в координатах, совпадающих с видимым (флипнутым) color. No-op safety: если main DSV не INTZ-формата (= ReShade Generic Depth не активен), функция silently возвращает false и depth не трогается.

**`r_hudMirror` (v35).** На том же PSCF c7 → `begin_capture` переключает RT на HUD-RTT (отдельный off-screen color, без DSV). Все HUD-draws пишут в HUD-RTT. В EndScene `composite()` блитит HUD-RTT обратно на BB с горизонтальным UV-flip.

**`r_mirrorViewmodel_depthFix 2` (v33).** Точечный stamp в main DSV: после композ-ита гуна рисует quad на pixel-позициях зеркального гуна (с UV-flip из RTT-маски) и пишет `oDepth = 0` (near-Z). MXAO видит "тут есть occluder" и больше не светит сквозь гун wall-shadow'ом. Дополняет v36 для случая `r_mirrorViewmodel_rtt=1` без `r_fullMirror`.

**`r_mirrorViewmodel_clearRttDepth 1` (v33).** В EndScene делает `Clear(D3DCLEAR_ZBUFFER)` на off-screen RTT depth. ReShade Generic Depth scan'ит все depth-stencil поверхности и автоматически может выбрать наш RTT depth (где лежит ОРИГИНАЛЬНЫЙ нефлипнутый гун) вместо main DSV → phantom AO в правой исходной позиции. Очистка делает этот pick безвредным.

**Filmtweak detect (v37.2, прозрачно).** Tonemap pass движка детектится по PSCF c7 fingerprint (массив из 4-х floats). На дефолтных filmtweak'ах паттерн `(R,G,B,1.0)` или `(-α,-α,-α,β)`. При не-дефолтных filmtweak'ах (`r_filmTweakBrightness/Contrast/Desaturation`, `r_contrast`, `r_desaturation`) c7 меняет форму. v37 расширил fingerprint до структурного теста (`c70==c71==c72` + `-1.5 < c70 < 0` + `0.5 < c73 < 20`). v37.1 дропнул PS-pointer cache (давал ghost на дефолтных значениях). v37.2 вернул кэш, но включает его fallback только когда `R==G==B` — это разделяет tonemap-PS от post-FX-PS (у которых RGB разные). Никакого нового dvar'а — фикс работает автоматически.

**r_blur ghost fix (v38.2, прозрачно).** Engine `r_blur` копит истории кадров в downsample RT'ы 320×180 и композ-ит на BB **после** нашего `do_fullscreen_flip` → "призрак" нефлипнутого мира поверх флипнутого BB. v38 пытался триггерить флип на `D3DRS_ALPHATESTENABLE=TRUE` — но блюр сам дёргает ATE→1→0, поэтому флип фигачил внутри блюра, до композита → ghost остался. v38.2 нашёл уникальный HUD-start сигнал в виде 4-state последовательности `CULLMODE=1 → ALPHABLENDENABLE=1 → SRCBLEND=2 → ALPHATESTENABLE=1 (rising 0→1)`. Эта сигнатура присутствует и при `r_blur=0`, и при `r_blur=1`, но **никогда** не возникает на ATE-rising'ах блюр-композита. `hud_start_detect::observe()` — shadow-state детектор, возвращает `true` один раз за кадр на этой сигнатуре. Используется одновременно для deferred `r_fullMirror` флипа (когда `r_blur > 0`) и для HUD-RTT capture'а (`r_hudMirror 1`). Если HUD-start не пришёл в кадре (меню/консоль) — EndScene fallback флипает в конце.

---

## 6. Известные ограничения

- **`r_dof_tweak 1` / `r_dof_enable 1` (в ADS) — оружие пропадает из виду при `r_mirrorViewmodel_rtt 1`.** Engine DOF — post-FX между tonemap и HUD; читает main DSV для distance-to-focus. v33 depth-fix пишет `z=0` на пиксели гуна (нужно для MXAO/SSAO), DOF интерпретирует это как "очень far from focus" и блюрит пиксели гуна с соседними wall-пикселями до полной невидимости. Workaround `r_mirrorViewmodel_dofWorkaround` (deferred composite после DOF) был отвергнут как ломающий цвет/зеркалирование гуна. **Рекомендация:** для записи/монтажа держать `r_dof_enable 0` и `r_dof_tweak 0`. Если очень нужен DOF в кадре — постобработка в монтаже (Sapphire Defocus Prism / OpticalFlares Bokeh / Resolve Lens Blur и т.д.).
- **`r_hudMirror 1` — жирная обводка у killstreak-баннера и world-space ников.** Известное ограничение текущего билда. Outline-шейдер этих элементов использует `SEPARATEALPHABLENDENABLE` с `srca=INVDESTALPHA, dsta=ZERO` — при рендере в HUD-RTT alpha-канал считается иначе, что визуально утолщает обводку. Все 4 проверенные гипотезы (alpha squaring v35.16, SetRenderState override v35.19, per-draw force v35.20, stencil masking v35.21) опровергнуты диагностикой; рабочий фикс требует инвазивного shader-resource inspection с риском регрессий и был отложен. Остальной HUD зеркалится корректно. **Рекомендация:** для нарезок где видны killstreak-баннеры — использовать пресет D (`r_fullMirror 2`) или пресет B без `r_hudMirror`.
- **Phase 1 (v35.15) исправлено.** Раньше при `r_hudMirror=1` во время damage-flash/death-cam мир флипался ~42 кадра. Сейчас post-FX fullscreen quad детектится по большой RT-текстуре на stage 0 и редиректится напрямую на back-buffer, минуя HUD-RTT.
- **`mirrorFxAxis 1` крашит на стрельбе** — оставлен только для документации, не использовать.
- **Только first-person FX зеркалятся.** Гильзы/вспышки от других игроков в мире не отражаются (и не должны).
- **Демо записанные ДО включения mirror** воспроизводятся правильно — mirror работает на стороне рендера.
- **На некоторых картах с экзотическими post-FX** PSCF c7 fingerprint может пропасть → `r_mirrorViewmodel_rttEarlyComposite` фолбэкается на EndScene-композ (HUD будет под ганом).
- **`r_fullMirrorDepth` зависит от ReShade Generic Depth.** Если ReShade выключен / Generic Depth не активен / драйвер не поддерживает FOURCC INTZ — функция silently no-op'ится. То есть фикс работает только когда он реально нужен (для MXAO/SSAO/RTGI). Без ReShade ничего не сломается.

---

## 7. История патчей mirror-фичи

| ver | что добавлено |
|-----|---|
| v15 | базовый RTT mirror viewmodel |
| v20 | early composite через PSCF c7 fingerprint (HUD над ганом) |
| v23 | обобщённая PSCF fingerprint (живая игра + демо replay) |
| v24 | sRGB write-encode (фикс "песочного" тинта) |
| v25 | tonemap-source injection (правильный свет) |
| v26 | хук `CG_DObjGetWorldBoneMatrix` для FX mirror |
| v29 | 8-байт trampoline (фикс краша на стрельбе) |
| v31 | RH-mirror axis mode 2 (фикс дрейфа гильз при повороте) |
| v32 | `r_fullMirror` для полного flip-а кадра |
| v33 | `r_mirrorViewmodel_depthFix`, `r_mirrorViewmodel_clearRttDepth` (фиксы ReShade MXAO/SSAO **на гане**) |
| v34 | первая версия `r_hudMirror` (matrix-flip; снят как нечистый) |
| v35.x | `r_hudMirror` через HUD-RTT capture + composite (текущая реализация) |
| v35.15 | Фикс damage/death world-flip: post-FX fullscreen quad escape на BB по большой RT-текстуре |
| v35.22 | Cleanup — убрана диагностика, оставлены только рабочие v35.15 + базовые счётчики. Phase 2 (bold outline) принят как known limitation. |
| v36 | `r_fullMirrorDepth` — флип main DSV после `r_fullMirror`, фикс ghost MXAO/SSAO **на зеркальном мире** через 2-pass ps_2_0 pipeline + scratch INTZ. No-op без ReShade Generic Depth. |
| v37 | Расширенный PSCF c7 fingerprint — гун следует за filmtweak. Дал ghost на дефолтных значениях. |
| v37.1 | Дропнут PS-pointer cache (источник ghost'а v37). Гун снова не следовал за filmtweak. |
| **v37.2** | Возвращён PS-pointer cache с RGB-equality gate (`R==G==B`): cache fallback срабатывает только для tonemap-PS (у которого RGB одинаковые), не для post-FX-PS. Гун следует за filmtweak / contrast / desaturation на любых значениях. |
| v38 | Попытка отложить flip после блюр-композита по триггеру `ALPHATESTENABLE=TRUE`. Не сработала — блюр сам дёргает ATE. |
| v38.1 | Расширенный SRS-лог для поиска уникального HUD-start сигнала (`r_mirrorViewmodel_logBlur`, диагностика, удалена в финале). |
| **v38.2** | HUD-start сигнатура `CULLMODE=1 + ABE=1 + SB=2 + ATE 0→1`. Deferred `r_fullMirror` флип и `r_hudMirror` capture теперь срабатывают **после** блюр-композита. Ghost устранён в обоих сценариях. |

---

## 8. Прочие полезные команды DLL (не mirror, но пригодится)

| команда | назначение |
|---|---|
| `/devgui` | dev GUI (RTX, движок, и пр.). Suggested bind: `/bind F5 devgui`. |
| `/condump` | сохраняет содержимое консоли в `root/iw3xo/condump/`. |
| `/help` | открывает https://xoxor4d.github.io/projects/iw3xo/#in-depth |
| `/iw3xo_github` | открывает upstream-репозиторий. |
| `/menu_open <name>` | открыть меню по имени. |
| `/dump_modes` | (RTX) дамп режимов. |
| `/dumpreflections` | дамп reflection probe текстур текущей карты в `root/iw3xo/reflection_cubes/`. |
| `/mapexport`, `/mapexport_selectionAdd`, `/mapexport_selectionClear` | экспорт коллизий карт в формат Radiant. |
| `/menu_export <name> [subdir]`, `/menu_export_itemdefs <name> [subdir]`, `/menu_list` | экспорт UI-меню в `root/menu_export/`. |
| `/radiant_saveSelection`, `/radiant_clearSaved` | live-link с Radiant editor'ом. |
| `/pm_preset_stock`, `/pm_preset_q3`, `/pm_preset_cs` | пресеты движения (stock CoD4 / Quake3 / Counter-Strike). |
| `/rtx_rebuild_world`, `/rtx_rebuild_all` | (RTX) пересборка fixed-function vertex buffer'ов. |
| `/mapsettings_update`, `/mapsettings_get_defaults` | (RTX) перезагрузить `map_settings.ini` / получить дефолты. |
| `/mainmenu_reload`, `/mainmenu_reload_changelog` | перезагрузка main menu zone. |

---

## 9. Если что-то сломалось — диагностика

1. **Гун невидимый / black** — проверь `r_mirrorViewmodel_rttBlend` (попробуй 1 или 3 = с alpha-test). Если включён `r_dof_tweak` или `r_dof_enable` (в ADS) — отключи их, см. секцию 6.
2. **Гун с "песочным" тинтом** — `r_mirrorViewmodel_rttTonemapInject 1` и `r_mirrorViewmodel_compositeSrgb 1` (default).
3. **Гильзы летят не туда** — `r_mirrorViewmodel_mirrorFxAxis 2` (RH-mirror).
4. **MXAO рисует тени гуна на стене (без `r_fullMirror`)** — `r_mirrorViewmodel_depthFix 2` + `r_mirrorViewmodel_clearRttDepth 1`.
5. **MXAO рисует ghost-тени мира при `r_fullMirror 1/2`** — `r_fullMirrorDepth 1` (default v36). Если не помогло — проверь что ReShade Generic Depth действительно подбирает нужный depth (в ReShade addon UI).
6. **Ghost-полоса при `r_blur 1` + `r_fullMirror 1` или `r_hudMirror 1`** — должно работать само на v38.2. Если ghost всё-таки виден — собери `mirror_dump 3` (см. §3.6) на проблемном кадре и приложи к issue.
7. **Гун не следует за `r_filmTweakBrightness/Contrast/Desaturation` / `r_contrast` / `r_desaturation`** — должно работать само на v37.2 (`r_mirrorViewmodel_rttTonemapInject 1` default). Если не работает — `mirror_dump 3`.
8. **Killstreak-баннер с жирной обводкой при `r_hudMirror 1`** — known limitation, переключись на `r_fullMirror 2`.
9. **HUD под ганом** — `r_mirrorViewmodel_rttEarlyComposite 1` (default).
10. **Хочется лог** — `/r_mirrorViewmodel_log 2` для verbose в консоль, `/mirror_dump 3` для file dump.

---

*Этот гайд — для билда `mirror-dll-v3` с патчами v32–v38.2. Если запускаешь старый билд — соответствующих dvar'ов / фиксов не будет (см. §7 как маппинг что появилось когда).*
