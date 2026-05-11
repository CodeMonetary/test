# Mirror Viewmodel — гайд для монтажа

Набор dvar-ов и хуков в **iw3xo** для зеркалирования первого лица (оружие + руки + FX) и/или всей картинки. Сделано для записи/монтажа фрагов с "перевернутой камерой" и для геймплея с оружием в левой руке.

## TL;DR — пресеты

### Леворукий геймплей (просто играть с оружием слева)
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
```
Оружие, руки, гильзы, muzzleflash, трассеры — всё на левой стороне. Мир и HUD — как обычно.

### Монтаж "перевернутая камера" (мир + всё кроме HUD)
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
/r_fullMirror 1
```
Двойное зеркало → мир и FX отражены, оружие визуально оказывается **справа** (это исходное оружие, отражённое дважды), HUD остаётся читаемым в нормальной ориентации. Идеально для нарезок где нужен эффект перевёрнутой записи без потери информации с HUD.

### Тестовый "тупой" режим (вся картинка целиком)
```
/r_fullMirror 2
```
Один горизонтальный flip всего кадра в самом конце. Всё включая HUD будет в зеркале. Самый простой способ "перевернуть запись".

### Выключить
```
/r_fullMirror 0
/r_mirrorViewmodel_rtt 0
/r_mirrorViewmodel_mirrorFx 0
```

---

## Что это вообще такое

`iw3xo.dll` — сторонний клиентский DLL для **CoD4 1.7 MP** (`iw3mp.exe`). Грузится через лоадер (`iw3xo-mp.exe`/loader), хукает D3D9 вызовы и часть функций движка. Mirror-фича — одна из частей iw3xo, которая:

1. Рендерит viewmodel (оружие + руки) в отдельный off-screen render target вместо back-buffer'а.
2. Вкомпозит-ит этот RT обратно в кадр с **горизонтально инвертированными UV** → визуально оружие оказывается на противоположной стороне экрана. При этом тангенты/нормали/cull остаются правильными (мирror через текстуру, не через инверсию матрицы).
3. Подмешивает гун в **tonemap source** перед финальным tonemap'ом → оружие получает ту же цветокоррекцию что и мир, без "песочного" оттенка.
4. Хукает `CG_DObjGetWorldBoneMatrix` — после чтения позиции тэга движком (для muzzleflash, гильз, трассеров) отражает точку и оси через плоскость камеры. Так FX совпадают с зеркальным оружием.
5. Опционально делает полный horizontal-flip back-buffer'а до или после HUD.

Никакие файлы движка не правятся, всё через runtime-хуки. Включается/выключается dvar-ами на лету.

---

## Команды (dvar-ы)

### Основной пайплайн зеркала оружия

#### `r_mirrorViewmodel_rtt` (0/1, default 0)
**Главный выключатель режима зеркала через RTT.**
- `0` — оружие рисуется как обычно, никакого зеркала.
- `1` — оружие рендерится в off-screen текстуру, потом композ-ится обратно с UV-flip. Без хэндедности-инверсии (геометрия и освещение корректные).

#### `r_mirrorViewmodel_rttTonemapInject` (0/1, default 1)
Инжектит зеркальный гун в **tonemap source** до финального tonemap-прохода. Без этого оружие будет с "песочным" / тёплым оттенком относительно мира. Оставлять `1`.

#### `r_mirrorViewmodel_rttEarlyComposite` (0/1/2, default 1)
Когда композ-ить mirror gun:
- `0` — на EndScene (после HUD; гун закрывает HUD).
- `1` — на pre-HUD сигнатуре (HUD рисуется поверх гана; правильно для геймплея). Default.
- `2` — оба места (диагностика).

#### `r_mirrorViewmodel_compositeSrgb` (0..3, default 1)
sRGB-флаги для композ-итa. `1` (default) — линейный sample → sRGB write. Менять только если видны цветовые артефакты.

#### `r_mirrorViewmodel_rttBlend` (0..3, default 2)
Blend-мод композ-итa. Default `2` = ONE/ONE additive, обычно правильно. `1` или `3` — если шейдер оружия пишет нулевую alpha (тогда гун пропадает на additive).

