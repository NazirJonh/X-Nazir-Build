# Дизайн: per-channel blend/opacity слоёного материала

**Дата:** 18.09.2026 · **Ветка:** `REF_A-paint-layers-cleanup`
**Контекст:** возврат концепции старого графового стека, где у каждого канала была своя
Mix-нода со своим blend и фактором. В новой модели «описание = истина» blend/opacity сейчас
хранятся по одному значению на `MaterialPaintLayer`, а переключатель канала в шапке Stack
Layers был удалён как мёртвый.

## 1. Цель

Пользователь выбирает канал в шапке Stack Layers (Base Color, Roughness, …) и получает быстрый
доступ к opacity и blending mode **этого канала** у листьев, папок и коррекций. Паритет с
Substance: у каждой пары «строка × канал» свои настройки смешивания.

## 2. Модель данных

`MaterialPaintLayer` получает фиксированный массив:

```cpp
struct MaterialPaintLayerChannelSettings { int8_t blend = -1; float opacity = 1.0f; };
MaterialPaintLayerChannelSettings channel_settings[10];   // indexed by eMaterialPaintChannel
```

Значение есть у **каждой** пары «строка × канал», есть ли у неё запись канала или нет. Это даёт
один стабильный RNA-путь, переживающий создание записи, и делает возможным ключ анимации.
Sentinel `blend == -1` = «как у строки», `opacity == 1.0` = без множителя.

Sparse-запись `MaterialPaintLayerChannel` снова отвечает только за участие и карту:
`channel`, `state`, `image`, `value`. Поля `blend`/`opacity` из неё перенесены в массив (старые
`blend`/`opacity` оставлены в DNA как `DNA_DEPRECATED` только для versioning-переноса).

`MaterialPaintLayer.blend` / `.opacity` **остаются** как значения по умолчанию строки.

## 3. Наследование (значения по умолчанию)

- `blend`: `channel_settings[ch].blend < 0` → `layer.blend`; иначе значение массива.
- `opacity`: всегда `layer.opacity * channel_settings[ch].opacity` (у массива default 1.0),
  поверх `BKE_paint_layers_effective_opacity` (enabled + константная маска).
- Набор `channel_settings` не зависит от записей, поэтому показ/отрисовка ничего не создаёт
  (K-1): писать можно только значение массива, и оно уже существует.

## 4. Эффективные значения — один резолвер

```cpp
int   BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, int channel);
float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, int channel);
```

Оба читают `layer.channel_settings[channel]` и падают на значения строки. Генератор и CPU-композит
вызывают только их, поэтому граф и CPU не могут разойтись.

## 5. Запись значений

```cpp
bool BKE_paint_layers_channel_blend_set(...);   // пишет массив, REGEN; Normal — отказ
bool BKE_paint_layers_channel_opacity_set(...); // пишет массив, value-only, clamp 0..1
```

- `_blend_set` — структурная правка (blend запекается в Mix-ноду), помечает `REGEN`.
- `_opacity_set` — value-only: `BAKE_STALE` + `values_sync`, без регенерации.
- Записи канала эти сеттеры **не создают**.

## 6. RNA и Outliner seam

- `MaterialPaintLayer.channel_settings` — коллекция фиксированной длины 10 (реальный DNA-массив),
  элемент `MaterialPaintLayerChannelSettings`: `channel` (ro), `blend_type` (enum, `INHERIT` =
  -1), `opacity` (**PROP_PERCENTAGE 0..100**, getter ×100, setter ÷100 и clamp 0..1 в BKE).
- Setter'ы зовут `BKE_paint_layers_channel_*_set`. У элемента есть `path`-функция
  (`paint_layers[i].channel_settings[ch]`), поэтому `keyframe_insert`, драйверы и Copy Data Path
  работают; путь не меняется при создании записи канала. Корректность проверена Python-тестом
  `test_channel_settings_opacity_keyframe_path_resolves`.
- `PaintModeSettings.stack_layer_channel` выбирает канал, чьи колонки показывают строки.
- Колонки Outliner **всегда** указывают на `channel_settings[channel]` (обычные RNA-кнопки);
  гибридные колбэки/scratch удалены. `value_inherited`/`mode_inherited` остаются только для
  приглушения текста. Для Normal mode-колонка не выставляется, а `_blend_set` отказывает.
- `MaterialPaintLayer.opacity` (и opacity коррекций) тоже **PROP_PERCENTAGE 0..100**: DNA и BKE
  остаются 0..1, ×100/÷100 — на границе RNA. Python и ключи видят проценты.
- Generic-файлы Outliner не упоминают `Material`/`bNode`/`paint`.

## 6b. Versioning (заменяет прежний Fix 5)

Под `!DNA_struct_member_exists_with_alias(fd->filesdna, "MaterialPaintLayer",
"MaterialPaintLayerChannelSettings", "channel_settings")` рекурсивно по всем слоям (дети,
коррекции) заполняем массив `-1`/`1.0`; если запись несла свои `blend`/`opacity` (файл между
`fbb97ca61b6` и текущим), переносим их значения в массив.


