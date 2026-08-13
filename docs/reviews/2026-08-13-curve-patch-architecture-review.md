# Архитектурное ревью: Sculpt / Paint Curve 3D (Curve Patch)

**Ветка:** `sculpt-paint-curve-3d-V9`  
**Коммит функционала:** `c44c9fd295e00a7d92f91e1652e942245fc47540`  
**База:** `fbe6228777e` (Blender 5.2.0 release)  
**Дата ревью:** 2026-08-13  
**Объём:** 124 файла, +32631 / −994 строк  

Debug-сообщения (`CURVE_PATCH_PROFILING`, `PAINT_CURVE_CURSOR_PROFILING`, `DEBUG-cpatch`, `DEBUG-pccursor`) в этом отчёте не рассматриваются.

Цель ревью — не «можно ли мержить в blender.org», а три рабочих вопроса:

1. Насколько реализация совпадает с архитектурой Blender (DNA / RNA / BKE / ED / Draw / Undo / WM).
2. Где уже есть точки расширения, а где следующее улучшение упрётся в связность.
3. Какие области закрыть **до** наращивания функционала, чтобы не тащить долг дальше.

---

## 1. Вердикт

**Каркас правильный и пригоден как база для дальнейшего развития.** Разделение BKE (чистая геометрия) и ED (сессия, эффекты, операторы, текстуры) настоящее, не косметическое. Интерфейс эффектов, headless apply, RNA-чтение патча, transform через публичные `ED_*` и overlay вместо WM paint-cursor — это те же слои, которыми Blender пользуется для sculpt/paint.

**К расширению код готов неравномерно.** Новый эффект (mask, ещё один paint-target) и новый слайдер кисти добавить относительно дёшево. Интерактивное редактирование нескольких кривых, новый тип ленты, другие режимы (GP, dyntopo, curves sculpt) и любые правки модального UX упрутся в несколько монолитов и неявных контрактов.

**Перед наращиванием функционала стоит закрыть точки хрупкости**, иначе следующий слой будет строиться на UAF-риске снимка поверхности, неполном lifetime сессии, пересборке GPU-батчей каждый кадр и раздутых файлах модалки.

Кратко:

| Вопрос | Ответ |
| --- | --- |
| Архитектура в целом совпадает с Blender? | Да, по основным слоям |
| Можно наращивать эффекты / Python / DNA-настройки? | Да, точки расширения уже есть |
| Можно сразу делать multi-curve edit / новые режимы / новый тип ленты? | Нет, сначала развязать модалку, overlay и контракт live/frozen |
| Блокеры корректности до расширения? | Снимок поверхности, active-index слотов, lifetime сессии при teardown без mode-exit |

---

## 2. Карта архитектуры

```
DNA
  Brush.curve_patch          настройки кисти (слоты, caps, stamp/ribbon)
  PaintCurve.geometry        CurvesGeometry — авторитетное представление
  Sculpt.paint_curve_*       видимость / display mode
        │
        ▼
BKE  (source/blender/blenkernel, BKE_curve_patch.hh)
  CurvePatchParams           brush-free вход сборки
  CurvePatchSpline           дуга, касательные, нормали, cyclic
  CurvePatchRibbonLut        (u, s) лента
  CurvePatchFrameSet         локальные окна на поверхности
  CurvePatchStamp            раскладка штампов
  CurvePatchSurfaceSnapshot  wrap по исходной поверхности
  curve_patch_build_*        сборка без PBVH / operator / context
        │
        ├── RNA
        │     rna_brush / rna_sculpt_paint / rna_object / rna_curves
        │     PaintCurve.curve_patch_to_mesh / curve_patch_stamps
        │     sculpt.curve_patch_apply
        │
        └── ED  (editors/sculpt_paint)
              CurvePatchSession          runtime, sibling StrokeCache
                CurvePatchItem[]         документ (кривая + frozen params + geometry)
                CurvePatchTextureBinding MTex-копии, DNA не протекает в BKE
                CurvePatchEffect         Relief | Color | Image
                CurvePatchSampler        покрытие, не знает target
                session undo             Knife-стиль (не BKE undo)
                SCULPT_OT_curve_patch_edit
                headless apply
                    │
                    ├── Draw overlay     ED_paint_curve_draw + session accessors
                    ├── Transform        ED_curve_patch_session_* + CTX_CURVE_PATCH
                    └── WM               modal-all-windows + ModalViewportTracker
```

### 2.1 Что на каком слое и почему это важно

| Слой | Ответственность | Расширять здесь |
| --- | --- | --- |
| DNA | Персистентность кисти и PaintCurve | Новый слайдер / enum / слот |
| BKE | Геометрия патча без кисти и контекста | Новая параметризация ленты, штампы, wrap |
| ED session | Lifetime, текстуры, PBVH, undo сессии | Multi-patch edit, новые операторы |
| Effect | Что писать в mesh / color / image | Новый target (mask, face set only, …) |
| Sampler | Куда патч достаёт и с какой силой | Новая проекция штампа |
| Overlay | Ручки, силуэты, hover | Новый визуальный элемент кривой |
| RNA | Python и UI | Read-back, apply, session introspection |
| WM/Transform | Модальность, G/R/S | Keymap, snap, multi-window |