### FX зеркало (гильзы, muzzleflash, трассеры)

#### `r_mirrorViewmodel_mirrorFx` (0/1, default 0)
**Включатель зеркала FX от первого лица.**
- `0` — FX в оригинальном (правом) положении.
- `1` — first-person FX (в пределах `mirrorFxDist` от камеры) отражаются через плоскость камеры. World FX (далеко) не трогаются.

#### `r_mirrorViewmodel_mirrorFxAxis` (0/1/2, default 0)
**Как именно отражать ориентацию tag-а** (от этого зависит направление вылета гильз):
- `0` — только origin, axis не трогаем. Безопасно, но направление вылета гильзы не отражено → визуально кажется что гильзы дрейфуют при повороте камеры.
- `1` — отразить все три оси (полный mirror). Получается left-handed (det = −1) → движок может крашнуться на `AxisToAngles`.
- `2` — **right-handed mirror**: rows 0+2 отражаются, row 1 негируется. det = +1, движок не падает, направление вылета гильзы корректно зеркалится. **Рекомендуется.**

#### `r_mirrorViewmodel_mirrorFxDist` (float, default 64.0)
Радиус (в юнитах от камеры) внутри которого FX считается first-person и отражается. Default 64 ловит muzzleflash/гильзы/трассеры на тэгах вью-модели и не трогает мировые эффекты.

#### `r_mirrorViewmodel_mirrorFxLog` (int, default 0)
Залогировать следующие N FX-отражений в консоль. Полезно для дебага.

#### `r_mirrorViewmodel_mirrorFxAxisIdx` (0/1/2, default 1)
Какой ряд `viewaxis` использовать как нормаль плоскости отражения. `1` = right-axis (правильно). Менять не надо.

### Полное зеркало картинки (NEW v32)

#### `r_fullMirror` (0/1/2, default 0)
**Полно-экранный mirror для монтажа.**
- `0` — выключено.
- `1` — flip всего back-buffer'а **до HUD**. Мир + ваш зеркальный gun отражаются вместе, HUD не трогается. С `r_mirrorViewmodel_rtt 1` оружие флипается дважды → визуально оказывается справа в перевёрнутом мире.
- `2` — flip всего кадра **в EndScene**, после HUD. Мирror всё включая HUD. Простой "перевернуть всю запись" режим.

#### `r_fullMirrorDepth` (0/1, default 1) — NEW v36
**Совместимость `r_fullMirror` с ReShade MXAO/SSAO/RTGI.**

`r_fullMirror` флипает только **color** back-buffer'а; main depth-stencil остаётся в исходной (нефлипнутой) ориентации. ReShade Generic Depth (D3D9) перехватывает `CreateDepthStencilSurface` и подменяет `D24S8 → INTZ` для того, чтобы MXAO/SSAO могли семплить depth как текстуру. С нефлипнутой depth и флипнутым color MXAO рассчитывает occlusion в исходных позициях геометрии и накладывает darkening поверх **уже отзеркалённого** color → видны "призраки" теней в зеркально-неправильных позициях (например, бледный контур оружия плавающий на стене).

При `r_fullMirrorDepth 1` (default) после каждого color flip'а делаем дополнительный 2-pass pixel-shader pass который горизонтально флипает main DSV. ReShade EndScene/Present hook срабатывает после нашего EndScene, поэтому MXAO читает уже флипнутую depth и считает occlusion в координатах, совпадающих с видимым (флипнутым) color — призраки исчезают.

- `0` — выключить depth flip (старое поведение v32–v35).
- `1` — включить depth flip (default, безопасно).

**Безопасность.** Если ReShade Generic Depth не загружен или main DSV не INTZ-формата (фолбек для драйверов без поддержки FOURCC INTZ), функция silently возвращает false — depth не трогается, никаких артефактов. То есть dvar безопасно держать в `1` всегда.

**Производительность.** Два fullscreen quad'а с тривиальным PS (1 texld + 1 mov oDepth). Накладные расходы пренебрежимо малы.

