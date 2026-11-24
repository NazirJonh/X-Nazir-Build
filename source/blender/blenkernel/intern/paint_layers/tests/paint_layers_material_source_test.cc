/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_appdir.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_main_namemap.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"

namespace blender::bke::tests {

/** A layered material, the materials it can read and the rows that read them. */
class PaintLayersMaterialSource : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Material *layered_material_add(const char *name)
  {
    Material *ma = BKE_material_add(bmain, name);
    ma->paint_layers_flag = MA_PAINT_LAYERED;
    return ma;
  }

  /** Add a row of \a owner that reads \a source; null when the Add is refused. */
  MaterialPaintLayer *material_row_add(Material &owner,
                                       Material &source,
                                       MaterialPaintLayer *anchor = nullptr)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Material;
    params.source = &source;
    params.anchor = anchor;
    return BKE_paint_layers_add_with_policy(owner, params);
  }

  /** Move \a id into \a lib, keeping the name map consistent. */
  void id_move_to_library(ID &id, Library *lib)
  {
    BKE_main_namemap_remove_id(*bmain, id);
    id.lib = lib;
    id.tag |= ID_TAG_EXTERN;
    BKE_main_namemap_get_unique_name(*bmain, id, id.name + 2);
  }
};

TEST_F(PaintLayersMaterialSource, a_material_row_reads_its_source_and_owns_no_record)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Rust");
  const int users_before = source->id.us;

  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(row->material, source);
  EXPECT_EQ(source->id.us, users_before + 1);
  /* The row goes by the name of what it reads. */
  EXPECT_STREQ(row->name, "Rust");

  const PaintLayerKindInfo *kind = paint_layer_kind_of(*row);
  ASSERT_NE(kind, nullptr);
  EXPECT_STREQ(kind->name, "Material");
  EXPECT_TRUE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_READS_SOURCE));
  EXPECT_TRUE(paint_layer_kind_role_allowed(*row));

  /* It owns no channel record, and a record added by hand would be one the kind never reads. */
  EXPECT_EQ(row->channels_num, 0);
  EXPECT_EQ(BKE_paint_layers_channel_add(*owner, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_EQ(row->channels_num, 0);
  EXPECT_FALSE(BKE_paint_layers_row_is_paintable(*row));
}

TEST_F(PaintLayersMaterialSource, an_add_with_a_database_reads_the_source_itself)
{
  Material *owner = layered_material_add("Owner");
  Material *source = layered_material_add("Source");
  MaterialPaintLayer *source_row = BKE_paint_layers_add(
      *source, "Rust", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(source_row, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Material;
  params.source = source;
  /* The Add popovers pass the database the payload kinds need. A Material row still reads the
   * material it was given, not a copy: the picked material's stack is the live one, so a row added
   * to it afterwards is what the row -- and its source-row picker -- sees. */
  params.bmain = bmain;
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*owner, params);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->material, source);

  MaterialPaintLayer *added = BKE_paint_layers_add(
      *source, "Added", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(added, nullptr);
  EXPECT_EQ(BKE_paint_layers_source_row_find(*row->material, added->marker), added);
  EXPECT_EQ(BKE_paint_layers_source_row_find(*row->material, source_row->marker), source_row);
}

TEST_F(PaintLayersMaterialSource, a_narrowed_row_keeps_both_canvas_modes)
{
  Material *owner = layered_material_add("Owner");
  Material *source = layered_material_add("Source");
  MaterialPaintLayer *source_row = BKE_paint_layers_add(
      *source, "Rust", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(source_row, nullptr);
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  row->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_ROW;
  row->material_row_marker = source_row->marker;
  EXPECT_TRUE(BKE_paint_layers_material_row_narrowed(*row));

  /* Live is the default. A narrowed row has no live source graph, so Live reads the referenced
   * row's render at the row's UV -- it is not a canvas, but it still renders. */
  row->material_canvas_mode = MA_PAINT_LAYER_CANVAS_MODE_LIVE;
  EXPECT_FALSE(BKE_paint_layers_material_row_is_canvas(*row));
  EXPECT_TRUE(BKE_paint_layers_material_row_narrowed(*row));

  /* Canvas is a request for the canvas read; the scope stays narrowed. */
  row->material_canvas_mode = MA_PAINT_LAYER_CANVAS_MODE_CANVAS;
  EXPECT_TRUE(BKE_paint_layers_material_row_is_canvas(*row));
  EXPECT_TRUE(BKE_paint_layers_material_row_narrowed(*row));

  /* A FULL row's mode keeps its old meaning. */
  row->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_MATERIAL;
  EXPECT_TRUE(BKE_paint_layers_material_row_is_canvas(*row));
  row->material_canvas_mode = MA_PAINT_LAYER_CANVAS_MODE_LIVE;
  EXPECT_FALSE(BKE_paint_layers_material_row_is_canvas(*row));
}

TEST_F(PaintLayersMaterialSource, a_linked_source_can_be_made_local_and_the_row_follows)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Rust");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  Library *lib = BKE_id_new<Library>(bmain, "SourceLibrary");
  id_move_to_library(source->id, lib);
  ASSERT_TRUE(ID_IS_LINKED(&source->id));

  Material *local = BKE_paint_layers_material_source_make_local(*bmain, *owner, row);
  ASSERT_NE(local, nullptr);
  EXPECT_FALSE(ID_IS_LINKED(&local->id));
  EXPECT_EQ(local->id.lib, nullptr);
  /* Localizing may strip the library in place or copy and remap; either way the row reads the
   * local material, never the linked one. */
  EXPECT_EQ(row->material, local);
  EXPECT_EQ(BKE_paint_layers_material_source_make_local(*bmain, *owner, row), nullptr)
      << "A source that is already local has nothing to localize";
}

TEST_F(PaintLayersMaterialSource, an_add_without_a_usable_source_is_refused)
{
  Material *owner = layered_material_add("Owner");
  Material *reader = layered_material_add("Reader");
  ASSERT_NE(material_row_add(*reader, *owner), nullptr);

  PaintLayerAddParams none;
  none.kind = PaintLayerAddKind::Material;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*owner, none), nullptr);

  /* The owner itself, and a material that already reads the owner: the pair would read each
   * other. Nothing is left behind by a refused Add. */
  EXPECT_EQ(material_row_add(*owner, *owner), nullptr);
  EXPECT_EQ(material_row_add(*owner, *reader), nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&owner->paint_layers));
}