Это правильное разложение. Главный риск расширения — что **модалка, overlay и live-sync кисти знают слишком много «всего сразу»**, и новый функционал начнёт оседать туда же.

---

## 3. Соответствие архитектуре Blender

### 3.1 Что сделано канонично

**DNA → BKE → ED → RNA.**  
`BrushCurvePatchSettings` собран в одну структуру на `Brush`, а не размазан по `MTex` и случайным полям. Комментарий в DNA прямо объясняет, почему: Python иначе ходил бы по двум несвязанным путям. Это тот же рефлекс, что у современных sculpt-настроек.

**`PaintCurve` переведён на `CurvesGeometry`.**  
Это тот же паттерн, что у ID `Curves` / Grease Pencil: авторитетная геометрия с атрибутами, bezier handles, cyclic, multi-spline. Copy/free/blend написаны через `CurvesGeometry::blend_write/read` и placement-new. Legacy `PaintCurvePoint *points` помечен `DNA_DEPRECATED` и живёт только между read и versioning — это правильный compatibility-путь Blender.

**Ядро BKE не тянет кисть, PBVH и `bContext`.**  
`CurvePatchParams` дублирует DNA-enum'ы сознательно. Конвертация в `paint_curve_patch_params.cc` — явный `switch` без `default`, чтобы новый DNA-enumerator без пары в BKE дал предупреждение компилятора, а не тихий fallback. Это лучшая практика для слоя, который должен работать из Python без живой кисти.

**Сессия — sibling `StrokeCache`.**  
`SculptSession::curve_patch_session` — тот же приём, что `StrokeCache` / `filter::Cache` / `expand::Cache`: editor-тип, который BKE только forward-declares. Для Blender это привычный компромисс (BKE не должен знать editor). Цена компромисса описана в §5.4.

**Эффекты — настоящая точка расширения.**  
`CurvePatchEffect` разделяет restore / begin_restamp / apply_pass / end_restamp / commit. Sampler не знает, пишем ли мы позиции, цвет или пиксели. Headless apply (`paint_curve_patch_apply.hh`) идёт тем же путём, что модалка. Python `sculpt.curve_patch_apply` не обходит ядро стороной.

**Undo сессии обоснован, не «свой велосипед от лени».**  
Sculpt undo хранит атрибуты меша и не имеет слота под control curve. Paint-curve undo отказывается от Sculpt Mode и хочет настоящий `PaintCurve` ID, а runtime-кривая сессии — не ID. Комментарий ссылается на knife (`KnifeUndoFrame`). Это канонический Blender-ответ: модалка с runtime-состоянием, которое ни один undo type не описывает, держит свой стек. Commit потом складывается в обычный sculpt/image undo.

**Transform не включает private header.**  
`transform_convert_curve_patch.cc` ходит через `ED_curve_patch_session_*`. Слой transform не знает `CurvePatchSession`. Это правильная граница модулей.

**Overlay вместо WM paint-cursor.**  
Ручки живут в Overlay engine, чтобы не пропадали, когда курсор уходит из viewport. Это исправление известного класса багов paint-cursor и совпадает с направлением Draw module.

**Мультиоконность вынесена в WM/ED util, а не спрятана в модалке.**  
`WM_event_add_modal_handler_all_windows` + `ModalViewportTracker` — переиспользуемый вклад в window manager. Им уже пользуются и paint-curve slide, и curve-patch edit. Для апстрима это даже более ценно, чем сам патч: решается общая проблема модалок в split view.

**ID users для текстур.**  
`brush.cc`: copy (`BLI_duplicatelist`), free, `foreach_id` (`tex_start/middle/end` + слоты), blend write/read. Слот добавляется через `MEM_new`, user count снимается при remove. Это обязательный Blender-контракт для указателей на `Tex`.

**Versioning есть.**  
`versioning_520.cc` 45–48: дефолты display, clamp stroke method, sentinel `length_repeat == 0` для дефолтов Curve Patch, конвертация legacy points, сброс `mtex.random_angle`. Для файлов, написанных **этой** веткой, путь продуман.

**Тесты стоят на правильных этажах.**  
BKE: spline / ribbon / frames / build. ED: sampler / session / geometry init. Python: `bl_curve_patch_apply.py`, `bl_paint_curve_rna.py`. Это ближе к практике Blender (gtest ядра + python operators), чем «только ручной прогон».

**Комментарии объясняют why.**  
Инварианты вроде `curve_patch_stamp_reach`, `stamp_search_reach`, timing `session_undo_begin` vs `BKE_undosys_step_push_init_abort` — это как раз то, что позволит следующему человеку не повторить уже пойманные клиппинги.

