# Per-channel blend/opacity (paint layers) — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Дать каждой паре «строка × канал» свои blend и opacity, выбираемые каналом в шапке Stack Layers.

**Architecture:** `MaterialPaintLayerChannel` несёт `blend`/`opacity` как override; значения строки остаются дефолтами. Один BKE-резолвер читают генератор, CPU и UI; запись создаётся только при правке значения (гибридные колонки Outliner). Дизайн: `docs/plans/2026-09-18-paint-layers-per-channel-blend-opacity-design.md`.

**Tech Stack:** C++ (BKE/ED/RNA), Blender gtest, CMake/VS.

**Сборка/тесты (только затронутые таргеты):**
```
$b="M:\Blender_Make_Again\build_windows_Full_Tests_x64_vc17_Release"
$a="M:\Blender_Make_Again\blender\tests\files"
cmake --build $b --config Release --parallel 4 --target <t>
& "$b\bin\tests\Release\<t>.exe" --test-assets-dir "$a" --test-release-dir "$b\bin\Release"
```
Арбитр: `blenkernel_paint_layers_graph_eval_test`. Ветка `REF_A-paint-layers-cleanup`, коммиты `REF_A`.

---

## Task 1: DNA — per-channel blend/opacity

**Files:** Modify `source/blender/makesdna/DNA_material_types.h` (`MaterialPaintLayerChannel`).

**Step 1:** Добавить поля:
```cpp
  /** #eMaterialPaintLayerBlend for this channel, or -1 to inherit the row's blend. */
  int8_t blend = -1;
  char _pad_blend[5] = {};   /* was _pad[6] */
  ...
  /** Multiplied by the row's opacity; 1.0 when the channel has no override. */
  float opacity = 1.0f;
  char _pad_channel[4] = {};
```
(текущий `_pad[6]` → `_pad_blend[5]`; после `value[4]` — `opacity` + `_pad_channel[4]`, чтобы размер остался кратен 8).

**Step 2:** Собрать DNA: `cmake --build $b --config Release --parallel 4 --target bf_dna` — ждём успех (makesdna проверит выравнивание).

**Step 3:** Commit: `REF_A: per-channel blend/opacity DNA fields`.

---

## Task 2: BKE — эффективные значения и сеттеры

**Files:** Modify `source/blender/blenkernel/BKE_paint_layers.hh`, `intern/paint_layers.cc`.

**Step 1:** Объявить:
```cpp
int   BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, int channel);
float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, int channel);
bool  BKE_paint_layers_channel_blend_set(Material &ma, MaterialPaintLayer &layer, int channel, int blend);
bool  BKE_paint_layers_channel_opacity_set(Material &ma, MaterialPaintLayer &layer, int channel, float opacity);
```

**Step 2:** Реализовать через существующий `paint_layer_channel_find`:
- `blend_effective`: запись отсутствует или `record->blend < 0` → `layer.blend`; иначе `record->blend`.
- `opacity_effective`: `layer.opacity * (record ? record->opacity : 1.0f)`.
- `_blend_set`: `BKE_paint_layers_channel_add` при отсутствии; `record->blend = blend`; `paint_layers_tag_edited` (REGEN).
- `_opacity_set`: `channel_add` при отсутствии; `record->opacity = opacity`; `BKE_paint_layers_values_sync(ma)` без REGEN.

**Step 3:** Тест в `intern/paint_layers_target_test.cc` (новая секция):
- set на строке без записи создаёт ровно одну запись;
- `blend == -1` и отсутствие записи дают одинаковый эффективный результат;
- `_opacity_set` не ставит `MA_PAINT_LAYERS_REGEN`, `_blend_set` ставит.

**Step 4:** Build+run `blenkernel_paint_layers_target_test` (ожидаем PASS).

**Step 5:** Commit: `REF_A: per-channel blend/opacity resolvers and setters`.

---

## Task 3: FolderHasMaps

**Files:** Modify `BKE_paint_layers.hh` (enum `PaintLayersIssueCode`), `intern/paint_layers.cc:975-990`, issues-тест.

**Step 1:** Переименовать `FolderHasChannels` → `FolderHasMaps` (обновить все вхождения).
**Step 2:** Условие: срабатывает, только если есть запись с `image != nullptr`.
**Step 3:** Тест: папка с записью без карты — issue нет; с картой — есть.
**Step 4:** Build+run `blenkernel_paint_layers_description_test`, commit.

---

## Task 4: Генератор