## 7. Fill

При `kind_change` в Fill записи каналов **сохраняются** (ради blend/opacity), очищается только
`image`. Сейчас `BKE_paint_layers_kind_change` (`paint_layers.cc:1528`) удаляет весь массив
`channels` — это меняется.

Правило участия после смены kind: слой участвует ровно в тех каналах, которые называют его
записи (`state != ABSENT`), поэтому Paint→Fill сохраняет прежний набор каналов, но показывает в
них `fill_color` вместо карт. Обратное Fill→Paint, как и раньше, сбрасывает `fill_color` в
дефолт, но записи (blend/opacity/state) не трогает. Тест композита: Paint→Fill с `fill_color`,
равным линейному цвету карты, даёт тот же результат, что до конверсии; Fill→Paint→Fill сохраняет
записи.

## 8. Issues

`PaintLayersIssueCode::FolderHasChannels` сужается до **`FolderHasMaps`**: срабатывает, только
если у папки есть запись с `image != nullptr` (`paint_layers.cc:978`). `value` папки по-прежнему
игнорируется одинаково в генераторе и CPU.

## 9. Hashing / cache

В `BKE_paint_layers_bake_hash` включаются blend/opacity всех 10 `channel_settings` строки, чтобы
запечённая строка пересчиталась при их изменении (и для пар без записи тоже).

## 12. Авторские слои по умолчанию (Fix 1, уточнено в Fix 7)

Слой без записей каналов инертен, и ничего не подключается к Principled. Одна BKE-функция
`BKE_paint_layers_default_channels_apply(Material&, MaterialPaintLayer&)` — единственное место,
где решается набор по умолчанию:

- набор: **{Base Color, Metallic, Roughness}** — каналы, у которых есть Principled-сокет и нет
  побочного эффекта на весь материал. Alpha и Emission **не по умолчанию** (они меняют
  прозрачность/свечение всего материала), Normal/Height/AO/Custom тоже (включаются явно через
  `channel_add`);
- для каждого создаётся запись `ENABLED` без карты; повторный вызов идемпотентен;
- **Fill** получает на каждый канал своё значение из «дефолтов Principled» (`BKE_paint_layers_channel_default_value`:
  Metallic 0, Roughness 0.5, Specular 0.5, Base Color 0.8, ...); **Paint** — прозрачное (0), чтобы
  ничего не класть до мазка;
- папки/коррекции и bake-backed виды (Material, Custom) — no-op;
- `BKE_paint_layers_add` остаётся «сырым» конструктором; функцию зовут только авторские пути
  (Outliner Add, `Material.paint_layers.new`, `MATERIAL_OT_new_layered`).

**Модель Fill (как в Substance):** у Fill своё значение на каждый канал. `fill_color` — это
значение Base Color; остальные каналы берут `record.value`. `paint_layer_channel_constant`
(одна функция в `paint_layers_intern.hh`) в генераторе и CPU: Fill Base Color → `fill_color`,
прочие каналы → `record.value`; Paint → `record.value` (пусто у свежего). Конверсия Fill→Paint
переносит текущую константу каждого канала в его запись (`BKE_paint_layers_fill_to_paint`), так
рендер сохраняется. Значение канала Fill — value-only (`paint_layers_tag_value_only` + `values_sync`),
без `REGEN`.

**Граница UI:** цвет из пикера Add/Fill-Color — `PROP_COLOR_GAMMA`; на границе оператора
(`outliner_stack_layers.cc`) он конвертируется `srgb_to_linearrgb_v4` в scene-linear, которым
описание и оперирует. Обратно swatch→пикер — `linearrgb_to_srgb_v4`.

Ограничение: Solid/Workbench `TEXTURE` Fill без карты не показывает (нет пикселей); виден в
Material Preview/render. Решение — композитная превью-карта — вне объёма.

## 10. Тесты

C++:

- set на строке без записи создаёт **ровно одну** запись; `_blend_set` ставит `REGEN`,
  `_opacity_set` — нет;
- генератор и CPU дают одинаковый результат с записью `blend == -1` и без записи;
- graph_eval: папка с переопределённым blend в одном канале и наследованием в другом; opacity
  канала у листа и у папки; вложенная папка;
- issues: `FolderHasMaps` только при наличии карты;
- `paint_stack_rows_from_description` не меняет описание (хэш до/после отрисовки).

## 11. Вне объёма / открытые вопросы

- Versioning не делаем: старые `.blend` форка не сохраняем (решение принято ранее).
- Per-channel blend для коррекций — по общей модели (запись канала), отдельных полей не заводим.
- Многоканальный projection — по-прежнему бэклог.
- **Отдельная задача: alpha вложенных папок.** В композите вложенной папки граф и CPU
  расходятся по alpha (покрытию); существующие тесты граф≡CPU сравнивают только RGB. Пока не
  выяснено, какая сторона верна по формуле `P/a` (дизайн §5), это известное расхождение, а не
  «граф ≡ CPU»; покрытие из него идёт в bake и Combined, поэтому задачу надо закрыть до опоры на
  bake вложенных папок.