### 3.2 Где расхождение с конвенциями Blender

| Тема | Как принято в Blender | Как сейчас |
| --- | --- | --- |
| Модальные горячие клавиши | `WM_modalkeymap`, пункты keymap, Industry Compatible | `switch (event->type)` на `EVT_ZKEY` / `CKEY` / `YKEY` / `GKEY` / `RKEY` / `SKEY` |
| Overlay data | Draw читает BKE/runtime-снимок | Overlay зовёт `ED_paint_curve_patch_active_control_curve` и знает сессию |
| RNA session type | Forward-declared struct или BKE-visible readonly | `const void*` + россыпь `ED_curve_patch_session_*` |
| Идентичность файла | Новая ломающая DNA — новый `BLENDER_VERSION` / `versioning_53x` | Файл всё ещё 5.2.0 LTS, subversion 48 |
| Размер translation unit | Операторы дробятся | `paint_curve.cc` ~3070, `paint_curve_patch_edit.cc` ~2044, `paint_stroke_roll.cc` ~2118 |
| Copyright header | Blender Authors | Часть файлов — «Nazir Galimov» (`paint_curve_intern.hh`, overlay, transform convert, `ED_paint_curve_draw.hh`) |

Расхождения не отменяют §3.1. Они задают цену входа в апстрим и цену следующего UX-слоя.

---

## 4. Готовность к расширению

Это главный раздел отчёта: что будет дёшево, что дорого, и какие скрытые контракты надо кодировать, прежде чем добавлять фичи.

### 4.1 Уже легко

**Новый эффект (mask, face-set-only, vertex-paint variant).**  
Новый `.cc`, ветка в `curve_patch_effect_create`, свой `UpdateType`. Sampler и session не обязаны меняться, если target адресуется плоским индексом и умеет snapshot/restore. Python уже выбирает эффект явно (`CurvePatchEffectType`), не только «угадать из кисти».

**Новый слайдер / enum кисти, который участвует в сборке.**  
DNA + RNA + поле в `CurvePatchParams` + ветка в `curve_patch_params_from_brush` + versioning default. Live-sync модалки сравнивает `CurvePatchParams` через `operator==`, поэтому новое поле в params подхватится без ещё одного `last_synced_*`.

**Новая проекция штампа.**  
Enum DNA/BKE + ветка в sampler. Раскладка штампов уже отдельный файл (`curve_patch_stamps.cc`).

**Python one-shot apply и read-back.**  
`PaintCurve.curve_patch_to_mesh`, `curve_patch_stamps`, `sculpt.curve_patch_apply(use_all_splines=...)` уже существуют. Headless путь не требует WM. Это правильный задел под скрипты и регрессии.

**Несколько сплайнов в headless.**  
`CurvePatchSession::patches` — вектор. Комментарий прямо говорит: интерактив сейчас публикует один, plural нужен для `use_all_splines` и как следующий шаг модалки. Модель данных к multi-curve ближе, чем UI.

**Переиспользуемый WM-слой.**  
Любая следующая модалка с 3D-кривой в split view уже может взять `ModalViewportTracker`, а не копировать ad-hoc.

### 4.2 Уже трудно — это и есть очередь на рефакторинг

**Интерактивное редактирование нескольких кривых.**  
Данные готовы (`Vector<CurvePatchItem>`, overlay уже рисует все патчи, undo шагает по всем items). Модалка, picking, transform, RNA `Object.curve_patch_session` и `active_point` заточены под **одну** активную кривую. Overlay даже копит handles всех патчей в один контейнер без per-patch индекса в hit-test. Следующий клик «переключить активный патч» полезет в `paint_curve_patch_edit.cc` (~2000 строк) и overlay header (~650).

**Новая параметризация ленты (объём, mesh-following UV без LUT, не-(u,s) поверхность).**  
LUT, frames, sampler, overlay и texture zones все предполагают ленту `(u, s)`. Отдельного интерфейса «surface parameter domain» нет — только функции. Это менять придётся пакетом, не точкой расширения.

**Новые paint modes (Grease Pencil, curves sculpt, dyntopo).**  
Сессия живёт на `SculptSession`. Dyntopo явно отказан: нет стабильного индекса для snapshot. GP потребовал бы другой ключ снимка. Color/Image завязаны на mesh color attribute и image canvas. Это не «ещё один Effect», это другой apply-state.

**Кастомные хоткеи / Industry Compatible.**  
Пока модалка хардкодит клавиши, любой новый жест (переключение патча, пропорциональное редактирование, lock axis) снова пойдёт в `switch (event->type)`. Сначала нужен `WM_modalkeymap`, как у `PAINTCURVE_OT_slide`.

**Производительность длинной кривой × штампы × symmetry.**  
Каждый drag: restore всех touched + rebuild LUT/frames + PBVH walk + (сейчас) пересборка GPU-батчей overlay. Потолки `CURVE_PATCH_MAX_LUT_PIXELS` (1M) и `CURVE_PATCH_STAMP_NUM_MAX` (10k) ударят раньше, чем UX успеет это объяснить. Overlay уже знает, что силуэты кэшируются, а handles — нет.

