/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"

namespace blender {

class IndexMask;
struct BMesh;
struct BMFace;
struct BMVert;
struct Mesh;
struct Object;
struct SubdivCCG;
struct SubdivCCGCoord;

struct bContext;
struct Depsgraph;
struct Brush;
struct MTex;
struct SculptSession;

namespace ed::sculpt_paint {
struct StrokeCache;
}

namespace ed::sculpt_paint::face_set {

/**
 * Per-stroke cache mapping quantized custom colors to Face Set IDs, owned by #StrokeCache.
 * The full definition lives here because #StrokeCache holds it through a unique_ptr and must see
 * the complete type to destroy it.
 */
struct FaceSetColorStrokeCache {
  bool enabled = false;
  const MTex *color_mtex = nullptr;

  Map<uint32_t, int> mesh_color_to_id;
  Map<uint32_t, int> stroke_color_to_id;
  bool mesh_geometry_tagged = false;
  /** Next ID to assign; initialized once per stroke from mesh data. */
  int next_face_set_id = 0;

  void clear();
  void preload_mesh_colors(const Mesh &mesh);
  int ensure_face_set_id_for_quant_color(Object &object, const float quant[3]);
};

/** Copy mask/alpha mapping (offset, scale, angle, mode) onto #Brush.face_set_color_mtex. */
void sync_face_set_color_mtex_mapping_from_mask(Brush &brush);
/** Build the per-stroke color cache once when color texture mode is active. */
void face_set_color_stroke_cache_init(StrokeCache &cache, const Brush &brush, const Mesh &mesh);
void face_set_color_stroke_cache_clear(StrokeCache &cache);

int active_face_set_get(const Object &object);
/* TODO: vert_face_set_max_get should likely be avoided and existing usages cleaned up, since by
 * definition, a vertex can be associated to more than a single face set. */
int vert_face_set_max_get(GroupedSpan<int> vert_to_face_map, Span<int> face_sets, int vert);
int active_update_and_get(bContext *C, Object &ob, const float mval[2]);

/** Sample the face set under the cursor into brush color and #face_set_sample_id. */
void sample_face_set_color_at_active(const Object &object, Brush &brush);

/** Update cursor picking and sample the face set color at \a mval. Returns false on miss. */
bool sample_face_set_color_at_cursor(bContext *C,
                                     Object &object,
                                     Brush &brush,
                                     const float mval[2]);

int vert_face_set_get(GroupedSpan<int> vert_to_face_map, Span<int> face_sets, int vert);
int vert_face_set_get(const SubdivCCG &subdiv_ccg, Span<int> face_sets, int grid);
int vert_face_set_max_get(int face_set_offset, const BMVert &vert);

Set<int> vert_face_sets_get(GroupedSpan<int> vert_to_face_map, Span<int> face_sets, int vert);

bool vert_has_face_set(GroupedSpan<int> vert_to_face_map,
                       Span<int> face_sets,
                       int vert,
                       int face_set);
/* TODO: audit the uses of vert_has_face_set for Multires. */
bool vert_has_face_set(const SubdivCCG &subdiv_ccg, Span<int> face_sets, int grid, int face_set);
bool vert_has_face_set(int face_set_offset, const BMVert &vert, int face_set);

bool vert_has_any_face_set(GroupedSpan<int> vert_to_face_map,
                           Span<int> face_sets,
                           int vert,
                           const Set<int> &allowed_face_sets);

bool vert_has_unique_face_set(GroupedSpan<int> vert_to_face_map, Span<int> face_sets, int vert);
bool vert_has_unique_face_set(OffsetIndices<int> faces,
                              Span<int> corner_verts,
                              GroupedSpan<int> vert_to_face_map,
                              Span<int> face_sets,
                              const SubdivCCG &subdiv_ccg,
                              SubdivCCGCoord coord);
bool vert_has_unique_face_set(int face_set_offset, const BMVert &vert);
bool coord_has_face_set(OffsetIndices<int> faces,
                        Span<int> corner_verts,
                        GroupedSpan<int> vert_to_face_map,
                        Span<int> face_sets,
                        const SubdivCCG &subdiv_ccg,
                        SubdivCCGCoord coord,
                        int face_set);
bool coord_has_any_face_set(OffsetIndices<int> faces,
                            Span<int> corner_verts,
                            GroupedSpan<int> vert_to_face_map,
                            Span<int> face_sets,
                            const SubdivCCG &subdiv_ccg,
                            SubdivCCGCoord coord,
                            const Set<int> &allowed_face_sets);

constexpr float FACE_SET_MIN_FADE = 0.05f;

void fill_factor_from_hide_and_mask(const BMesh &bm,
                                    const Set<BMFace *, 0L> &faces,
                                    const MutableSpan<float> r_factors);
void fill_factor_from_hide_and_mask(const Mesh &mesh,
                                    const Span<int> face_indices,
                                    const MutableSpan<float> r_factors);
void calc_face_centers(const OffsetIndices<int> faces,
                       const Span<int> corner_verts,
                       const Span<float3> vert_positions,
                       const Span<int> face_indices,
                       const MutableSpan<float3> positions);
void calc_face_centers(const Set<BMFace *, 0L> &faces, const MutableSpan<float3> centers);
void calc_face_indices_grids(const SubdivCCG &subdiv_ccg,
                             const Span<int> grids,
                             const MutableSpan<int> &face_indices);

/**
 * Creates the sculpt face set attribute on the mesh if it doesn't exist.
 *
 * \see face_set::ensure_face_sets_mesh if further writing to the attribute is desired.
 */
bool create_face_sets_mesh(Object &object);

int find_next_available_id(Object &object);
void initialize_none_to_id(Mesh *mesh, int new_id);

/**
 * Ensures that the sculpt face set attribute exists on the mesh.
 *
 * \see face_set::create_face_sets_mesh to avoid having to remember to call .finish()
 */
bke::SpanAttributeWriter<int> ensure_face_sets_mesh(Mesh &mesh);
int ensure_face_sets_bmesh(Object &object);
Array<int> duplicate_face_sets(const Mesh &mesh);
Set<int> gather_hidden_face_sets(Span<bool> hide_poly, Span<int> face_sets);

void filter_verts_with_unique_face_sets_mesh(GroupedSpan<int> vert_to_face_map,
                                             Span<int> face_sets,
                                             bool unique,
                                             Span<int> verts,
                                             MutableSpan<float> factors);
void filter_verts_with_unique_face_sets_grids(OffsetIndices<int> faces,
                                              Span<int> corner_verts,
                                              GroupedSpan<int> vert_to_face_map,
                                              Span<int> face_sets,
                                              const SubdivCCG &subdiv_ccg,
                                              bool unique,
                                              Span<int> grids,
                                              MutableSpan<float> factors);
void filter_verts_with_unique_face_sets_bmesh(int face_set_offset,
                                              bool unique,
                                              const Set<BMVert *, 0> &verts,
                                              MutableSpan<float> factors);

/* -------------------------------------------------------------------- */
/** \name Texture-as-data Face Set modes (#eBrushTextureDataMode)
 *
 * Centralized predicates for the "texture as data source" brush modes, so adding a new
 * #eBrushTextureDataMode only touches one place. \{ */

/** True when the brush samples its texture as a data source, not an intensity multiplier. */
bool brush_texture_data_mode_is_active(const Brush &brush);
/** Binary alpha-threshold mode (#BRUSH_TEXTURE_DATA_MODE_FACE_SETS_FROM_TEXTURE). */
bool brush_texture_data_mode_is_alpha(const Brush &brush);
/** Per-face RGB color mode (#BRUSH_TEXTURE_DATA_MODE_FACE_SETS_COLOR_FROM_TEXTURE). */
bool brush_texture_data_mode_is_color(const Brush &brush);
/** Color mode with an actual RGB texture assigned. */
bool brush_uses_color_texture(const Brush &brush);
/** Whether the stroke writes Face Set IDs (vs. #BRUSH_DISABLE_FACE_SET_WRITE). */
bool brush_texture_data_writes_face_sets(const Brush &brush);
/** Whether the stroke also writes sampled values into a color attribute. */
bool brush_texture_data_writes_color(const Brush &brush);

/**
 * Resolve the Face Set ID to paint for the current stroke into #StrokeCache.paint_face_set,
 * creating a new custom-colored Face Set if needed. No-op once resolved. Handles explicit
 * #Brush.face_set_id, inverted strokes, Custom color mode and Random mode.
 */
void ensure_stroke_face_set(Object &object, const Brush &brush);

/** \} */

void apply_from_texture(const Depsgraph &depsgraph,
                        Object &object,
                        const Brush &brush,
                        const IndexMask &node_mask);

void apply_from_color_texture(const Depsgraph &depsgraph,
                              Object &object,
                              const Brush &brush,
                              const IndexMask &node_mask);

/** Set face set colors through RNA so unified-color sync handlers are notified. */
void brush_face_set_color_set(Brush *brush, const float color[3]);
void brush_face_set_secondary_color_set(Brush *brush, const float color[3]);

}  // namespace ed::sculpt_paint::face_set

}  // namespace blender