TEST_F(PaintLayersMaterialSource, the_dependency_follows_rows_nested_in_folders)
{
  Material *a = layered_material_add("A");
  Material *b = layered_material_add("B");
  Material *c = layered_material_add("C");

  PaintLayerAddParams folder_params;
  folder_params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(*a, folder_params);
  ASSERT_NE(folder, nullptr);
  PaintLayerAddParams inner;
  inner.kind = PaintLayerAddKind::Material;
  inner.source = b;
  inner.anchor = folder;
  inner.place = PaintLayerPlace::Into;
  ASSERT_NE(BKE_paint_layers_add_with_policy(*a, inner), nullptr);

  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*a, *b));
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*a, *a));
  EXPECT_FALSE(BKE_paint_layers_material_depends_on(*b, *a));
  EXPECT_FALSE(BKE_paint_layers_material_depends_on(*a, *c));

  /* B may not read A, which reads B through the folder, nor C once C reads A. */
  EXPECT_EQ(material_row_add(*b, *a), nullptr);
  ASSERT_NE(material_row_add(*c, *a), nullptr);
  EXPECT_EQ(material_row_add(*b, *c), nullptr);
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*c, *b));

  /* The refusal a UI explains is the same rule the Add applies, named. */
  EXPECT_EQ(BKE_paint_layers_material_source_issue(*a, *a), PaintLayerMaterialSourceIssue::Self);
  EXPECT_EQ(BKE_paint_layers_material_source_issue(*b, *a), PaintLayerMaterialSourceIssue::Cycle);
  EXPECT_EQ(BKE_paint_layers_material_source_issue(*a, *b), PaintLayerMaterialSourceIssue::None);
}