### 4.3 Скрытые контракты — главный тормоз расширения

Их нет в типах, они живут в комментариях. Пока они не кодированы, каждый новый разработчик (и каждый новый слайдер) будет ломать их молча.

#### Контракт A — что frozen, что live

На `CurvePatchItem::params` заморожены: `radius` / `radius_per_size`, `plane_normal`, `stamp_seed`, `swap_axis` (ещё и хоткей S / undo).  
Сила, направление Add/Subtract, Length/Stamps-режим, текстуры, falloff, mirror symmetry, цвет — читаются live.  
Модалка дополнительно держит десяток `last_synced_*` watchdog'ов на то, чего нет в `CurvePatchParams` (указатели `Tex`, digest слотов, `changed_timestamp` falloff, overlay tex edit count).

Следствие: новый параметр кисти надо явно классифицировать.

- Если он влияет на **сборку геометрии** — поле в `CurvePatchParams`, дальше `operator==` сам триггерит restamp.
- Если он влияет на **сэмплинг/силу/цвет**, но не на params — новый `last_synced_*` в `CurvePatchEditOpData`. Иначе UI будет меняться, а патч — нет, либо наоборот.
- Если он **per-patch** (как `stamp_seed`) — ещё и undo item, и решение «синхронизировать ли по всем patches». Сейчас модалка пишет brush-driven поля во **все** патчи, а frozen оставляет per-item. Это нужно сохранить в одном месте, не копировать в каждый новый оператор.

Рекомендация: явная таблица/хелпер `curve_patch_params_live_overlay(brush, frozen_base)` плюс отдельный `CurvePatchLiveInputs` для того, что не должно жить в params. Сейчас это размазано между `params`, binding и `CurvePatchEditOpData`.

#### Контракт B — два редактора одной геометрии

| Редактор | Документ | Undo | Операторы |
| --- | --- | --- | --- |
| Paint Curve ID | `PaintCurve.geometry` | paint-curve undo sys | `PAINTCURVE_OT_*` в `paint_curve.cc` |
| Live Curve Patch | `CurvePatchItem.control_curve` (не ID) | session undo | `SCULPT_OT_curve_patch_edit` |

Общая часть — `paintcurve_geometry_*` в `paint_curve_intern.hh` / `paint_curve_geometry.cc`. Это хорошо.  
Плохо то, что picking, slide, insert, keymap, snap всё ещё живут в двух модалках. Любой новый edit-tool (multi-select, proportional, lock) почти наверняка будет реализован один раз в patch-edit и забыт в paint-curve — или наоборот.

Рекомендация до расширения edit-tooling: вынести «редактор bezier на `CurvesGeometry`» (pick, insert, slide, radius, snap) в общие функции, а двум операторам оставить только документ и undo.

#### Контракт C — захват `StrokeCache`

После anchor/roll-штриха сессия **забирает** `SculptSession::cache`, подменяет `ViewContext` на owned-копию и держит cache живым до commit/cancel. Stroke undo transaction при этом abort'ится, потому что иначе любой чужой undo в приложении мог бы её усыновить.

Это работает, но это не API. Любой рефактор `StrokeCache` / `do_symmetrical_brush_actions` сломает restamp. Новый способ стартовать патч (не из штриха, а из готовой PaintCurve без cache) уже частично есть в headless apply; интерактивный путь всё ещё завязан на cache.

Рекомендация: сузить зависимость restamp до `CurvePatchStrokeContext` (он уже есть в sampler) и явно не читать `ss.cache` из эффектов. Тогда третий entry point не обязан притворяться штрихом.

#### Контракт D — смешивание вкладов

Relief и Image усредняют overlapping contributions через `pass_weight_accum`. Color документированно отдаёт last-item-wins. Пока интерактивно один патч — это незаметно. Как только multi-curve edit включится, два пересекающихся патча на vertex color будут вести себя иначе, чем relief. Это нужно решить **до** включения переключения кривых, не после баг-репорта.

#### Контракт E — Python всё ещё требует `Brush`

`curve_patch_to_mesh` / apply принимают кисть, даже если BKE умеет строить из `CurvePatchParams`. Headless «без кисти», ради которого ядро отделяли, на RNA-границе не завершён: эффект `apply_pass` тоже берёт `const Brush &`. Скрипт не может собрать патч из голых params + списка текстур.

Для расширения скриптового API это следующий естественный шаг: `CurvePatchParams` (+ optional texture binding) как вход RNA, кисть — удобный сахар.

#### Контракт F — идентичность файла vs официальный 5.2.0 LTS

`BLENDER_VERSION 502`, cycle `release`, suffix `LTS`, file subversion 48. `PaintCurve` больше не пишет старый массив `points`; авторитет у `CurvesGeometry`. Официальный 5.2.0 откроет файл `502.48` (min version 4.5) и не найдёт `geometry` — кривые приедут пустыми. Плюс коллизия с blender.org subversions 5.2 LTS.