**Files:** `intern/paint_layers_generate.cc` (листы ~1350, ~1480; коррекции ~941, ~1190; папки).

**Step 1:** В per-channel цикле заменить `eMaterialPaintLayerBlend(layer->blend)` и `BKE_paint_layers_effective_opacity(*layer)` на резолверы:
```cpp
const int blend = BKE_paint_layers_channel_blend_effective(*layer, channel);
const float op  = BKE_paint_layers_channel_opacity_effective(*layer, channel);
```
Для папок и коррекций — тот же резолвер (записи может не быть, тогда значения строки).

**Step 2:** `values_sync` (`BKE_paint_layers_values_sync`) должен писать эффективную opacity канала в соответствующий group input.

**Step 3:** Build+run `blenkernel_paint_layers_generate_test`, `blenkernel_paint_layers_graph_eval_test`.

**Step 4:** Добавить кейсы в `paint_layers_graph_eval_test.cc`:
- папка: override blend в одном канале, наследование в другом → граф ≡ CPU;
- opacity канала у листа и у папки;
- вложенная папка.

**Step 5:** Commit.

---

## Task 5: CPU-композит

**Files:** `intern/paint_layers_composite.cc`.

**Step 1:** Те же резолверы вместо `layer->blend` / `layer->opacity`.
**Step 2:** Build+run `blenkernel_paint_layers_graph_eval_test` (арбитр) и `blenkernel_paint_material_combined_*`.
**Step 3:** Commit.

---

## Task 6: Bake hash

**Files:** `intern/paint_layers_bake.cc` (`BKE_paint_layers_bake_hash`).

**Step 1:** В хэш добавить по каждому каналу `record->blend` и `record->opacity`.
**Step 2:** Build+run `blenkernel_paint_material_combined_cache_test`, commit.

---

## Task 7: Fill сохраняет записи

**Files:** `intern/paint_layers.cc` (`BKE_paint_layers_kind_change`, ~1528).

**Step 1:** Вместо `MEM_delete(layer->channels)` — пройти записи и обнулить только `image` (и `state` → `ABSENT`?), сохранив blend/opacity.
**Step 2:** Тест в `paint_layers_description_test.cc`: Fill→Paint и Paint→Fill сохраняют blend/opacity; карта очищается.
**Step 3:** Build+run description/graph_eval, commit.

---

## Task 8: RNA

**Files:** `makesrna/intern/rna_material.cc` (`rna_def_material_paint_layer_channel`), `rna_sculpt_paint.cc`.

**Step 1:** `MaterialPaintLayerChannel.blend_type` — enum с пунктом `INHERIT` = -1; `.opacity` — float.
**Step 2:** Вернуть `PaintModeSettings.stack_layer_channel` (enum `rna_enum_material_paint_channel_items`, update `NC_SPACE | ND_SPACE_OUTLINER`).
**Step 3:** Build `bf_rna`; commit.

---

## Task 9: Outliner seam (гибридные колонки)

**Files:** `editors/space_outliner/outliner_stack_source.hh` (`StackRow`), `outliner_draw.cc:2653-2697`, `outliner_stack_source_paint_layers.cc` (~435-446).

**Step 1:** `StackRow`: generic `value_ptr/prop`, `mode_ptr/prop` (если запись есть), `inherited` флаг и `get/set`-колбэки (если записи нет). Без `Material`/`paint`.
**Step 2:** `outliner_draw`: если RNA-указатель есть — `uiDefAutoButR` как сейчас; иначе кнопка с `UI_BUT_UNDO` + `UI_but_func_set`, зовущая колбэки источника.
**Step 3:** `paint_stack_rows_from_description`: для выбранного `layer_target_mode`/`stack_layer_channel` строить указатель на запись, если есть; иначе — колбэки и `inherited=true`. Отрисовка **не меняет** описание.
**Step 4:** Тесты в `outliner_stack_source_paint_layers_test.cc`:
- set на строке без записи создаёт одну запись и снимает `inherited`;
- хэш описания до/после `paint_stack_rows_from_description` одинаков.
**Step 5:** Build+run `editor_space_outliner_outliner_stack_source_paint_layers_test`; grep generic-файлов на `paint`/`Material`/`bNode`. Commit.

---

## Task 10: Ручная оконная проверка (пользователь)

- Выбор канала в шапке; перетаскивание opacity/channel blend у листа, папки, коррекции.
- Первая правка унаследованной строки — один Ctrl+Z.
- После первой правки на колонке работает I (ключ).
- Граф ≡ CPU (EEVEE).