TEST_F(PaintLayersMaterialSource, set_material_swaps_the_users_and_refuses_what_cannot_be_read)
{
  Material *owner = layered_material_add("Owner");
  Material *first = BKE_material_add(bmain, "First");
  Material *second = BKE_material_add(bmain, "Second");
  MaterialPaintLayer *row = material_row_add(*owner, *first);
  ASSERT_NE(row, nullptr);
  const int first_users = first->id.us;
  const int second_users = second->id.us;

  ASSERT_TRUE(BKE_paint_layers_set_material(*owner, row, second));
  EXPECT_EQ(row->material, second);
  EXPECT_EQ(first->id.us, first_users - 1);
  EXPECT_EQ(second->id.us, second_users + 1);

  /* The same source again changes nothing, users included. */
  ASSERT_TRUE(BKE_paint_layers_set_material(*owner, row, second));
  EXPECT_EQ(second->id.us, second_users + 1);

  EXPECT_FALSE(BKE_paint_layers_set_material(*owner, row, nullptr));
  EXPECT_FALSE(BKE_paint_layers_set_material(*owner, row, owner));
  EXPECT_EQ(row->material, second);

  /* Only a Material row has a source to set, and it is never turned into another kind. */
  PaintLayerAddParams paint_params;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(*owner, paint_params);
  ASSERT_NE(paint, nullptr);
  EXPECT_FALSE(BKE_paint_layers_set_material(*owner, paint, first));
  EXPECT_FALSE(BKE_paint_layers_set_source(*owner, row, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_FALSE(BKE_paint_layers_set_source(*owner, row, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
}

TEST_F(PaintLayersMaterialSource, removing_the_row_lets_go_of_the_source)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  const int users_before = source->id.us;
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);
  ASSERT_EQ(source->id.us, users_before + 1);

  ASSERT_TRUE(BKE_paint_layers_remove(*owner, row));
  EXPECT_EQ(source->id.us, users_before);
}

TEST_F(PaintLayersMaterialSource, a_duplicate_of_the_row_reads_the_same_source)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);
  const int users_before = source->id.us;

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *owner, row);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  /* The duplicate reads a copy of the source, so the two rows keep settings of their own; the
   * original's source is untouched (see #paint_layer_material_duplicate). Sharing still happens
   * when the whole owner is copied, which `a_copy_of_the_owner_reads_the_same_source` covers. */
  EXPECT_NE(copy->material, source);
  EXPECT_EQ(copy->material->id.us, 1);
  EXPECT_EQ(source->id.us, users_before);
  EXPECT_EQ(copy->channels_num, 0);
}

TEST_F(PaintLayersMaterialSource, a_copy_of_the_owner_reads_the_same_source)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  ASSERT_NE(material_row_add(*owner, *source), nullptr);
  const int users_before = source->id.us;

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &owner->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(BLI_listbase_count(&copy->paint_layers), 1);
  const MaterialPaintLayer &copied_row = *copy->paint_layers.begin();
  EXPECT_EQ(copied_row.material, source);
  EXPECT_EQ(source->id.us, users_before + 1);
}

TEST_F(PaintLayersMaterialSource, the_dependencies_and_the_id_walk_report_the_source)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  Vector<PaintLayerDependency> found;
  paint_layer_dependencies(*row, [&](const PaintLayerDependency &dep) { found.append(dep); });
  ASSERT_EQ(found.size(), 1);
  EXPECT_EQ(found[0].kind, PaintLayerDependencyKind::SourceMaterial);
  EXPECT_EQ(found[0].slot, reinterpret_cast<ID **>(&row->material));
  EXPECT_EQ(found[0].channel, -1);
  EXPECT_EQ(paint_layer_dependency_foreach_flag(found[0]), IDWALK_CB_USER);

  Vector<ID *> visited;
  auto collector = [](LibraryIDLinkCallbackData *data) -> int {
    if (*data->id_pointer != nullptr) {
      static_cast<Vector<ID *> *>(data->user_data)->append(*data->id_pointer);
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(bmain, &owner->id, collector, &visited, IDWALK_READONLY);
  EXPECT_TRUE(visited.contains(&source->id));

  /* A row that reads no material reports none. */
  PaintLayerAddParams paint_params;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(*owner, paint_params);
  ASSERT_NE(paint, nullptr);
  int paint_ids = 0;
  paint_layer_dependencies(*paint, [&](const PaintLayerDependency &dep) {
    paint_ids += dep.kind == PaintLayerDependencyKind::SourceMaterial ? 1 : 0;
  });
  EXPECT_EQ(paint_ids, 0);
}

TEST_F(PaintLayersMaterialSource, a_material_row_is_not_a_stroke_target)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);
  BKE_paint_layers_active_set(*owner, row->marker);

  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *owner, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_EQ(BKE_paint_layers_active_maps_ensure(*bmain, *owner, 4), 0);
}