Для **внутренней** работы на ветке это не стоп-кран. Для любого обмена `.blend` и для апстрима — обязательно увести идентичность на 5.3 (или свой fork-version), иначе архитектурно правильная DNA будет выглядеть как битый 5.2-файл.

---

## 5. Находки

Серьёзность калибрована под цель «готовить почву к расширению», а не под merge в blender.org сегодня.

### 5.1 Critical — закрыть до того, как на этом будут строить новое

Сюда попали вещи, которые дают неверный результат или UAF на уже существующих путях, плюс ломающая идентичность файла, если `.blend` вообще покидает машину.

#### C1. Снимок поверхности держит topology spans живого меша

**Где:** `source/blender/blenkernel/intern/curve_patch_surface.cc:35-42`, `BKE_curve_patch.hh` (`CurvePatchSurfaceSnapshot`), `BKE_bvhutils.hh:30-46`

**Что не так.** Копируются `positions` и `vert_normals`. `BVHTreeFromMesh` получает `Span` на `mesh.faces()`, `mesh.corner_verts()`, `mesh.corner_tris()` — это массивы **живого** меша. Инвалидация сессии смотрит только на `verts_num` (`CurvePatchApplyState::element_num`).

**Почему это важно.** Модалка пропускает события наружу. Remesh / rebuild CustomData с тем же числом вершин оставляет BVH callbacks на освобождённых массивах. Shrinkwrap читает freed memory. Комментарии сами описывают этот класс опасности, но детект слишком узкий.

**Как чинить.** Копировать faces / corner_verts / corner_tris в снимок (или владеть BVH вместе с topology). Считать любой topology/CustomData change инвалидацией, не только `verts_num`. Это блокер для любых фич, которые дольше держат сессию открытой (multi-curve edit, смена объекта, скрипты в фоне).

#### C2. Удаление текстурного слота всегда декрементирует `texture_active_index`

**Где:** `source/blender/blenkernel/intern/brush.cc:855-857`

**Что не так.** После `BLI_freelinkN` код делает `max(0, texture_active_index - 1)` независимо от того, какой слот удалили.

**Почему это важно.** Пять слотов, active = 2, удалили слот 4 → active становится 1. UI list и RNA `texture_active` указывают не туда. Move-оператор индекс обновляет правильно (`paint_ops.cc:636-637`); remove — нет. При расширении списка текстур это всплывёт сразу.

**Как чинить.** Запомнить индекс удаляемого. Если `removed < active` — декремент; если `active >= new_count` — clamp; если список пуст — 0.

#### C3. Файлы всё ещё представляются как Blender 5.2.0 LTS при ломающей DNA `PaintCurve`

**Где:** `BKE_blender_version.h:23-33`, `versioning_520.cc:918-975`, `DNA_brush_types.h:587-616`

**Что не так.** См. контракт F. Versioning внутри ветки аккуратный; внешний контракт файла — нет.

**Когда чинить.** Перед любым обменом `.blend` и перед апстримом. Не блокирует локальную разработку, но это не «мелочь на потом», если файлы уже разъезжаются по людям.

### 5.2 Important — архитектура / расширяемость / lifetime

#### I1. Overlay пересобирает GPU-батчи ручек на каждый `begin_sync`

**Где:** `overlay_paint_curve_cursor.hh:39-44, 69-70, 95-100`

Силуэты кэшируются по ключу. Handles / points / radius — `free_batches()` каждый overlay sync (движение мыши). Curve Patch на drag и так пересобирает ленту. На длинных кривых viewport упрётся сюда раньше, чем в PBVH.

**Фикс:** ключ батчей = hash геометрии + view/projection, по аналогии с `silhouette_batches_key_`. Это же откроет дорогу к отрисовке нескольких патчей без линейного роста стоимости кадра.

#### I2. Живую сессию нельзя уничтожить из BKE object free

**Где:** `paint.cc` `SculptSession::~SculptSession` (не освобождает `curve_patch_session`), `paint_curve_patch_session.cc:581-604`, `mesh/sculpt_ops.cc:505-512`

Mode-exit вызывает `curve_patch_discard_on_session_end`. `BKE_object_free` → `BKE_sculptsession_free` — нет. Это тот же паттерн, что у `StrokeCache`, но сессия держит snapshot меша и uncommitted relief. Модалка уже имеет dangling-session guard (`paint_curve_patch_edit.cc:953-973`) — значит, сценарий реальный.

**Фикс:** либо минимальный session type в BKE с restore+free в деструкторе, либо editor free-callback на `SculptSession`. Не полагаться только на mode-exit. Для расширения (дольше живущая сессия, несколько объектов) это обязательный фундамент.

#### I3. `SCULPT_OT_curve_patch_edit` хардкодит клавиши вместо modal keymap