### Старый matrix-flip пайплайн (для справки)
До RTT был matrix-flip путь — менялся знак столбца projection-матрицы. Производил left-handed геометрию, ломал нормали. Оставлен для отладки.

- `r_mirrorViewmodel_method` — выбор паттерна matrix-flip.
- `r_mirrorViewmodel_flipVSCF`, `r_mirrorViewmodel_flipFollow`, `r_mirrorViewmodel_flipAxis`, `r_mirrorViewmodel_flipReg` — низкоуровневые тогглы.
- `r_mirrorViewmodel_cullFix` — компенсация инвертированного cull-mode для matrix-flip.

**Не используйте если включён `r_mirrorViewmodel_rtt 1`** — RTT путь даёт корректный результат без побочек.

---

## Workflow для монтажа

### 1. Геймплей-нарезка с оружием в левой руке (без эффекта "перевернутой камеры")
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
/r_fullMirror 0
```
Записываете демо как обычно, играете и стреляете — оружие, руки, гильзы, вспышка, трассеры все на левой стороне. Мир и противники в нормальной ориентации.

### 2. "Перевернутая камера" (мир отражён, оружие справа, HUD читается)
```
/r_mirrorViewmodel_rtt 1
/r_mirrorViewmodel_mirrorFx 1
/r_mirrorViewmodel_mirrorFxAxis 2
/r_fullMirror 1
```
Включаете во время воспроизведения демо. На записи мир будет зеркальный, оружие визуально оправа (двойное отражение), HUD читаемый. Эффект "записал я, но смонтировал зеркально" без потери UI.

### 3. Простой brute-force flip всей картинки
```
/r_fullMirror 2
```
Любую запись/демо — ещё раз воспроизвели с этим флагом → готово, всё в зеркале (включая HUD-цифры, но они теперь развёрнуты).

### Тонкости

- `r_mirrorViewmodel_rtt` влияет на live-геймплей и на воспроизведение демо одинаково — можно записать демо без mirror, потом включить mirror и воспроизвести.
- `r_fullMirror 1` имеет смысл только когда `r_mirrorViewmodel_rtt 1` включён (иначе оружие отразится **один** раз и окажется неправильно). Если хотите без двойного отражения — используйте `r_fullMirror 2`.
- HUD-элементы (kill-feed, ammo, kill-cam) остаются читаемыми в режиме `r_fullMirror 1` потому что они рисуются **после** flip'а.
- Для записи 60+ FPS — отдельно, эта фича рендеру FPS не вредит.

---

## Внутреннее устройство (кратко)

### Pipeline зеркала viewmodel (`r_mirrorViewmodel_rtt 1`)
1. **Detect viewmodel scene.** При `SetVertexShaderConstantF` / projection-matrix upload смотрим на `depthHackNearClip != 0` — это маркер depth-hack proj, используемый только для гана.
2. **Switch RT.** Bind off-screen color + depth (POOL_DEFAULT, BB размер). Гун рисуется туда вместо back-buffer'а.
3. **Restore engine RT.** На std-proj upload (мир продолжает рисоваться) возвращаем engine RT — мир пишется в свой target, depth_prefill переиспользуется.
4. **Detect tonemap-source bind.** PSCF c7 fingerprint: `c70 == c71 == c72`, `−0.2 < c70 < 0`, `1 < c73 < 5`. Это уникальная подпись финального tonemap-прохода (обычные сцены / pre-FX / shadow проходы её не используют).
5. **Inject.** Перед tonemap-проходом композ-им гун-RT в **tonemap source** текстуру с UV-flip. Tonemap проход потом съедает её и пишет на BB.
6. **Result:** Гун получает ту же tone-mapping / film curve / color grade что и мир. Без "песочного" оттенка.

### FX mirror (`r_mirrorViewmodel_mirrorFx 1`)
1. **Hook `CG_DObjGetWorldBoneMatrix`** на адресе `0x433F00`. Пролог (8 байт): `83 EC 30 53 8B 5C 24 38`. Trampoline сохраняет полный пролог + JMP назад.
2. **Pre-call:** позволяем оригинальной функции отработать (вызывается рекурсивно, потому используется stack-frame state, не статики).
3. **Post-call:** проверяем dist между tag origin и `cgs->refdef.vieworg`. Если ≤ `mirrorFxDist`:
   - **Отражаем `origin`** через плоскость через `vieworg` с нормалью = `viewaxis[1]` (right-axis):
     ```
     d = dot(origin - vieworg, n)
     origin' = origin - 2*d*n
     ```
   - **Отражаем `axis`** в зависимости от `mirrorFxAxis`:
     - mode 2 (RH-mirror): rows 0 и 2 отражаются, row 1 негируется. det остаётся +1, brass eject direction корректно зеркалится.

### Full-screen mirror (`r_fullMirror`)
1. **`mode 1`** — на том же PSCF c7 fingerprint ставим `g_pending_fullmirror_flip = true`. После следующего draw-а (= tonemap output draw) делаем:
   - `StretchRect(BB → flip_tex)`
   - SetTexture flip_tex, draw fullscreen quad с UV (1→0) обратно в BB.
   - HUD рисуется потом на уже отражённый BB.
2. **`mode 2`** — то же, но в `EndScene()` после того как HUD уже отрисован.

---

## Известные ограничения

- **Только first-person FX.** Мировые гильзы/вспышки от других игроков не отражаются (и не должны — иначе мир выглядит сломанным).
- **`mirrorFxAxis 1` крашит на стрельбе** — оставлен для документации, не использовать.
- **Демо записанные ДО включения mirror** воспроизводятся правильно — mirror применяется на стороне рендера, не модифицирует demo-data.
- **Скриншоты** через `screenshot` команду игры — ловят финальный BB, поэтому работают со всеми режимами mirror корректно.
- **На некоторых картах с экзотическими post-FX шейдерами** PSCF c7 fingerprint может пропасть — в этом случае `r_mirrorViewmodel_rttEarlyComposite` фолбэкается на EndScene композ (HUD будет под ганом).

---

## Если что-то сломалось

1. Включить лог: `/r_mirrorViewmodel_log 2` и `/r_mirrorViewmodel_mirrorFxLog 30`, повторить кейс, прислать выхлоп консоли.
2. Записать дамп: `/mirror_dump 5` — пишет 5 кадров в `mirror_dump_<timestamp>.txt` рядом с iw3xo-mp.exe. Прислать файл.
3. Откатить всё: `/r_fullMirror 0; r_mirrorViewmodel_rtt 0; r_mirrorViewmodel_mirrorFx 0`.

---

## История версий (mirror feature only)

| ver | что добавлено |
|-----|---------------|
| v15 | базовый RTT mirror viewmodel |
| v20 | early composite через PSCF c7 fingerprint (HUD над ганом) |
| v23 | обобщённая PSCF fingerprint (живая игра + демо replay) |
| v24 | sRGB write-encode (фикс "песочного" тинта) |
| v25 | tonemap-source injection (правильный свет) |
| v26 | хук `CG_DObjGetWorldBoneMatrix` для FX |
| v29 | trampoline 8 байт (фикс краша на стрельбе) |
| v31 | RH-mirror axis mode 2 (фикс дрейфа гильз при повороте) |
| v32 | `r_fullMirror` для полного flip-а кадра |
| v33 | `r_mirrorViewmodel_depthFix` / `r_mirrorViewmodel_clearRttDepth` — фикс ghost MXAO на зеркальном гуне |
| v35 | `r_hudMirror` — отдельный HUD-mirror через RTT (совместим с ReShade Flip.fx) |
| v36 | `r_fullMirrorDepth` — flip main DSV вслед за color при `r_fullMirror 1/2` (фикс ghost MXAO/SSAO в ReShade) |
| v37 | расширенный PSCF c7 fingerprint — гун снова получает filmtweak/grading при произвольных `r_filmTweakBrightness/Contrast/Desaturation`, `r_contrast`, `r_desaturation`. Изначально включал PS-pointer cache fallback, но он давал ghost-слой на `r_fullMirror 1` и `r_hudMirror 1` (плюс чёрный sun-glare в hud-зеркале) → cache откатан, остался только расширенный structural check |