TEST_F(PaintLayersMaterialSource, the_bake_planner_leaves_a_material_row_alone)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  /* A cache of the row's own subtree is not what a Material row shows: asking for one changes
   * nothing. */
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*owner, *row, MA_PAINT_LAYER_BAKE_ALWAYS));
  BKE_paint_layers_bake_request(*row);
  bool changed = false;
  BKE_paint_layers_bake_plan_run(*bmain, *owner, &changed);
  EXPECT_FALSE(changed);
  /* A row with nothing to render is as current as it can be, so the validity check passes it; what
   * the planner must not do is mint a cache of maps for it. */
  EXPECT_EQ(row->bake == nullptr ? nullptr : row->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR],
            nullptr);
  EXPECT_EQ(row->bake == nullptr ? nullptr : row->bake->coverage, nullptr);
}

TEST_F(PaintLayersMaterialSource, a_deleted_source_leaves_the_row_without_one)
{
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Source");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  BKE_id_delete(bmain, source);
  EXPECT_EQ(row->material, nullptr);
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
}

TEST_F(PaintLayersMaterialSource, blend_round_trip_keeps_the_source)
{
  Material *owner = layered_material_add("RoundTripOwner");
  id_us_plus(&owner->id);
  Material *source = BKE_material_add(bmain, "RoundTripSource");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_material_source.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded_owner = nullptr;
  Material *reloaded_source = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "RoundTripOwner")) {
      reloaded_owner = &candidate;
    }
    else if (STREQ(candidate.id.name + 2, "RoundTripSource")) {
      reloaded_source = &candidate;
    }
  }
  ASSERT_NE(reloaded_owner, nullptr);
  ASSERT_NE(reloaded_source, nullptr);
  ASSERT_EQ(BLI_listbase_count(&reloaded_owner->paint_layers), 1);
  const MaterialPaintLayer &reloaded_row = *reloaded_owner->paint_layers.begin();
  EXPECT_EQ(reloaded_row.source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  /* The reference is remapped on read, not left as the address the file was written with. */
  EXPECT_EQ(reloaded_row.material, reloaded_source);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersMaterialSource, the_three_predicates_agree_on_the_four_modes)
{
  /* The one test of "what the row lays" is #BKE_paint_layers_material_row_reads_render: the canvas
   * and the narrowed questions answer narrower things, and no reader may pair them by hand. The
   * table is every combination of scope and mode a Material row can hold. */
  struct Expect {
    int scope;
    int mode;
    bool canvas;
    bool narrowed;
    bool reads_render;
  };
  const Expect table[] = {
      {MA_PAINT_LAYER_MATERIAL_SCOPE_MATERIAL, MA_PAINT_LAYER_CANVAS_MODE_LIVE, false, false, false},
      {MA_PAINT_LAYER_MATERIAL_SCOPE_MATERIAL,
       MA_PAINT_LAYER_CANVAS_MODE_CANVAS,
       true,
       false,
       true},
      {MA_PAINT_LAYER_MATERIAL_SCOPE_ROW, MA_PAINT_LAYER_CANVAS_MODE_LIVE, false, true, true},
      {MA_PAINT_LAYER_MATERIAL_SCOPE_ROW, MA_PAINT_LAYER_CANVAS_MODE_CANVAS, true, true, true},
  };

  Material *owner = layered_material_add("Owner");
  Material *source = layered_material_add("Source");
  MaterialPaintLayer *source_row = BKE_paint_layers_add(
      *source, "Rust", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(source_row, nullptr);
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  for (const Expect &expect : table) {
    row->material_scope = int8_t(expect.scope);
    row->material_canvas_mode = int8_t(expect.mode);
    if (expect.narrowed) {
      row->material_row_marker = source_row->marker;
    }
    EXPECT_EQ(BKE_paint_layers_material_row_is_canvas(*row), expect.canvas)
        << "scope " << expect.scope << " mode " << expect.mode;
    EXPECT_EQ(BKE_paint_layers_material_row_narrowed(*row), expect.narrowed)
        << "scope " << expect.scope << " mode " << expect.mode;
    EXPECT_EQ(BKE_paint_layers_material_row_reads_render(*row), expect.reads_render)
        << "scope " << expect.scope << " mode " << expect.mode;
  }
}

TEST_F(PaintLayersMaterialSource, clearing_the_source_slot_lets_go_of_the_reference_and_maps)
{
  /* Clear is a legal "no source chosen" state, not the loss of a reference: the material and the
   * marker go, the maps of the old source do not outlive them, and the status reads OK for the empty
   * slot rather than the stale/gone words of a reference that failed. */
  Material *owner = layered_material_add("Owner");
  Material *source = BKE_material_add(bmain, "Rust");
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);
  ASSERT_EQ(row->material, source);

  ASSERT_TRUE(BKE_paint_layers_material_clear(*owner, row));
  EXPECT_EQ(row->material, nullptr);
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_TRUE(BLI_uuid_is_nil(row->material_row_marker));
  EXPECT_EQ(BKE_paint_layers_material_source_status(*owner, *row),
            MA_PAINT_LAYER_SOURCE_STATUS_OK);

  /* Clearing an already-empty slot is a no-op that still reports success. */
  EXPECT_TRUE(BKE_paint_layers_material_clear(*owner, row));
}