**Где:** `paint_curve_patch_edit.cc:1834-1904` (`EVT_ZKEY`, `CKEY`, `YKEY`, `GKEY`/`RKEY`/`SKEY`)

Paint-curve slide уже использует `WM_modalkeymap`. Curve Patch — нет. Пользователь не перебиндит undo/cyclic/swap-axis/G/R/S. Industry Compatible будет расходиться. Любой новый жест снова пойдёт в тот же `switch`.

**Фикс:** `WM_modalkeymap` по образцу `PAINTCURVE_OT_slide` (`paint_curve.cc` ~2803), dispatch enum items в modal. Делать **до** добавления новых edit-жестов.

#### I4. Нет BKE-тестов на штампы, surface wrap и texture mapping

**Где:** `blenkernel/CMakeLists.txt:770-773`. Есть spline/ribbon/frames/build. Нет `curve_patch_stamps_test.cc`, `curve_patch_surface_test.cc`, `curve_patch_texture_map_test.cc`, `curve_patch_to_geometry_test.cc`. Editor geometry test — один кейс (`paint_curve_geometry_test.cc`, 17 строк).

Именно эти куски уже давали клиппинг «два раунда». Python покрывает apply/RNA сверху. Без BKE-тестов следующий рефактор ленты/штампов будет гадать.

Минимум: stamp count + cyclic ghosts; snapshot shrinkwrap + invalid normals; `curve_patch_texture_zone_at` caps/repeat/cyclic; `curve_patch_geometry_to_mesh` ownership.

#### I5. Draw overlay зависит от живой editor-сессии

**Где:** `overlay_paint_curve_cursor.hh:14, 164-221`; `ED_paint_curve_draw.hh`

Overlay включает editor draw helpers и зовёт `ED_paint_curve_patch_active_control_curve` / `_control_curve_at`. Draw уже зависит от `editors/include` в Blender, но теперь overlay знает про Curve Patch session.

Следствие: второй живой тип кривой или новый визуальный слой будет расти в `ED_paint_curve_draw.hh` и overlay вместе. Overlay нельзя понять без sculpt_paint.

**Фикс:** маленький `PaintCurveOverlayData` (или `CurvesGeometry` + флаги + active index) на runtime. Overlay потребляет данные, не session accessors. Это развязка, без которой multi-curve overlay будет копипастить session-знание.

#### I6. Два монолита операторов

**Где:** `paint_curve.cc` (~3070 строк после сплита), `paint_curve_patch_edit.cc` (~2044), плюс `paint_stroke_roll.cc` (~2118)

Геометрию/sync/convert/undo/draw уже вынесли — это правильный сплит. Остались operators + picking + slide в одном файле и вся семантика live-edit в другом. Следующий эффект или edit-tool снова осядет сюда. Review и бисect уже тяжелые (один 32k-строчный коммит).

Практичный сплит, не «размазать всё»:

- `paint_curve_ops.cc` — регистрация / poll / exec операторов ID
- `paint_curve_slide.cc` — slide + radius + snap marker
- `paint_curve_patch_edit_keymap.cc` / `_undo.cc` / `_sync.cc` — вынуть из модалки
- `paint_stroke_roll.cc` трогать только когда понадобится менять roll; сейчас это отдельный вход в ту же сессию

#### I7. `Object.curve_patch_session` — opaque `void*`

**Где:** `ED_paint.hh:273-293`, `rna_object.cc:358-365, 3788-3795`

RNA и transform избегают private header через `const void*`. Каждый новый session field = ещё один `ED_curve_patch_session_*` без проверки типа.

**Фикс:** тонкий публичный `ED_curve_patch.hh` с forward-declared `CurvePatchSession`, либо BKE-visible readonly struct. Мутацию оставить в editor. Иначе RNA session API не вырастет дальше 5 полей, не раздуваясь.

#### I8. Неявный live/frozen контракт (см. §4.3 A)

Не баг, а архитектурный долг. Пока таблица ответственности не кодирована, новый DNA-слайдер с высокой вероятностью попадёт не в тот слой (заморозить силу, забыть про текстурный digest, синхронизировать `stamp_seed` по всем патчам и т.д.).

#### I9. `apply_pass` и RNA apply требуют `Brush`

Ядро brush-free, граница ED/RNA — нет. Блокирует скриптовый apply без синтетической кисти и тестирование эффектов на голых params.

#### I10. Color vs Relief/Image при overlap (см. §4.3 D)

Документировано. Решить политику смешивания до интерактивного multi-patch.

### 5.3 Minor

#### M1. Сиротский Doxygen у `curve_patch_geometry_build`

`BKE_curve_patch.hh:885-926` — два подряд блока комментариев; первый описывает `curve_patch_geometry_build`, второй — `curve_patch_build_from_control_curve`. Привязать каждый к своей функции (или удалить мёртвый, если `geometry_build` больше не публичный).

#### M2. `CURVE_PATCH_FORCE_SINGLE_FRAME` в production

`curve_patch_build.cc:25-30, 233-238`. Сейчас 0. Перед апстримом убрать или оставить только под `NDEBUG`/локальный define. Это не debug-print, это compile-time диагностический рычаг в ядре.

#### M3. Смешанные copyright headers

`paint_curve_intern.hh`, `overlay_paint_curve_cursor.hh`, `transform_convert_curve_patch.cc`, `ED_paint_curve_draw.hh` — «Nazir Galimov». Остальное — Blender Authors. Для апстрима нужен единый SPDX Blender Authors.

#### M4. RNA radius fallback 0 вместо конвенции paint-curve 1.0

`rna_curves.cc:261-267` — `lookup_or_default(..., 0.0f)`. Комментарии и `add_point` пишут 1.0 = полный размер кисти; 0.01 — hair default `CurvesGeometry`. Бьёт только отсутствующий атрибут. Для `ID_PC` default должен быть 1.0.

#### M5. Versioning sentinel `length_repeat == 0`

`versioning_520.cc:945-959`. Элегантно, но любая запись 0 после появления фичи будет затёрта при reload файла со старым subversion. После ухода на 5.3 блок станет историей; паттерн не копировать для новых полей.

#### M6. Дублирование surface snapshot на патч

`paint_curve_patch_session.cc:670-682`. Задокументировано. Интерактив — один патч. Headless `use_all_splines` копирует mesh+BVH на каждый сплайн. Держать один snapshot на сессию.

#### M7. Модалка перерегистрирует multi-window handlers на каждом событии

`paint_curve_patch_edit.cc:976-979`, `wm_event_system.cc:4830-4878`. Идемпотентно через `WM_operator_is_modal`, но каждый mousemove обходит все окна. Регистрировать на invoke + notifier `WM_OT_window_new`.

#### M8. `paint_curve_geometry_test.cc` проверяет только `init_bezier`

Добавить insert/remove/cyclic/duplicate — это как раз silent-empty-patch, описанный в `curve_patch_control_curve_from_points`.

#### M9. Sampler включает `paint_curve_patch_effect.hh` только ради `CURVE_PATCH_PROFILING`

Лишняя связность. Вынести макрос в общий `paint_curve_patch_profile.hh` или в session.

---

## 6. Соответствие лучшим практикам Blender (сводка)

| Практика | Оценка | Комментарий |
| --- | --- | --- |
| Слои DNA / BKE / ED / RNA | Хорошо | Ядро без кисти; конвертация явная |
| ID users / foreach_id / blend | Хорошо | Слоты и cap-текстуры учтены |
| `CurvesGeometry` как авторитет | Хорошо | Как Curves / GPencil |
| Versioning старых файлов | Хорошо внутри ветки | Плохо снаружи (5.2 LTS identity) |
| Undo | Хорошо | Session stack обоснован knife-паттерном; commit в sculpt/image undo |
| Transform / keymap границ модулей | Смешанно | Transform — через ED_*; modal keymap — нет |
| Draw не знает editor session | Слабо | Overlay ходит в session accessors |
| Тесты на инварианты клиппинга | Смешанно | Spline/ribbon сильны; stamps/surface/tex слабы |
| Размер файлов / reviewability | Слабо | Три файла > 2000 строк; один 32k коммит |
| Const / namespaces / BLI types | Хорошо | `Span`, `Array`, `Vector`, `float3`, namespaces |
| DNA alignment / `char` вместо `bool` | Хорошо | Комментарий про makesdna padding |
| Trivially copyable list nodes | Хорошо | Слоты без non-trivial ctor из-за `BLI_duplicatelist` |
| Headless без `bContext` | Хорошо в BKE/session | Не завершено на RNA/effect (`Brush` обязателен) |

---

## 7. Рекомендуемый порядок улучшений

Порядок подобран так, чтобы каждое следующее расширение не стояло на гнилом полу. Это не план реализации и не просьба начать работу — чеклист областей.

### Волна 0 — корректность фундамента (короткие фиксы)

1. Active-index при удалении слота (C2).  
2. Снимок поверхности владеет topology + более широкая инвалидация (C1).  
3. BKE-тесты stamps / surface / texture zones (I4) — дешевле третьего раунда клиппинга.  
4. Решить идентичность файла, если `.blend` уходят с машины (C3).

### Волна 1 — развязка перед новым UX

5. Кэш overlay-батчей (I1).  
6. Modal keymap (I3).  
7. Кодировать live/frozen: один хелпер + что не в params (I8).  
8. Политика overlap Color vs Relief (I10) — до multi-curve.  
9. Lifetime сессии при teardown без mode-exit (I2).

### Волна 2 — нарезка монолитов (когда пойдут новые edit-tools)

10. Сплит `paint_curve.cc` / `paint_curve_patch_edit.cc` (I6).  
11. Общий bezier-editor над `CurvesGeometry` для двух модалок (контракт B).  
12. `PaintCurveOverlayData` вместо session accessors (I5).  
13. Публичный тип сессии вместо `void*` (I7).