TEST_F(PaintLayersMaterialSource, a_fill_flip_keeps_the_picked_material_and_its_view)
{
  /* Maps | Material is a view over one row, not two rows: the picked material and the row's own
   * view of it stay while the Fill lays its own content, so switching back brings them back as
   * they were. */
  Material *owner = layered_material_add("Owner");
  Material *source = layered_material_add("Source");
  MaterialPaintLayer *source_row = BKE_paint_layers_add(
      *source, "Rust", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(source_row, nullptr);
  MaterialPaintLayer *row = material_row_add(*owner, *source);
  ASSERT_NE(row, nullptr);

  row->material_scope = MA_PAINT_LAYER_MATERIAL_SCOPE_ROW;
  row->material_row_marker = source_row->marker;
  row->material_canvas_mode = MA_PAINT_LAYER_CANVAS_MODE_CANVAS;
  row->material_canvas_aspect = MA_PAINT_LAYER_CANVAS_ASPECT_CUSTOM;
  row->material_canvas_width = 512;
  row->material_canvas_height = 256;
  row->source_refresh = MA_PAINT_LAYER_SOURCE_REFRESH_MANUAL;
  const int users_before = source->id.us;

  /* Maps is the Fill view: the row lays its own content, the material only kept. */
  ASSERT_TRUE(BKE_paint_layers_fill_source_set(*owner, row, false));
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_EQ(row->material, source);
  EXPECT_EQ(source->id.us, users_before);
  /* The held pointer is a dependency the cycle walk follows, not only the Material view: another
   * material may not read the owner while this Fill holds the source. */
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*owner, *source));
  EXPECT_EQ(material_row_add(*source, *owner), nullptr);
  EXPECT_EQ(BKE_paint_layers_material_source_issue(*source, *owner),
            PaintLayerMaterialSourceIssue::Cycle);

  /* Back to Material: the source is read again with the view it had. */
  ASSERT_TRUE(BKE_paint_layers_fill_source_set(*owner, row, true));
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(row->material, source);
  EXPECT_EQ(source->id.us, users_before);
  EXPECT_EQ(row->material_scope, MA_PAINT_LAYER_MATERIAL_SCOPE_ROW);
  EXPECT_TRUE(BLI_uuid_equal(row->material_row_marker, source_row->marker));
  EXPECT_EQ(row->material_canvas_mode, MA_PAINT_LAYER_CANVAS_MODE_CANVAS);
  EXPECT_EQ(row->material_canvas_aspect, MA_PAINT_LAYER_CANVAS_ASPECT_CUSTOM);
  EXPECT_EQ(row->material_canvas_width, 512);
  EXPECT_EQ(row->material_canvas_height, 256);
  EXPECT_EQ(row->source_refresh, MA_PAINT_LAYER_SOURCE_REFRESH_MANUAL);
}

}  // namespace blender::bke::tests