### Волна 3 — расширение функционала (уже дёшево после 0–1)

14. Новый эффект / новый DNA-слайдер / Python apply без обязательной кисти (I9).  
15. Интерактивное переключение патчей (модель данных уже плюральна).  
16. Не трогать параметризацию ленты, пока LUT/frames/sampler не имеют общего domain-интерфейса.

---

## 8. Что не стоит ломать

Эти решения правильные. Рефакторинг «для красоты» здесь скорее навредит расширяемости.

- **Дублирование DNA-enum и BKE-enum** с явным switch. Склеивать кастом нельзя: ядро потеряет независимость от кисти.
- **`CurvePatchParams` как единственный вход сборки** для brush / Python / frozen session copy.
- **`curve_patch_stamp_reach` / `stamp_search_reach` / `ribbon_end_margin` как один bound.** Их снова разъехать = новый клиппинг.
- **Эффект владеет snapshot target'а, sampler — нет.** Иначе relief снова увидит сам себя.
- **Session undo, отдельный от sculpt undo.** Пытаться впихнуть control curve в `undo::Type` без слота — хуже, чем knife-стек.
- **Abort stroke undo перед модалкой.** Держать транзакцию штриха открытой на всё время edit уже пробовали — любой чужой undo её усыновляет.
- **`final_quality` как вход сборки, не как session flag.** Commit-build должен уметь вызываться без модалки.
- **`ModalViewportTracker` в `editors/util`.** Это общая инфраструктура, не деталь Curve Patch.
- **Headless apply, разделяющий publish/restamp/commit с интерактивом.** Не заводить второй «скриптовый» пайплайн.

---

## 9. Размеры модулей (ориентир, где резать)

Строки — физические, не «логика». Нужны как карта тяжести, не как KPI.

| Файл | Строк | Роль |
| --- | --- | --- |
| `paint_curve.cc` | ~3070 | Операторы PaintCurve ID, picking, slide |
| `paint_stroke_roll.cc` | ~2118 | Roll stroke → handoff в ту же сессию |
| `paint_curve_patch_edit.cc` | ~2044 | Вся live-edit семантика |
| `paint_curve_patch_effect_image.cc` | ~1308 | Image canvas effect |
| `paint_curve_geometry.cc` | ~1110 | Общая bezier-геометрия |
| `curve_patch_ribbon.cc` | ~1094 | LUT ленты (BKE) |
| `paint_curve_patch_session.cc` | ~1023 | Publish / restamp / lifetime |
| `BKE_curve_patch.hh` | ~871 | Публичный контракт ядра |
| `paint_curve_patch_effect_relief.cc` | ~834 | Relief effect |
| `overlay_paint_curve_cursor.hh` | ~649 | Overlay (header-only class, как принято в overlay) |
| `curve_patch_stamps.cc` | ~193 | Раскладка штампов, **без теста** |
| `curve_patch_surface.cc` | ~105 | Снимок + shrinkwrap, **без теста** |
| `curve_patch_texture_map.cc` | ~153 | Зоны текстуры, **без теста** |
| `paint_curve_geometry_test.cc` | ~17 | Почти пустой |
| `paint_curve_patch_session_test.cc` | ~30 | Почти пустой |

Ядро геометрии уже нарезано хорошо (spline / ribbon / frames / stamps / surface / texture_map / to_geometry — отдельные .cc). Резать нужно editor-операторы и модалку, не BKE.

Покрытие тестами ядра: spline (~925 строк тестов) и ribbon (~359) выглядят сознательно. Stamps/surface/tex — дыра ровно под те инварианты, которые комментарии называют уже обожжёнными.

---

## 10. Итог для планирования следующих улучшений

Архитектура **в целом правильная для Blender**: DNA на кисти и PaintCurve, чистое BKE-ядро ленты, editor-сессия как sibling cache, эффекты как стратегия, RNA/Python и transform по публичной границе, overlay вместо paint-cursor, WM multi-window как переиспользуемый кусок.

Готовность к расширению **высокая для «ещё один эффект / ещё один слайдер / ещё один Python apply»** и **низкая для «ещё один способ редактировать кривую / ещё один тип поверхности / другие режимы»**.

Имеет смысл улучшать в таком приоритете областей:

1. **Корректность снимка и ID-слотов** — чтобы сессия переживала чужие операторы.  
2. **Тесты stamps/surface/tex** — чтобы рефактор ленты не был вслепую.  
3. **Overlay cache + modal keymap + явный live/frozen** — чтобы следующий UX не расползался.  
4. **Нарезка модалки и общий bezier-editor** — когда пойдут multi-curve и новые жесты.  
5. **Только потом** новые крупные фичи поверх `(u,s)`-ленты.

Debug-инструментацию (`CURVE_PATCH_PROFILING` и аналоги) оставлять выключенной в коммитах; в этом ревью она не оценивалась.
